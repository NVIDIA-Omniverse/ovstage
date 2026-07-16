// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage sparse-write test: index_map (a gather over a subset of the
// query's prims) and mask (a bitmask over the target rows) each update only their
// targeted prims, leaving the rest unchanged. CPU-only. The write-flavors example
// is the workflow tour; this file asserts the sparse semantics.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <cstdint>
#include <cstring>

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

class SparseWriteTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.sparse";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        const ovx_string_t attrName = str("sparse-heat");
        ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, &attrName, 1, &heat_).status, OVX_API_SUCCESS);
        const ovx_string_t paths[] = { str("/World/Sparse/A"), str("/World/Sparse/B"), str("/World/Sparse/C"),
                                       str("/World/Sparse/D") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, paths, 4, &pathList_).status, OVX_API_SUCCESS);
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

    void seal(ovstage_ordinal_t ordinal)
    {
        ovstage_write_floor_desc_t floor{};
        floor.ordinal = ordinal;
        floor.scope = OVSTAGE_SCOPE_ALL;
        ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));
    }

    // Latest value per prim into out[list-index]. A latest read of a sparsely
    // updated column may arrive as several groups (rows last written at
    // different ordinals); each group's prim index is a list-relative index into
    // the query's path list (our A/B/C/D order).
    void readFour(ovstage_ordinal_t endOrdinal, float out[4])
    {
        for (int i = 0; i < 4; ++i)
            out[i] = -1.0f;
        ovstage_ordinal_range_t range{};
        range.end_ordinal = endOrdinal;
        ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
        ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &heat_, 1, range, &read), "read"));
        for (;;)
        {
            ovstage_read_group_t group{};
            const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group);
            if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
                break;
            if (fetched != OVSTAGE_OK)
            {
                ADD_FAILURE() << "fetch_read_next failed (code " << fetched << ")";
                break;
            }
            if (!group.is_delete && group.data.tensor_count == 1 && group.data.tensors[0].data)
            {
                const float* rows = static_cast<const float*>(group.data.tensors[0].data);
                for (uint32_t local = 0; local < group.prims.count; ++local)
                {
                    const size_t listIndex = group.prims.index_map ? group.prims.index_map[local]
                                                                   : group.prims.offset + local;
                    const uint32_t row = group.data.index_map ? group.data.index_map[local] : local;
                    if (listIndex < 4)
                        out[listIndex] = rows[row];
                }
            }
            ovstage_release_group(stage_, &group);
        }
        waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_token_t heat_ = OVX_INVALID_TOKEN;
    ovx_primpath_list_t pathList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

TEST_F(SparseWriteTest, IndexMapAndMask)
{
    int64_t four[] = { 4 };
    int64_t three[] = { 3 };
    int64_t strides[] = { 1 };
    auto tensor = [&](float* data, int64_t* shape) {
        DLTensor t{};
        t.data = data;
        t.device = { kDLCPU, 0 };
        t.ndim = 1;
        t.dtype = { kDLFloat, 32, 1 };
        t.shape = shape;
        t.strides = strides;
        return t;
    };

    float dense[] = { 10.0f, 20.0f, 30.0f, 40.0f };
    DLTensor denseTensor = tensor(dense, four);
    ovstage_write_data_t denseWrite{};
    denseWrite.tensors = &denseTensor;
    denseWrite.tensor_count = 1;
    denseWrite.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { heat_, {} }, 1, denseWrite, OVSTAGE_PRIM_MODE_UPSERT), "dense"));
    seal(1);
    float values[4] = {};
    readFour(1, values);
    EXPECT_FLOAT_EQ(values[0], 10.0f);
    EXPECT_FLOAT_EQ(values[3], 40.0f);

    // [snippet:sparse-index-map-and-mask-c]
    // index_map is a GATHER: count selects how many of the query's prims are
    // targeted (the first `count`, in query order) and index_map[i] picks the
    // SOURCE tensor row for target i. A non-involutory 3-cycle proves the direction:
    // gather reads source[index_map[i]] into target i (scatter would give a different
    // result). mask is a bitmask over the `count` target rows: only set bits are
    // written. Untouched prims keep their earlier value.
    float gathered[] = { 111.0f, 222.0f, 333.0f };
    const uint32_t indexMap[] = { 1, 2, 0 }; // A<-row1(222), B<-row2(333), C<-row0(111)
    DLTensor gatheredTensor = tensor(gathered, three);
    ovstage_write_data_t gatherWrite{};
    gatherWrite.tensors = &gatheredTensor;
    gatherWrite.tensor_count = 1;
    gatherWrite.is_array = false;
    gatherWrite.count = 3;
    gatherWrite.index_map = indexMap;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { heat_, {} }, 2, gatherWrite, OVSTAGE_PRIM_MODE_UPSERT), "gather"));
    seal(2);

    readFour(2, values);
    EXPECT_FLOAT_EQ(values[0], 222.0f);
    EXPECT_FLOAT_EQ(values[1], 333.0f);
    EXPECT_FLOAT_EQ(values[2], 111.0f);
    EXPECT_FLOAT_EQ(values[3], 40.0f);

    float masked[] = { 0.0f, 0.0f, 0.0f, 44.0f };
    const uint64_t maskBits[] = { 0b1000ull }; // bit 3 set -> only D (row 3)
    DLTensor maskedTensor = tensor(masked, four);
    ovstage_write_data_t maskWrite{};
    maskWrite.tensors = &maskedTensor;
    maskWrite.tensor_count = 1;
    maskWrite.is_array = false;
    maskWrite.count = 4;
    maskWrite.mask = maskBits;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { heat_, {} }, 3, maskWrite, OVSTAGE_PRIM_MODE_UPSERT), "mask"));
    seal(3);
    // [/snippet:sparse-index-map-and-mask-c]

    readFour(3, values);
    EXPECT_FLOAT_EQ(values[0], 222.0f);
    EXPECT_FLOAT_EQ(values[1], 333.0f);
    EXPECT_FLOAT_EQ(values[2], 111.0f);
    EXPECT_FLOAT_EQ(values[3], 44.0f);
}

} // namespace
