// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage attribute-shapes test: the write→read round-trip preserves the
// three fixed/ragged column shapes — a 1-lane scalar, a fixed multi-lane tuple
// (float3, lanes in the dtype not the shape), and a ragged per-prim array.
// CPU-only. The write-flavors example is the workflow tour; this file asserts it.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include <gtest/gtest.h>

namespace
{

ovx_string_t str(const char* s)
{
    return ovx_string_t{ s, std::strlen(s) };
}

bool waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq, const char* what)
{
    if (enq.status != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " enqueue rejected (code " << enq.status << ")";
        return false;
    }
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t werr = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    for (size_t i = 0; i < wait.error_op_id_count; ++i)
    {
        const ovx_string_t e = ovstage_get_last_op_error(stage, wait.error_op_ids[i]);
        ADD_FAILURE() << what << " op failed: " << std::string(e.ptr ? e.ptr : "", e.ptr ? e.length : 0);
    }
    ovstage_release_op(stage, enq.op_index);
    if (werr != OVSTAGE_OK)
        ADD_FAILURE() << what << " wait failed (code " << werr << ")";
    return werr == OVSTAGE_OK && wait.error_op_id_count == 0;
}

class ShapesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.shapes";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        const ovx_string_t paths[] = { str("/World/A"), str("/World/B"), str("/World/C") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, paths, 3, &pathList_).status, OVX_API_SUCCESS);
        ASSERT_EQ(ovstage_query_from_path_list(stage_, pathList_, &query_), OVSTAGE_OK);
    }

    void TearDown() override
    {
        if (stage_ && query_ != OVSTAGE_INVALID_QUERY_HANDLE)
            waitOp(stage_, ovstage_release_query(stage_, query_), "release_query");
        if (dict_ && pathList_ != OVX_INVALID_PRIMPATH_LIST)
            path_dictionary_release_path_list_reference(dict_, pathList_);
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    ovx_token_t intern(const char* name)
    {
        const ovx_string_t s = str(name);
        ovx_token_t token = OVX_INVALID_TOKEN;
        EXPECT_EQ(path_dictionary_create_tokens_from_strings(dict_, &s, 1, &token).status, OVX_API_SUCCESS);
        return token;
    }

    void seal(ovstage_ordinal_t ordinal)
    {
        ovstage_write_floor_desc_t floor{};
        floor.ordinal = ordinal;
        floor.scope = OVSTAGE_SCOPE_ALL;
        ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_primpath_list_t pathList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

TEST_F(ShapesTest, ScalarAndFixedLane)
{
    const ovx_token_t temperature = intern("temperature");
    const ovx_token_t velocity = intern("velocity");

    // [snippet:attribute-shapes-fixed-c]
    // A fixed-size column stacks one row per prim in ONE tensor: a scalar is a
    // 1-lane dtype; a float3 is the SAME write with a 3-lane dtype. The shape
    // stays {prim_count}; the tuple width lives in dtype.lanes.
    float temperatures[] = { 18.5f, 19.5f, 20.5f };
    float velocities[] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; // 3 prims x 3 lanes
    int64_t rows[] = { 3 };
    int64_t strides[] = { 1 };

    DLTensor tempTensor{};
    tempTensor.data = temperatures;
    tempTensor.device = { kDLCPU, 0 };
    tempTensor.ndim = 1;
    tempTensor.dtype = { kDLFloat, 32, 1 };
    tempTensor.shape = rows;
    tempTensor.strides = strides;

    DLTensor velTensor{};
    velTensor.data = velocities;
    velTensor.device = { kDLCPU, 0 };
    velTensor.ndim = 1;
    velTensor.dtype = { kDLFloat, 32, 3 };
    velTensor.shape = rows;
    velTensor.strides = strides;

    ovstage_write_data_t scalarWrite{};
    scalarWrite.tensors = &tempTensor;
    scalarWrite.tensor_count = 1;
    scalarWrite.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { temperature, {} }, 1, scalarWrite, OVSTAGE_PRIM_MODE_UPSERT),
        "write scalar"));

    ovstage_write_data_t vecWrite{};
    vecWrite.tensors = &velTensor;
    vecWrite.tensor_count = 1;
    vecWrite.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { velocity, {} }, 1, vecWrite, OVSTAGE_PRIM_MODE_UPSERT),
        "write vec3"));
    seal(1);
    // [/snippet:attribute-shapes-fixed-c]

    // Read the scalar column: one group, 3 rows, 1 lane.
    {
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 1;
        ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
        ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &temperature, 1, range, &read),
            "read scalar"));
        ovstage_read_group_t group{};
        ASSERT_EQ(ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group), OVSTAGE_OK);
        ASSERT_EQ(group.data.tensor_count, 1u);
        EXPECT_EQ(group.data.tensors[0].dtype.lanes, 1u);
        float out[3] = {};
        std::memcpy(out, group.data.tensors[0].data, sizeof(out));
        EXPECT_FLOAT_EQ(out[0], 18.5f);
        EXPECT_FLOAT_EQ(out[1], 19.5f);
        EXPECT_FLOAT_EQ(out[2], 20.5f);
        ovstage_release_group(stage_, &group);
        waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
    }

    // Read the fixed-lane column: one group, 3 rows x 3 lanes = 9 flat floats.
    {
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 1;
        ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
        ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &velocity, 1, range, &read),
            "read vec3"));
        ovstage_read_group_t group{};
        ASSERT_EQ(ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group), OVSTAGE_OK);
        ASSERT_EQ(group.data.tensor_count, 1u);
        EXPECT_EQ(group.data.tensors[0].dtype.lanes, 3u);
        float out[9] = {};
        std::memcpy(out, group.data.tensors[0].data, sizeof(out));
        const float expected[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        for (int i = 0; i < 9; ++i)
            EXPECT_FLOAT_EQ(out[i], expected[i]);
        ovstage_release_group(stage_, &group);
        waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
    }
}

TEST_F(ShapesTest, RaggedArray)
{
    const ovx_token_t samples = intern("samples");

    // [snippet:attribute-shapes-ragged-c]
    // is_array=true declares a ragged (variable-length per prim) column — never
    // inferred from the payload. With tensor_count > 1 each tensor is one prim's
    // row, so per-prim lengths may differ (2 / 3 / 1 here). Read groups mirror
    // that transport: one tensor per covered prim.
    float rowA[] = { 1.0f, 2.0f };
    float rowB[] = { 3.0f, 4.0f, 5.0f };
    float rowC[] = { 6.0f };
    int64_t lenA[] = { 2 }, lenB[] = { 3 }, lenC[] = { 1 };
    int64_t strides[] = { 1 };

    auto makeRow = [&](float* data, int64_t* shape) {
        DLTensor t{};
        t.data = data;
        t.device = { kDLCPU, 0 };
        t.ndim = 1;
        t.dtype = { kDLFloat, 32, 1 };
        t.shape = shape;
        t.strides = strides;
        return t;
    };
    DLTensor rowTensors[] = { makeRow(rowA, lenA), makeRow(rowB, lenB), makeRow(rowC, lenC) };

    ovstage_write_data_t raggedWrite{};
    raggedWrite.tensors = rowTensors;
    raggedWrite.tensor_count = 3;
    raggedWrite.is_array = true;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { samples, {} }, 1, raggedWrite, OVSTAGE_PRIM_MODE_UPSERT),
        "write ragged"));
    seal(1);
    // [/snippet:attribute-shapes-ragged-c]

    ovstage_ordinal_range_t range{};
    range.end_ordinal = 1;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &samples, 1, range, &read), "read ragged"));

    // Collect each covered prim's row keyed by length (order-independent): the
    // three ragged rows are 2/3/1 elements long. A latest read may arrive across
    // several groups, so iterate to END_OF_ITERATION.
    std::map<int64_t, std::vector<float>> byLength;
    for (;;)
    {
        ovstage_read_group_t group{};
        const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group);
        if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        ASSERT_EQ(fetched, OVSTAGE_OK);
        for (uint32_t local = 0; local < group.prims.count; ++local)
        {
            const uint32_t rowIdx = group.data.index_map ? group.data.index_map[local] : local;
            const DLTensor& t = group.data.tensors[rowIdx];
            byLength[t.shape[0]] = std::vector<float>(static_cast<const float*>(t.data),
                                                      static_cast<const float*>(t.data) + t.shape[0]);
        }
        ovstage_release_group(stage_, &group);
    }
    waitOp(stage_, ovstage_release_read(stage_, read), "release_read");

    ASSERT_EQ(byLength.size(), 3u);
    EXPECT_EQ(byLength[2], (std::vector<float>{ 1.0f, 2.0f }));
    EXPECT_EQ(byLength[3], (std::vector<float>{ 3.0f, 4.0f, 5.0f }));
    EXPECT_EQ(byLength[1], (std::vector<float>{ 6.0f }));
}

} // namespace
