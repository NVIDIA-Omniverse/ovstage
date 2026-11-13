// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage attribute-semantics test: a semantic (the authored USD meaning of
// a column's bytes) round-trips through the write→read cycle, orthogonal to the
// storage dtype; TOKEN_ID pins uint64 storage carrying pre-interned token ids.
// CPU-only. The write-flavors example is the workflow tour; this file asserts it.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>

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

class SemanticsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.semantics";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);
        const ovx_string_t paths[] = { str("/World/Semantics/A"), str("/World/Semantics/B") };
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

    ovx_token_t intern(const char* name)
    {
        const ovx_string_t s = str(name);
        ovx_token_t token = OVX_INVALID_TOKEN;
        EXPECT_EQ(path_dictionary_create_tokens_from_strings(dict_, &s, 1, &token).status, OVX_API_SUCCESS);
        return token;
    }

    void writeSemantic(ovx_token_t attr, DLTensor* tensor, ovstage_attribute_semantic_t semantic)
    {
        ovstage_write_data_t write{};
        write.tensors = tensor;
        write.tensor_count = 1;
        write.is_array = false;
        write.semantic = semantic;
        ASSERT_TRUE(waitOp(stage_,
            ovstage_write_attribute(stage_, query_, { attr, {} }, 1, write, OVSTAGE_PRIM_MODE_UPSERT), "write"));
    }

    ::testing::AssertionResult readSemantic(ovx_token_t attr, ovstage_read_group_t* group, ovstage_read_handle_t* read,
                                            ovstage_attribute_semantic_t* semantic)
    {
        size_t fetchedGroupCount = 0;
        *group = ovstage_read_group_t{};
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 1;
        *read = OVSTAGE_INVALID_READ_HANDLE;
        if (!waitOp(stage_, ovstage_read_attributes(stage_, query_, &attr, 1, range, read), "read"))
        {
            if (*read != OVSTAGE_INVALID_READ_HANDLE)
                waitOp(stage_, ovstage_release_read(stage_, *read), "release_read");
            *read = OVSTAGE_INVALID_READ_HANDLE;
            return ::testing::AssertionFailure() << "read_attributes failed";
        }

        const ovstage_api_status_t fetched =
            ovstage_fetch_read_next(stage_, *read, OVSTAGE_TIMEOUT_INFINITE, group, 1, &fetchedGroupCount);
        if (fetched != OVSTAGE_OK)
        {
            waitOp(stage_, ovstage_release_read(stage_, *read), "release_read");
            *read = OVSTAGE_INVALID_READ_HANDLE;
            return ::testing::AssertionFailure() << "fetch_read_next failed (code " << fetched << ")";
        }

        if (group->column_count != 1u || group->columns == nullptr)
        {
            const size_t columnCount = group->column_count;
            const void* columns = group->columns;
            ovstage_release_groups(stage_, group, 1);
            waitOp(stage_, ovstage_release_read(stage_, *read), "release_read");
            *read = OVSTAGE_INVALID_READ_HANDLE;
            return ::testing::AssertionFailure()
                   << "single-attribute read returned " << columnCount
                   << " columns with column array " << columns;
        }
        *semantic = group->columns[0].semantic;
        return ::testing::AssertionSuccess();
    }

    void releaseRead(ovstage_read_group_t* group, ovstage_read_handle_t read)
    {
        ovstage_release_groups(stage_, group, 1);
        waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_primpath_list_t pathList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

TEST_F(SemanticsTest, SemanticRolesRoundTrip)
{
    const ovx_token_t points = intern("points");
    const ovx_token_t color = intern("display-color");
    const ovx_token_t matrix = intern("local-matrix");
    const ovx_token_t material = intern("material");

    int64_t shape[] = { 2 };
    int64_t strides[] = { 1 };

    // [snippet:semantic-roles-c]
    // A semantic is the authored USD interpretation of a column's bytes, orthogonal
    // to the storage dtype: the same 3-lane float32 storage is POINT on one column
    // and COLOR on another; a 16-lane float64 is MATRIX. The write stamps it and
    // reads recover it (column.semantic). TOKEN_ID pins uint64 storage whose payload
    // is pre-interned path-dictionary token ids — ovstage never stringifies.
    float pointData[] = { 0, 0, 1, 0, 1, 0 };
    float colorData[] = { 1, 0, 0, 0, 0.5f, 0 };
    double matrixData[32];
    for (int i = 0; i < 32; ++i)
        matrixData[i] = ((i % 16) % 5 == 0) ? 1.0 : 0.0; // two identity mat4s (diagonal within each 4x4)
    ovx_token_t materialIds[] = { intern("steel"), intern("rubber") };

    DLTensor pointTensor{ pointData, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, shape, strides, 0 };
    DLTensor colorTensor{ colorData, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, shape, strides, 0 };
    DLTensor matrixTensor{ matrixData, { kDLCPU, 0 }, 1, { kDLFloat, 64, 16 }, shape, strides, 0 };
    DLTensor materialTensor{ materialIds, { kDLCPU, 0 }, 1, { kDLUInt, 64, 1 }, shape, strides, 0 };

    writeSemantic(points, &pointTensor, OVSTAGE_SEMANTIC_POINT);
    writeSemantic(color, &colorTensor, OVSTAGE_SEMANTIC_COLOR);
    writeSemantic(matrix, &matrixTensor, OVSTAGE_SEMANTIC_MATRIX);
    writeSemantic(material, &materialTensor, OVSTAGE_SEMANTIC_TOKEN_ID);

    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 1;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));
    // [/snippet:semantic-roles-c]

    ovstage_read_group_t group{};
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ovstage_attribute_semantic_t semantic{};
    ASSERT_TRUE(readSemantic(points, &group, &read, &semantic));
    EXPECT_EQ(semantic, OVSTAGE_SEMANTIC_POINT);
    releaseRead(&group, read);
    ASSERT_TRUE(readSemantic(color, &group, &read, &semantic));
    EXPECT_EQ(semantic, OVSTAGE_SEMANTIC_COLOR);
    releaseRead(&group, read);
    ASSERT_TRUE(readSemantic(matrix, &group, &read, &semantic));
    const ovstage_read_column_t& matrixColumn = group.columns[0];
    const bool hasMatrixTensor = matrixColumn.data.tensor_count > 0u;
    if (hasMatrixTensor)
    {
        const DLTensor& tensor = matrixColumn.data.tensors[0];
        EXPECT_EQ(tensor.dtype.lanes, 1u);
        ASSERT_EQ(tensor.ndim, 3);
        ASSERT_NE(tensor.shape, nullptr);
        EXPECT_EQ(tensor.shape[0], 2);
        EXPECT_EQ(tensor.shape[1], 4);
        EXPECT_EQ(tensor.shape[2], 4);
    }
    releaseRead(&group, read);
    EXPECT_EQ(semantic, OVSTAGE_SEMANTIC_MATRIX);
    ASSERT_TRUE(hasMatrixTensor);

    ASSERT_TRUE(readSemantic(material, &group, &read, &semantic));
    const ovstage_read_column_t& materialColumn = group.columns[0];
    const bool hasMaterialTensor = materialColumn.data.tensor_count > 0u && materialColumn.data.tensors[0].data;
    std::set<std::string> resolved;
    bool resolvedIds = true;
    if (hasMaterialTensor)
    {
        const auto* ids = reinterpret_cast<const uint64_t*>(
            static_cast<const uint8_t*>(materialColumn.data.tensors[0].data) + materialColumn.data.tensors[0].byte_offset);
        for (int i = 0; i < 2; ++i)
        {
            const ovx_token_t token{ ids[i] };
            ovx_string_t name{};
            if (path_dictionary_get_strings_from_tokens(dict_, &token, 1, &name).status != OVX_API_SUCCESS)
            {
                resolvedIds = false;
                ADD_FAILURE() << "failed to resolve token id " << token.raw_id;
                continue;
            }
            resolved.insert(std::string(name.ptr ? name.ptr : "", name.ptr ? name.length : 0));
        }
    }
    releaseRead(&group, read);
    EXPECT_EQ(semantic, OVSTAGE_SEMANTIC_TOKEN_ID);
    ASSERT_TRUE(hasMaterialTensor);
    ASSERT_TRUE(resolvedIds);
    EXPECT_EQ(resolved, (std::set<std::string>{ "steel", "rubber" }));  // order-independent
}

} // namespace

TEST_F(SemanticsTest, BatchedReadFetchAndRelease)
{
    const ovx_token_t attrs[] = { intern("batch:a"), intern("batch:b"), intern("batch:c"), intern("batch:d") };
    float values[] = { 1.0f, 2.0f };
    int64_t shape = 2;
    DLTensor tensor{ values, { kDLCPU, 0 }, 1, { kDLFloat, 32, 1 }, &shape, nullptr, 0 };
    for (auto attr : attrs)
        writeSemantic(attr, &tensor, OVSTAGE_SEMANTIC_NONE);
    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 1;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "seal"));
    ovstage_ordinal_range_t range{};
    range.end_ordinal = 1;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, attrs, 4, range, &read), "read"));

    size_t totalGroups = 0;
    size_t totalValues = 0;
    float sum = 0.0f;
    // [snippet:batched-read-groups-c]
    ovstage_read_group_t groups[3]{};
    for (;;)
    {
        size_t count = 0;
        const auto status = ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, groups, 3, &count);
        if (status == OVSTAGE_ERROR_END_OF_ITERATION)
        {
            EXPECT_EQ(count, 0u);
            break;
        }
        ASSERT_EQ(status, OVSTAGE_OK);
        ASSERT_GT(count, 0u);
        ASSERT_LE(count, 3u);
        for (size_t i = 0; i < count; ++i)
        {
            for (size_t c = 0; c < groups[i].column_count; ++c)
            {
                const auto& column = groups[i].columns[c];
                const auto& tensor = column.data.tensors[0];
                const auto* data =
                    reinterpret_cast<const float*>(static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
                for (size_t row = 0; row < groups[i].prims.count; ++row)
                {
                    const size_t index = column.data.index_map ? column.data.index_map[row] : row;
                    sum += data[index]; // Finish using borrowed values before release.
                    ++totalValues;
                }
            }
        }
        totalGroups += count;
        ASSERT_EQ(ovstage_release_groups(stage_, groups, count), OVSTAGE_OK);
        // Even a short batch needs another fetch to observe completion.
    }
    ASSERT_TRUE(waitOp(stage_, ovstage_release_read(stage_, read), "release_read"));
    // [/snippet:batched-read-groups-c]
    EXPECT_GT(totalGroups, 0u);
    EXPECT_EQ(totalValues, 8u);
    EXPECT_FLOAT_EQ(sum, 12.0f);
}
