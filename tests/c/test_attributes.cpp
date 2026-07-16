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

        const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage_, *read, OVSTAGE_TIMEOUT_INFINITE, group);
        if (fetched != OVSTAGE_OK)
        {
            waitOp(stage_, ovstage_release_read(stage_, *read), "release_read");
            *read = OVSTAGE_INVALID_READ_HANDLE;
            return ::testing::AssertionFailure() << "fetch_read_next failed (code " << fetched << ")";
        }

        *semantic = group->semantic;
        return ::testing::AssertionSuccess();
    }

    void releaseRead(ovstage_read_group_t* group, ovstage_read_handle_t read)
    {
        ovstage_release_group(stage_, group);
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
    // reads recover it (group.semantic). TOKEN_ID pins uint64 storage whose payload
    // is pre-interned path-dictionary token ids — ovstage never stringifies.
    float pointData[] = { 0, 0, 1, 0, 1, 0 };
    float colorData[] = { 1, 0, 0, 0, 0.5f, 0 };
    double matrixData[32];
    for (int i = 0; i < 32; ++i)
        matrixData[i] = ((i % 16) % 5 == 0) ? 1.0 : 0.0; // two identity mat4s (diagonal within each 4x4)
    uint64_t materialIds[] = { intern("steel"), intern("rubber") };

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
    const bool hasMatrixTensor = group.data.tensor_count > 0u;
    const uint16_t matrixLanes = hasMatrixTensor ? group.data.tensors[0].dtype.lanes : 0u;
    releaseRead(&group, read);
    EXPECT_EQ(semantic, OVSTAGE_SEMANTIC_MATRIX);
    ASSERT_TRUE(hasMatrixTensor);
    EXPECT_EQ(matrixLanes, 16u);

    ASSERT_TRUE(readSemantic(material, &group, &read, &semantic));
    const bool hasMaterialTensor = group.data.tensor_count > 0u && group.data.tensors[0].data;
    std::set<std::string> resolved;
    bool resolvedIds = true;
    if (hasMaterialTensor)
    {
        const uint64_t* ids = static_cast<const uint64_t*>(group.data.tensors[0].data);
        for (int i = 0; i < 2; ++i)
        {
            const ovx_token_t token = static_cast<ovx_token_t>(ids[i]);
            ovx_string_t name{};
            if (path_dictionary_get_strings_from_tokens(dict_, &token, 1, &name).status != OVX_API_SUCCESS)
            {
                resolvedIds = false;
                ADD_FAILURE() << "failed to resolve token id " << token;
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
