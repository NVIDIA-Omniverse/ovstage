// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage delete test: a named-attribute delete tombstones just that column
// on the target prim; an empty attribute list tombstones the prim entirely. A HAS
// filter query (latest committed state) shows the prim set shrinking. CPU-only.

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

class DeleteTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.delete";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        const ovx_string_t attrName = str("del-heat");
        ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, &attrName, 1, &heat_).status, OVX_API_SUCCESS);

        const ovx_string_t both[] = { str("/World/Del/A"), str("/World/Del/B") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, both, 2, &bothList_).status, OVX_API_SUCCESS);
        ASSERT_EQ(ovstage_query_from_path_list(stage_, bothList_, &query_), OVSTAGE_OK);
        const ovx_string_t a[] = { str("/World/Del/A") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, a, 1, &aList_).status, OVX_API_SUCCESS);
        ASSERT_EQ(ovstage_query_from_path_list(stage_, aList_, &aQuery_), OVSTAGE_OK);
        const ovx_string_t b[] = { str("/World/Del/B") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, b, 1, &bList_).status, OVX_API_SUCCESS);
        ASSERT_EQ(ovstage_query_from_path_list(stage_, bList_, &bQuery_), OVSTAGE_OK);
    }

    void TearDown() override
    {
        for (ovstage_query_handle_t* q : { &bQuery_, &aQuery_, &query_ })
            if (stage_ && *q != OVSTAGE_INVALID_QUERY_HANDLE)
                waitOp(stage_, ovstage_release_query(stage_, *q), "release_query");
        if (dict_)
        {
            for (ovx_primpath_list_t* l : { &bList_, &aList_, &bothList_ })
                if (*l != OVX_INVALID_PRIMPATH_LIST)
                    path_dictionary_release_path_list_reference(dict_, *l);
        }
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

    size_t hasCount()
    {
        ovstage_predicate_t pred{};
        pred.attribute = { heat_, {} };
        pred.op = OVSTAGE_FILTER_OP_HAS;
        ovstage_filter_t filter{};
        filter.predicates = &pred;
        filter.count = 1;
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        if (!waitOp(stage_, ovstage_query(stage_, &filter, nullptr, 0, &query), "has-query"))
            return 0;
        ovstage_query_result_t result{};
        if (ovstage_fetch_query_result(stage_, query, OVSTAGE_TIMEOUT_INFINITE, &result) != OVSTAGE_OK)
        {
            waitOp(stage_, ovstage_release_query(stage_, query), "release_query");
            return 0;
        }
        const size_t count = result.total_prim_count;
        ovstage_release_query_result(stage_, &result);
        waitOp(stage_, ovstage_release_query(stage_, query), "release_query");
        return count;
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_token_t heat_ = OVX_INVALID_TOKEN;
    ovx_primpath_list_t bothList_ = OVX_INVALID_PRIMPATH_LIST;
    ovx_primpath_list_t aList_ = OVX_INVALID_PRIMPATH_LIST;
    ovx_primpath_list_t bList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_query_handle_t aQuery_ = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_query_handle_t bQuery_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

TEST_F(DeleteTest, DeleteAttributeThenPrim)
{
    float heats[] = { 1.0f, 2.0f };
    int64_t shape[] = { 2 };
    int64_t strides[] = { 1 };
    DLTensor tensor{};
    tensor.data = heats;
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
        ovstage_write_attribute(stage_, query_, { heat_, {} }, 1, write, OVSTAGE_PRIM_MODE_UPSERT), "write"));
    seal(1);
    EXPECT_EQ(hasCount(), 2u);

    // [snippet:delete-attribute-then-prim-c]
    // delete_attributes writes a tombstone: reads at or above the delete ordinal
    // no longer see the attribute. A named attribute list deletes just those
    // columns on the target prims; an EMPTY list (count 0) tombstones the prims
    // entirely. A HAS filter query shows the prim set shrinking.
    ovx_string_or_token_t deleteAttr{ heat_, {} };
    ASSERT_TRUE(waitOp(stage_, ovstage_delete_attributes(stage_, bQuery_, &deleteAttr, 1, 2), "delete attr on B"));
    seal(2);
    EXPECT_EQ(hasCount(), 1u); // only A still carries del-heat

    ASSERT_TRUE(waitOp(stage_, ovstage_delete_attributes(stage_, aQuery_, nullptr, 0, 3), "tombstone A"));
    seal(3);
    EXPECT_EQ(hasCount(), 0u);
    // [/snippet:delete-attribute-then-prim-c]
}

} // namespace
