// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage read-selection test: read_attributes accepts the attribute
// selector patterns "*" and "prefix*" (one '*', as the last character). Names
// that contain '*' are reserved for read selection, so writes and other
// operations reject them. CPU-only.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <cstdint>
#include <cstring>
#include <string>
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

class ReadPatternsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.read-patterns";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);
        const ovx_string_t paths[] = { str("/World/Patterns/A"), str("/World/Patterns/B") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, paths, 2, &pathList_).status, OVX_API_SUCCESS);
        ASSERT_EQ(ovstage_query_from_path_list(stage_, pathList_, &query_), OVSTAGE_OK);

        // Three float columns, written at ordinal 1 and sealed.
        for (const char* name : { "primvars:displayOpacity", "primvars:roughness", "mass" })
            writeFloats(intern(name), 1);
        ovstage_write_floor_desc_t floor{};
        floor.ordinal = 1;
        floor.scope = OVSTAGE_SCOPE_ALL;
        ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));
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

    std::string name(ovx_token_t token)
    {
        ovx_string_t s{};
        EXPECT_EQ(path_dictionary_get_strings_from_tokens(dict_, &token, 1, &s).status, OVX_API_SUCCESS);
        return std::string(s.ptr ? s.ptr : "", s.ptr ? s.length : 0);
    }

    ovstage_write_data_t floatWrite(float* values, int64_t* shape, DLTensor* tensor)
    {
        *tensor = DLTensor{};
        tensor->data = values;
        tensor->device = { kDLCPU, 0 };
        tensor->ndim = 1;
        tensor->dtype = { kDLFloat, 32, 1 };
        tensor->shape = shape;
        ovstage_write_data_t write{};
        write.tensors = tensor;
        write.tensor_count = 1;
        return write;
    }

    void writeFloats(ovx_token_t attribute, ovstage_ordinal_t ordinal)
    {
        float values[] = { 1.0f, 2.0f };
        int64_t shape[] = { 2 };
        DLTensor tensor{};
        ASSERT_TRUE(waitOp(stage_,
                           ovstage_write_attribute(stage_, query_, { attribute, {} }, ordinal,
                                                   floatWrite(values, shape, &tensor), OVSTAGE_PRIM_MODE_UPSERT),
                           "write_attribute"));
    }

    // Returns the write floor of one attribute.
    ovstage_ordinal_t writeFloor(const char* attribute)
    {
        ovstage_ordinal_query_handle_t handle = OVSTAGE_INVALID_ORDINAL_QUERY_HANDLE;
        const ovstage_enqueue_result_t enq = ovstage_get_attribute_write_floor(stage_, { intern(attribute), {} }, &handle);
        EXPECT_EQ(enq.status, OVSTAGE_OK);
        ovstage_ordinal_t floor = 0;
        EXPECT_EQ(ovstage_fetch_ordinal(stage_, handle, OVSTAGE_TIMEOUT_INFINITE, &floor), OVSTAGE_OK);
        waitOp(stage_, ovstage_release_ordinal_query(stage_, handle), "release_ordinal_query");
        return floor;
    }

    // Reads the selectors and returns the attribute name of each group, in fetch order.
    std::vector<std::string> readNames(const std::vector<const char*>& selectors, ovstage_ordinal_range_t range)
    {
        std::vector<ovx_token_t> tokens;
        for (const char* selector : selectors)
            tokens.push_back(intern(selector));
        ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
        std::vector<std::string> names;
        if (!waitOp(stage_, ovstage_read_attributes(stage_, query_, tokens.data(), tokens.size(), range, &read),
                    "read_attributes"))
            return names;
        while (true)
        {
            size_t fetchedBatchCount = 0;
            ovstage_read_group_t group{};
            const ovstage_api_status_t status = ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE,
                &group, 1, &fetchedBatchCount);
            if (status != OVSTAGE_OK)
            {
                EXPECT_EQ(status, OVSTAGE_ERROR_END_OF_ITERATION);
                break;
            }
            for (size_t i = 0; i < group.column_count; ++i)
                names.push_back(name(group.columns[i].attribute));
            ovstage_release_groups(stage_, &group, 1);
        }
        waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
        return names;
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_primpath_list_t pathList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

ovstage_ordinal_range_t latest(ovstage_ordinal_t end)
{
    ovstage_ordinal_range_t range{};
    range.end_ordinal = end;
    return range;
}

ovstage_ordinal_range_t changes(ovstage_ordinal_t start, ovstage_ordinal_t end)
{
    ovstage_ordinal_range_t range{};
    range.start_ordinal = start;
    range.end_ordinal = end;
    range.has_start_ordinal = true;
    return range;
}

} // namespace

TEST_F(ReadPatternsTest, PrefixPatternReadsMatchingAttributes)
{
    // [snippet:read-attribute-patterns-c]
    // "primvars:*" selects every attribute whose name starts with "primvars:".
    // A latest read returns the current value of each matched attribute.
    const ovx_token_t selector = intern("primvars:*");
    ovstage_ordinal_range_t range{};
    range.end_ordinal = 1;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &selector, 1, range, &read), "read"));

    std::vector<std::string> matched;
    size_t fetchedBatchCount = 0;
    ovstage_read_group_t group{};
    ovstage_api_status_t status;
    while ((status = ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE,
        &group, 1, &fetchedBatchCount)) == OVSTAGE_OK)
    {
        // Each group can hold several columns over the same prim rows.
        for (size_t i = 0; i < group.column_count; ++i)
            matched.push_back(name(group.columns[i].attribute));
        ovstage_release_groups(stage_, &group, 1);
    }
    // END_OF_ITERATION is the normal end. Any other status is an error.
    EXPECT_EQ(status, OVSTAGE_ERROR_END_OF_ITERATION);
    waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
    // [/snippet:read-attribute-patterns-c]

    EXPECT_EQ(matched, (std::vector<std::string>{ "primvars:displayOpacity", "primvars:roughness" }));
}

TEST_F(ReadPatternsTest, ExplicitNamesComeFirstAndPatternsAddEachNameOnce)
{
    for (const ovstage_ordinal_range_t& range : { latest(1), changes(1, 1) })
    {
        // Explicit names first, in caller order. Then the names that patterns
        // add, sorted.
        EXPECT_EQ(readNames({ "primvars:*", "mass" }, range),
                  (std::vector<std::string>{ "mass", "primvars:displayOpacity", "primvars:roughness" }));
        // A pattern does not add a name again.
        EXPECT_EQ(readNames({ "primvars:roughness", "primvars:*", "primvars:r*" }, range),
                  (std::vector<std::string>{ "primvars:roughness", "primvars:displayOpacity" }));
        // Repeated explicit names are deduplicated by exact token.
        EXPECT_EQ(readNames({ "mass", "mass" }, range), (std::vector<std::string>{ "mass" }));
        // A pattern that matches nothing gives no groups.
        EXPECT_TRUE(readNames({ "colors:*" }, range).empty());
    }
    // A range read with "*" returns the changed attributes.
    EXPECT_EQ(readNames({ "*" }, changes(1, 1)),
              (std::vector<std::string>{ "mass", "primvars:displayOpacity", "primvars:roughness" }));
}

TEST_F(ReadPatternsTest, InvalidSelectorFormsFailTheRead)
{
    for (const char* selector : { "*mass", "a*b", "**", "primvars:**", "*:*" })
    {
        SCOPED_TRACE(selector);
        const ovx_token_t token = intern(selector);
        ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
        EXPECT_EQ(ovstage_read_attributes(stage_, query_, &token, 1, latest(1), &read).status,
                  OVSTAGE_ERROR_INVALID_ARGUMENT);
        EXPECT_EQ(read, OVSTAGE_INVALID_READ_HANDLE);
    }
}

TEST_F(ReadPatternsTest, OtherOperationsRejectNamesWithStar)
{
    float values[] = { 5.0f, 6.0f };
    int64_t shape[] = { 2 };
    DLTensor tensor{};
    const ovstage_write_data_t write = floatWrite(values, shape, &tensor);
    for (const char* reserved : { "*", "primvars:*", "a*b" })
    {
        SCOPED_TRACE(reserved);
        const ovx_token_or_string_t byName{ OVX_INVALID_TOKEN, str(reserved) };
        EXPECT_EQ(ovstage_write_attribute(stage_, query_, byName, 2, write, OVSTAGE_PRIM_MODE_UPSERT).status,
                  OVSTAGE_ERROR_INVALID_ARGUMENT);
        EXPECT_EQ(ovstage_delete_attributes(stage_, query_, &byName, 1, 2).status, OVSTAGE_ERROR_INVALID_ARGUMENT);

        const ovx_token_t token = intern(reserved);
        ovstage_write_floor_desc_t floor{};
        floor.ordinal = 2;
        floor.scope = OVSTAGE_SCOPE_INCLUDE;
        const ovx_token_t floorAttributes[] = { intern("mass"), token };
        floor.attributes = floorAttributes;
        floor.attribute_count = 2;
        EXPECT_EQ(ovstage_advance_write_floor(stage_, &floor).status, OVSTAGE_ERROR_INVALID_ARGUMENT);
    }
    // No call did part of its work: the floor of "mass" did not move, and
    // ordinal 2 has no change.
    EXPECT_EQ(writeFloor("mass"), 1u);
    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 2;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));
    EXPECT_TRUE(readNames({ "*" }, changes(2, 2)).empty());
}
