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
// Public C contract for the opt-in history feature. An instance of
// "history_selective" turns history on for an exact (prim, attribute)
// selection: the selected pairs retain their earlier sealed values. The
// member `implementation` of ovstage_instance_desc_t with the name
// "history_selective" selects it. An instance of "history_latest" (the
// default) keeps history off and reports OVSTAGE_ERROR_NOT_SUPPORTED. Tested
// source of the history-enable-read-c snippet. CPU-only.

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_history.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <gtest/gtest.h>

#include <cstdint>
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
        ADD_FAILURE() << what << " enqueue rejected (code " << enq.status << "): "
                      << ovstage_get_error_string(stage, enq.status);
        return false;
    }
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t werr = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    if (werr == OVSTAGE_OK)
    {
        for (size_t i = 0; i < wait.error_op_id_count; ++i)
        {
            const ovx_string_t e = ovstage_get_last_op_error(stage, wait.error_op_ids[i]);
            ADD_FAILURE() << what << " op failed: " << std::string(e.ptr ? e.ptr : "", e.ptr ? e.length : 0);
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

void writeOne(ovstage_instance_t* stage, ovstage_query_handle_t query, ovx_token_t attr, float value, ovstage_ordinal_t ordinal)
{
    int64_t shape[] = { 1 };
    DLTensor tensor{};
    tensor.data = &value;
    tensor.device = { kDLCPU, 0 };
    tensor.ndim = 1;
    tensor.dtype = { kDLFloat, 32, 1 };
    tensor.shape = shape;
    ovstage_write_data_t write{};
    write.tensors = &tensor;
    write.tensor_count = 1;
    write.is_array = false;
    ASSERT_TRUE(waitOp(stage, ovstage_write_attribute(stage, query, { attr, {} }, ordinal, write, OVSTAGE_PRIM_MODE_UPSERT),
                       "write_attribute"));
}

void sealAll(ovstage_instance_t* stage, ovstage_ordinal_t ordinal)
{
    ovstage_write_floor_desc_t floor{};
    floor.ordinal = ordinal;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(stage, ovstage_advance_write_floor(stage, &floor), "advance_write_floor"));
}

// Read the single float of a one-prim query for `range`. Returns false when the
// read produced no value.
bool readOne(ovstage_instance_t* stage, ovstage_query_handle_t query, ovx_token_t attr, ovstage_ordinal_range_t range, float* out)
{
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    const ovstage_enqueue_result_t enq = ovstage_read_attributes(stage, query, &attr, 1, range, &read);
    if (enq.status != OVSTAGE_OK)
        return false;
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t werr = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    ovstage_release_op(stage, enq.op_index);
    bool found = false;
    if (werr == OVSTAGE_OK && wait.error_op_id_count == 0)
    {
        ovstage_read_group_t group{};
        size_t fetchedGroupCount = 0;
        while (ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group, 1, &fetchedGroupCount) ==
                   OVSTAGE_OK &&
               fetchedGroupCount == 1)
        {
            if (group.column_count == 1 && !group.columns[0].is_delete && group.columns[0].data.tensor_count == 1 &&
                group.columns[0].data.tensors && group.columns[0].data.tensors[0].data)
            {
                std::memcpy(out, group.columns[0].data.tensors[0].data, sizeof(float));
                found = true;
            }
            ovstage_release_groups(stage, &group, 1);
        }
    }
    waitOp(stage, ovstage_release_read(stage, read), "release_read");
    return found;
}

ovstage_ordinal_range_t latest(ovstage_ordinal_t end)
{
    ovstage_ordinal_range_t range{};
    range.end_ordinal = end;
    return range;
}

ovstage_ordinal_range_t fixed(ovstage_ordinal_t ordinal)
{
    ovstage_ordinal_range_t range{};
    range.start_ordinal = ordinal;
    range.end_ordinal = ordinal;
    range.has_start_ordinal = true;
    return range;
}

} // namespace

TEST(HistoryTest, DefaultInstanceRejectsSelection)
{
    ovstage_instance_desc_t desc{};
    desc.name = "test.ovstage.history.default";
    ovstage_instance_t* stage = nullptr;
    ASSERT_EQ(ovstage_create_instance(&desc, &stage), OVSTAGE_OK);
    path_dictionary_instance_t* dict = ovstage_get_path_dictionary(stage);
    ASSERT_NE(dict, nullptr);

    const ovx_string_t path = str("/World/A");
    ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
    ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict, &path, 1, &list).status, OVX_API_SUCCESS);
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ASSERT_EQ(ovstage_query_from_path_list(stage, list, &query), OVSTAGE_OK);

    ovx_token_or_string_t attribute{};
    attribute.string = str("temperature");
    EXPECT_EQ(ovstage_enable_history(stage, query, &attribute, 1), OVSTAGE_ERROR_NOT_SUPPORTED);
    EXPECT_EQ(ovstage_disable_history(stage, query, &attribute, 1), OVSTAGE_ERROR_NOT_SUPPORTED);

    waitOp(stage, ovstage_release_query(stage, query), "release_query");
    path_dictionary_release_path_list_reference(dict, list);
    ASSERT_EQ(ovstage_destroy_instance(stage), OVSTAGE_OK);
}

TEST(HistoryTest, SelectedPairRetainsEarlierSealedValues)
{
    float firstValue = 0.0f;
    float latestValue = 0.0f;

    // [snippet:history-enable-read-c]
    // Select the implementation "history_selective" in the descriptor of the
    // instance: history is on for the pairs that ovstage_enable_history
    // selects. The selection is per instance: an instance of "history_latest",
    // the default, in the same process keeps history off.
    ovstage_instance_desc_t desc{};
    desc.name = "test.ovstage.history.selected";
    desc.implementation = "history_selective";
    ovstage_instance_t* stage = nullptr;
    ASSERT_EQ(ovstage_create_instance(&desc, &stage), OVSTAGE_OK);
    path_dictionary_instance_t* dict = ovstage_get_path_dictionary(stage);

    const ovx_string_t path = str("/World/A");
    ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
    ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict, &path, 1, &list).status, OVX_API_SUCCESS);
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ASSERT_EQ(ovstage_query_from_path_list(stage, list, &query), OVSTAGE_OK);
    const ovx_string_t attrName = str("temperature");
    ovx_token_t attr = OVX_INVALID_TOKEN;
    ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict, &attrName, 1, &attr).status, OVX_API_SUCCESS);

    // Write and seal the first value, then select the exact (prim, attribute)
    // pair. The sealed value becomes the retained baseline.
    writeOne(stage, query, attr, 1.0f, /*ordinal*/ 1);
    sealAll(stage, 1);
    ovx_token_or_string_t selection{ attr, {} };
    ASSERT_EQ(ovstage_enable_history(stage, query, &selection, 1), OVSTAGE_OK);

    // A later sealed write keeps the earlier value readable for this pair.
    writeOne(stage, query, attr, 2.0f, /*ordinal*/ 2);
    sealAll(stage, 2);
    ASSERT_TRUE(readOne(stage, query, attr, fixed(1), &firstValue));
    ASSERT_TRUE(readOne(stage, query, attr, latest(2), &latestValue));

    // Disabling discards the retained history and keeps the live value.
    ASSERT_EQ(ovstage_disable_history(stage, query, &selection, 1), OVSTAGE_OK);

    waitOp(stage, ovstage_release_query(stage, query), "release_query");
    path_dictionary_release_path_list_reference(dict, list);
    ASSERT_EQ(ovstage_destroy_instance(stage), OVSTAGE_OK);
    // [/snippet:history-enable-read-c]

    EXPECT_FLOAT_EQ(firstValue, 1.0f);
    EXPECT_FLOAT_EQ(latestValue, 2.0f);
}
