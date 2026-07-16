// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage error-handling test: assert clone's create-only contract fails
// a second clone onto an already-existing target, and that the op error names
// the reason. CPU-only.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <gtest/gtest.h>

#include <cstring>
#include <string>

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
        ADD_FAILURE() << what << " op failed: "
                      << std::string(e.ptr ? e.ptr : "", e.ptr ? e.length : 0);
    }
    ovstage_release_op(stage, enq.op_index);
    if (werr != OVSTAGE_OK)
        ADD_FAILURE() << what << " wait failed (code " << werr << ")";
    return werr == OVSTAGE_OK && wait.error_op_id_count == 0;
}

// Drive an enqueue expected to FAIL, capturing the first per-op error message.
// A clone failure collapses to a failed op (the wait may still return OK); the
// reason is carried in the op error string. Returns true iff the op failed.
bool opFailed(ovstage_instance_t* stage, ovstage_enqueue_result_t enq, std::string& outError)
{
    outError.clear();
    if (enq.status != OVSTAGE_OK)
    {
        const char* s = ovstage_get_error_string(stage, enq.status);
        outError = s ? s : "";
        return true;
    }
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t werr = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    const bool failed = werr != OVSTAGE_OK || wait.error_op_id_count > 0;
    // The first failed id carries the root-cause message; if the wait itself
    // failed without naming one, fall back to the waited op (mirrors the Python
    // binding's _wait_and_release).
    if (failed)
    {
        const ovstage_op_id_t failedId = wait.error_op_id_count > 0 ? wait.error_op_ids[0] : enq.op_index;
        const ovx_string_t e = ovstage_get_last_op_error(stage, failedId);
        outError.assign(e.ptr ? e.ptr : "", e.ptr ? e.length : 0);
    }
    ovstage_release_op(stage, enq.op_index);
    return failed;
}

class ErrorHandlingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.error";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        const ovx_string_t attrName = str("temperature");
        ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, &attrName, 1, &attr_).status, OVX_API_SUCCESS);

        const ovx_string_t paths[] = { str("/World/Source") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, paths, 1, &pathList_).status, OVX_API_SUCCESS);
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

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_token_t attr_ = OVX_INVALID_TOKEN;
    ovx_primpath_list_t pathList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

TEST_F(ErrorHandlingTest, CloneToExistingTargetFails)
{
    // Establish the clone source (a write creates a queryable prim) and seal it.
    float value = 1.0f;
    int64_t shape[] = { 1 };
    int64_t strides[] = { 1 };
    DLTensor tensor{};
    tensor.data = &value;
    tensor.device = { kDLCPU, 0 };
    tensor.ndim = 1;
    tensor.dtype = { kDLFloat, 32, 1 };
    tensor.shape = shape;
    tensor.strides = strides;

    ovstage_write_data_t write{};
    write.tensors = &tensor;
    write.tensor_count = 1;
    write.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { attr_, {} }, 1, write, OVSTAGE_PRIM_MODE_UPSERT),
        "write_attribute"));

    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 1;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));

    // [snippet:clone-target-exists-error-c]
    // Clone targets are create-only. The first clone to a fresh path succeeds; a
    // second clone onto the now-existing target fails. The op-level failure
    // collapses to a failed op whose error string names the reason.
    const ovx_string_t target[] = { str("/World/Target") };
    ASSERT_TRUE(waitOp(stage_, ovstage_clone(stage_, str("/World/Source"), target, 1, 2), "clone"));

    std::string cloneError;
    EXPECT_TRUE(opFailed(stage_, ovstage_clone(stage_, str("/World/Source"), target, 1, 3), cloneError));
    EXPECT_NE(cloneError.find("already exists"), std::string::npos) << "reason: " << cloneError;
    // [/snippet:clone-target-exists-error-c]
}

} // namespace
