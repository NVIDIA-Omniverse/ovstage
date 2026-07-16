// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage write-mode test: UPSERT creates-or-updates; INSERT is create-only
// admission and rejects (synchronously, PRIM_NOT_FOUND) a write whose target prims
// already exist. CPU-only. The write-flavors example is the workflow tour.

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

DLTensor scalarTensor(float* data, int64_t* shape, int64_t* strides)
{
    DLTensor t{};
    t.data = data;
    t.device = { kDLCPU, 0 };
    t.ndim = 1;
    t.dtype = { kDLFloat, 32, 1 };
    t.shape = shape;
    t.strides = strides;
    return t;
}

class WriteModesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.write-modes";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        const ovx_string_t attrName = str("score");
        ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, &attrName, 1, &score_).status, OVX_API_SUCCESS);
        const ovx_string_t paths[] = { str("/World/Admission/A"), str("/World/Admission/B") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, paths, 2, &pathList_).status, OVX_API_SUCCESS);
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

    void readTwo(ovstage_ordinal_t endOrdinal, float out[2])
    {
        ovstage_ordinal_range_t range{};
        range.end_ordinal = endOrdinal;
        ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
        ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &score_, 1, range, &read), "read"));
        ovstage_read_group_t group{};
        const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group);
        if (fetched == OVSTAGE_OK)
        {
            if (group.data.tensor_count == 1u)
            {
                const DLTensor& tensor = group.data.tensors[0];
                if (tensor.data && tensor.ndim == 1 && tensor.shape && tensor.shape[0] >= 2 &&
                    tensor.dtype.code == kDLFloat && tensor.dtype.bits == 32 && tensor.dtype.lanes == 1)
                {
                    std::memcpy(out, tensor.data, sizeof(float) * 2);
                }
                else
                {
                    ADD_FAILURE() << "read expected one float32 1D tensor with at least two elements";
                }
            }
            else
            {
                ADD_FAILURE() << "read expected one tensor, got " << group.data.tensor_count;
            }
            ovstage_release_group(stage_, &group);
        }
        else
        {
            ADD_FAILURE() << "fetch_read_next failed (code " << fetched << ")";
        }
        waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_token_t score_ = OVX_INVALID_TOKEN;
    ovx_primpath_list_t pathList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

TEST_F(WriteModesTest, UpsertVsInsert)
{
    int64_t shape[] = { 2 };
    int64_t strides[] = { 1 };

    float created[] = { 1.0f, 2.0f };
    DLTensor createdTensor = scalarTensor(created, shape, strides);
    ovstage_write_data_t insertWrite{};
    insertWrite.tensors = &createdTensor;
    insertWrite.tensor_count = 1;
    insertWrite.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { score_, {} }, 1, insertWrite, OVSTAGE_PRIM_MODE_INSERT),
        "insert"));
    seal(1);

    float values[2] = {};
    readTwo(1, values);
    EXPECT_FLOAT_EQ(values[0], 1.0f);
    EXPECT_FLOAT_EQ(values[1], 2.0f);

    // [snippet:upsert-vs-insert-c]
    // With the target prims already present, INSERT rejects the write
    // (PRIM_NOT_FOUND, before anything is written). UPSERT updates present prims
    // and creates absent ones. The INSERT rejection is a synchronous preflight,
    // so the enqueue itself returns the error status.
    float rewrite[] = { 9.0f, 9.0f };
    DLTensor rewriteTensor = scalarTensor(rewrite, shape, strides);
    ovstage_write_data_t rewriteWrite{};
    rewriteWrite.tensors = &rewriteTensor;
    rewriteWrite.tensor_count = 1;
    rewriteWrite.is_array = false;
    const ovstage_enqueue_result_t rejected =
        ovstage_write_attribute(stage_, query_, { score_, {} }, 2, rewriteWrite, OVSTAGE_PRIM_MODE_INSERT);
    EXPECT_EQ(rejected.status, OVSTAGE_ERROR_PRIM_NOT_FOUND);

    float updated[] = { 10.0f, 20.0f };
    DLTensor updatedTensor = scalarTensor(updated, shape, strides);
    ovstage_write_data_t upsertWrite{};
    upsertWrite.tensors = &updatedTensor;
    upsertWrite.tensor_count = 1;
    upsertWrite.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { score_, {} }, 2, upsertWrite, OVSTAGE_PRIM_MODE_UPSERT),
        "upsert"));
    seal(2);
    // [/snippet:upsert-vs-insert-c]

    readTwo(2, values);
    EXPECT_FLOAT_EQ(values[0], 10.0f);
    EXPECT_FLOAT_EQ(values[1], 20.0f);
}

TEST_F(WriteModesTest, WriteBelowFloorRejected)
{
    int64_t shape[] = { 2 };
    int64_t strides[] = { 1 };

    // Seed the prims and advance the write floor to 5, sealing ordinals <= 5.
    float seed[] = { 1.0f, 2.0f };
    DLTensor seedTensor = scalarTensor(seed, shape, strides);
    ovstage_write_data_t seedWrite{};
    seedWrite.tensors = &seedTensor;
    seedWrite.tensor_count = 1;
    seedWrite.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { score_, {} }, 5, seedWrite, OVSTAGE_PRIM_MODE_UPSERT),
        "seed"));
    seal(5);

    // A write whose ordinal is at or below the write floor is rejected synchronously with
    // WRITE_FLOOR_VIOLATION, before anything is written: sealed ordinals can never be
    // mutated. Here ordinal 3 is below the floor (5).
    float stale[] = { 9.0f, 9.0f };
    DLTensor staleTensor = scalarTensor(stale, shape, strides);
    ovstage_write_data_t staleWrite{};
    staleWrite.tensors = &staleTensor;
    staleWrite.tensor_count = 1;
    staleWrite.is_array = false;
    const ovstage_enqueue_result_t rejected =
        ovstage_write_attribute(stage_, query_, { score_, {} }, 3, staleWrite, OVSTAGE_PRIM_MODE_UPSERT);
    EXPECT_EQ(rejected.status, OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION);

    // An ordinal above the floor is admitted.
    float fresh[] = { 10.0f, 20.0f };
    DLTensor freshTensor = scalarTensor(fresh, shape, strides);
    ovstage_write_data_t freshWrite{};
    freshWrite.tensors = &freshTensor;
    freshWrite.tensor_count = 1;
    freshWrite.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { score_, {} }, 6, freshWrite, OVSTAGE_PRIM_MODE_UPSERT),
        "above floor"));
    seal(6);

    float values[2] = {};
    readTwo(6, values);
    EXPECT_FLOAT_EQ(values[0], 10.0f);
    EXPECT_FLOAT_EQ(values[1], 20.0f);
}

} // namespace
