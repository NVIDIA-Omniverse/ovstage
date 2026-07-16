// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
// property and proprietary rights in and to this material, related
// documentation and any modifications thereto. Any use, reproduction,
// disclosure or distribution of this material and related documentation
// without an express license agreement from NVIDIA CORPORATION or
// its affiliates is strictly prohibited.
//
// First ovstage public C test (Phase 1 vertical slice). Unlike the minimal
// example — which prints the round-trip — this asserts it: write an attribute
// column, seal it by advancing the write floor, read it back, and check the
// values. It builds against the produced ovstage package (find_package(ovstage))
// and doubles as the tested source for the write-read-roundtrip-c snippet the
// skills reference. Keep the snippet markers intact (see
// public/tools/ci/validate_skills.py). CPU-only — no GPU required.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

namespace
{

// Non-owning ovx_string_t view over a C literal (len in bytes).
ovx_string_t str(const char* s)
{
    return ovx_string_t{ s, std::strlen(s) };
}

// Drive an async enqueue to completion: check the enqueue status, wait on the
// op id, then report any per-op errors. Enqueue success only means the op was
// accepted, not that it ran. Returns true iff the op completed error-free.
bool waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq, const char* what)
{
    if (enq.status != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " enqueue rejected (code " << enq.status << "): "
                      << ovstage_get_error_string(stage, enq.status);
        return false;
    }
    ovstage_op_wait_result_t wait{};
    ovstage_api_status_t werr = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    if (werr == OVSTAGE_OK)
    {
        for (size_t i = 0; i < wait.error_op_id_count; ++i)
        {
            const ovx_string_t e = ovstage_get_last_op_error(stage, wait.error_op_ids[i]);
            ADD_FAILURE() << what << " op failed: "
                          << std::string(e.ptr ? e.ptr : "", e.ptr ? e.length : 0);
        }
    }
    ovstage_release_op(stage, enq.op_index);
    if (werr != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " wait failed (code " << werr << ")";
        return false;
    }
    return wait.error_op_id_count == 0;
}

// A stage + its (instance-owned) path dictionary + one query over three prims,
// set up once per test and released in reverse in TearDown.
class MinimalTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.minimal";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        ASSERT_NE(stage_, nullptr);

        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        ovx_string_t attrName = str("temperature");
        ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, &attrName, 1, &attr_).status,
                  OVX_API_SUCCESS);

        const ovx_string_t paths[] = { str("/World/A"), str("/World/B"), str("/World/C") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, paths, 3, &pathList_).status,
                  OVX_API_SUCCESS);
        ASSERT_EQ(ovstage_query_from_path_list(stage_, pathList_, &query_), OVSTAGE_OK);
    }

    void TearDown() override
    {
        // Release every handle before destroying the instance (a public API
        // precondition), in reverse order of acquisition: query, then path list.
        if (stage_ && query_ != OVSTAGE_INVALID_QUERY_HANDLE)
            waitOp(stage_, ovstage_release_query(stage_, query_), "release_query");
        if (dict_ && pathList_ != OVX_INVALID_PRIMPATH_LIST)
            path_dictionary_release_path_list_reference(dict_, pathList_);
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_token_t attr_ = OVX_INVALID_TOKEN;
    ovx_primpath_list_t pathList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

// Write one float per prim, seal ordinal 1, read the column back, and assert the
// values survive the round-trip. This is the tested source of truth for the
// write-read-roundtrip-c snippet.
TEST_F(MinimalTest, WriteAdvanceRead)
{
    float out[3] = { 0.0f, 0.0f, 0.0f };

    // [snippet:write-read-roundtrip-c]
    // Write one float per prim into the "temperature" column at ordinal 1, seal
    // it by advancing the write floor to 1, then read the column back. Each
    // async enqueue is driven to completion with waitOp (checks enqueue status,
    // waits on the op id, reports per-op errors).
    float values[] = { 1.0f, 2.0f, 3.0f };
    int64_t shape[] = { 3 };
    int64_t strides[] = { 1 };
    DLTensor tensor{};
    tensor.data = values;
    tensor.device = { kDLCPU, 0 };
    tensor.ndim = 1;
    tensor.dtype = { kDLFloat, 32, 1 };
    tensor.shape = shape;
    tensor.strides = strides;

    ovstage_write_data_t write{};
    write.tensors = &tensor;
    write.tensor_count = 1;
    write.is_array = false;

    ovx_string_or_token_t attrArg{ attr_, {} };
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, attrArg, /*ordinal*/ 1, write, OVSTAGE_PRIM_MODE_UPSERT),
        "write_attribute"));

    ovstage_write_floor_desc_t writeFloor{};
    writeFloor.ordinal = 1;
    writeFloor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &writeFloor), "advance_write_floor"));

    ovstage_ordinal_range_t range{};
    range.end_ordinal = 1;
    range.has_start_ordinal = false;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &attr_, 1, range, &read),
        "read_attributes"));

    ovstage_read_group_t group{};
    ASSERT_EQ(ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group), OVSTAGE_OK);
    ASSERT_EQ(group.data.tensor_count, 1u);
    ASSERT_NE(group.data.tensors[0].data, nullptr);
    std::memcpy(out, group.data.tensors[0].data, sizeof(out));

    ovstage_release_group(stage_, &group);
    waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
    // [/snippet:write-read-roundtrip-c]

    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[1], 2.0f);
    EXPECT_FLOAT_EQ(out[2], 3.0f);
}

} // namespace
