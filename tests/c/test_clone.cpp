// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage clone test: write a source prim, clone to new targets, and
// assert the clones are queryable with copied values. CPU-only.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <gtest/gtest.h>

#include <cstring>

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

size_t queryPrimCount(ovstage_instance_t* stage, const ovstage_filter_t* filter)
{
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    if (!waitOp(stage, ovstage_query(stage, filter, nullptr, 0, &query), "query"))
        return 0;

    ovstage_query_result_t result{};
    if (ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result) != OVSTAGE_OK)
    {
        waitOp(stage, ovstage_release_query(stage, query), "release_query");
        return 0;
    }
    const size_t count = result.total_prim_count;
    ovstage_release_query_result(stage, &result);
    waitOp(stage, ovstage_release_query(stage, query), "release_query");
    return count;
}

float readScalarAtPath(ovstage_instance_t* stage, path_dictionary_instance_t* dict, ovx_token_t attr,
                       const char* path, ovstage_ordinal_t endOrdinal)
{
    const ovx_string_t pathStr = str(path);
    ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
    if (path_dictionary_create_path_list_from_strings(dict, &pathStr, 1, &list).status != OVX_API_SUCCESS)
        return 0.0f;

    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    if (ovstage_query_from_path_list(stage, list, &query) != OVSTAGE_OK)
    {
        path_dictionary_release_path_list_reference(dict, list);
        return 0.0f;
    }

    ovstage_ordinal_range_t range{};
    range.end_ordinal = endOrdinal;
    range.has_start_ordinal = false;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    if (!waitOp(stage, ovstage_read_attributes(stage, query, &attr, 1, range, &read), "read_attributes"))
    {
        waitOp(stage, ovstage_release_query(stage, query), "release_query");
        path_dictionary_release_path_list_reference(dict, list);
        return 0.0f;
    }

    ovstage_read_group_t group{};
    float value = 0.0f;
    const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
    if (fetched == OVSTAGE_OK && group.data.tensor_count == 1u && group.data.tensors[0].data)
        std::memcpy(&value, group.data.tensors[0].data, sizeof(value));

    if (fetched == OVSTAGE_OK)
        ovstage_release_group(stage, &group);
    waitOp(stage, ovstage_release_read(stage, read), "release_read");
    waitOp(stage, ovstage_release_query(stage, query), "release_query");
    path_dictionary_release_path_list_reference(dict, list);
    return value;
}

class CloneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.clone";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        const ovx_string_t attrName = str("temperature");
        ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, &attrName, 1, &attr_).status, OVX_API_SUCCESS);

        const ovx_string_t paths[] = { str("/World/A") };
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

TEST_F(CloneTest, CloneSubtreeCopiesValues)
{
    // [snippet:clone-and-verify-c]
    float value = 42.0f;
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

    const ovx_string_t targets[] = { str("/World/A_env0"), str("/World/A_env1") };
    ASSERT_TRUE(waitOp(stage_, ovstage_clone(stage_, str("/World/A"), targets, 2, 2), "clone"));

    floor.ordinal = 2;
    ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));
    // [/snippet:clone-and-verify-c]

    for (const char* target : { "/World/A_env0", "/World/A_env1" })
    {
        ovx_string_t pathValue = str(target);
        ovstage_predicate_t predicate{};
        predicate.attribute.string = str("usd-path");
        predicate.op = OVSTAGE_FILTER_OP_IN;
        predicate.values = &pathValue;
        predicate.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &predicate;
        filter.count = 1;
        EXPECT_EQ(queryPrimCount(stage_, &filter), 1u);
        EXPECT_FLOAT_EQ(readScalarAtPath(stage_, dict_, attr_, target, 2), 42.0f);
    }
}

} // namespace
