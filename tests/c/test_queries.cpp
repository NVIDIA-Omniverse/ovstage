// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage query/filter test: assert built-in metadata filters discover
// client-authored prims. CPU-only. The queries example is the workflow tour;
// this file holds the compact asserted snippets the stage-queries skill sources.

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_population.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <cmath>
#include <cstring>
#include <set>
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
        ADD_FAILURE() << what << " op failed: "
                      << std::string(e.ptr ? e.ptr : "", e.ptr ? e.length : 0);
    }
    ovstage_release_op(stage, enq.op_index);
    if (werr != OVSTAGE_OK)
        ADD_FAILURE() << what << " wait failed (code " << werr << ")";
    return werr == OVSTAGE_OK && wait.error_op_id_count == 0;
}

size_t queryPrimCount(ovstage_instance_t* stage, const ovstage_filter_t* filter, const ovx_token_t* attrs,
                      size_t attrCount, std::vector<ovx_token_t>* outAttrs = nullptr)
{
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    if (!waitOp(stage, ovstage_query(stage, filter, attrs, attrCount, &query), "query"))
        return 0;

    ovstage_query_result_t result{};
    if (ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result) != OVSTAGE_OK)
    {
        waitOp(stage, ovstage_release_query(stage, query), "release_query");
        return 0;
    }
    const size_t count = result.total_prim_count;
    if (outAttrs && result.attribute_count > 0 && result.attributes)
        outAttrs->assign(result.attributes, result.attributes + result.attribute_count);
    ovstage_release_query_result(stage, &result);
    waitOp(stage, ovstage_release_query(stage, query), "release_query");
    return count;
}

// Run one single-predicate filter query (string attribute, optional single
// string value) and return how many prims matched.
size_t runFilter(ovstage_instance_t* stage, const char* attrName, ovstage_filter_op_t op, const char* value)
{
    ovx_string_t val{};
    ovstage_predicate_t pred{};
    pred.attribute.string = str(attrName);
    pred.op = op;
    if (value)
    {
        val = str(value);
        pred.values = &val;
        pred.value_count = 1;
    }
    ovstage_filter_t filter{};
    filter.predicates = &pred;
    filter.count = 1;
    return queryPrimCount(stage, &filter, nullptr, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Write-only: usd-path IN and HAS on client-authored prims (no population).
// ─────────────────────────────────────────────────────────────────────────────
class QueryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.queries";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        const ovx_string_t attrName = str("temperature");
        ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, &attrName, 1, &attr_).status, OVX_API_SUCCESS);

        const ovx_string_t paths[] = { str("/World/MeshA"), str("/World/MeshB"), str("/World/Camera") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, paths, 3, &pathList_).status, OVX_API_SUCCESS);
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

TEST_F(QueryTest, QueryByUsdPathAndHasAttribute)
{
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
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { attr_, {} }, 1, write, OVSTAGE_PRIM_MODE_UPSERT),
        "write_attribute"));

    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 1;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));

    // [snippet:query-by-usd-path-c]
    ovx_string_t pathValue = str("/World/MeshB");
    ovstage_predicate_t pathPredicate{};
    pathPredicate.attribute.string = str("usd-path");
    pathPredicate.op = OVSTAGE_FILTER_OP_IN;
    pathPredicate.values = &pathValue;
    pathPredicate.value_count = 1;
    ovstage_filter_t pathFilter{};
    pathFilter.predicates = &pathPredicate;
    pathFilter.count = 1;
    EXPECT_EQ(queryPrimCount(stage_, &pathFilter, nullptr, 0), 1u);
    // [/snippet:query-by-usd-path-c]

    // [snippet:query-has-attribute-c]
    ovstage_predicate_t hasPredicate{};
    hasPredicate.attribute = { attr_, {} };
    hasPredicate.op = OVSTAGE_FILTER_OP_HAS;
    ovstage_filter_t hasFilter{};
    hasFilter.predicates = &hasPredicate;
    hasFilter.count = 1;
    std::vector<ovx_token_t> attrs;
    EXPECT_EQ(queryPrimCount(stage_, &hasFilter, &attr_, 1, &attrs), 3u);
    ASSERT_EQ(attrs.size(), 1u);
    EXPECT_EQ(attrs[0], attr_);
    // [/snippet:query-has-attribute-c]
}

// ─────────────────────────────────────────────────────────────────────────────
// Population-backed: the full predicate matrix + query introspection. Mirrors
// examples/c/queries (which prints these); here they are asserted.
// ─────────────────────────────────────────────────────────────────────────────
const char kQueryUsda[] = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Xform "Group"
    {
        def Mesh "Left"
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 0, 1)]
            uniform token subdivisionScheme = "none"
        }

        def Mesh "Right" (
            prepend apiSchemas = ["ShadowAPI"]
        )
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0, 0, 0), (-1, 0, 0), (0, 0, -1)]
            uniform token subdivisionScheme = "none"
        }
    }

    def Cube "Anchor"
    {
        double size = 1.0
    }

    def Xform "Prototype" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Looks/BoxMaterial>

        def Cube "Box"
        {
            double size = 1.0
        }
    }

    def Scope "Looks"
    {
        def Material "BoxMaterial"
        {
            token outputs:surface.connect = </World/Looks/BoxMaterial/Shader.outputs:surface>

            def Shader "Shader"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.6, 0.6, 0.6)
                token outputs:surface
            }
        }
    }

    def Xform "InstanceA" (
        instanceable = true
        prepend references = </World/Prototype>
    )
    {
    }

    def Xform "InstanceB" (
        instanceable = true
        prepend references = </World/Prototype>
    )
    {
    }
}

def Xform "Worldwide"
{
}
)";

class QueryMatrixTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.queries.matrix";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        ovstage_population_enqueue_result_t pop = ovstage_population_open_usd_from_string(
            stage_, str(kQueryUsda), /*ordinal=*/1, /*time=*/NAN, OVSTAGE_POPULATION_DOMAIN_ALL);
        ASSERT_EQ(pop.status, OVSTAGE_OK);
        ASSERT_EQ(ovstage_population_wait_op(stage_, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, nullptr), OVSTAGE_OK);

        ovstage_write_floor_desc_t floor{};
        floor.ordinal = 1;
        floor.scope = OVSTAGE_SCOPE_ALL;
        ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));

        const ovx_string_t tokenNames[] = { str("usd-prim-type"), str("example:count") };
        ovx_token_t tokens[2] = { OVX_INVALID_TOKEN, OVX_INVALID_TOKEN };
        ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, tokenNames, 2, tokens).status, OVX_API_SUCCESS);
        primType_ = tokens[0];
        count_ = tokens[1];

        // Write example:count to two prims so the HAS/introspection cases have targets.
        const ovx_string_t targets[] = { str("/World/Anchor"), str("/World/Group/Left") };
        ovx_primpath_list_t targetList = OVX_INVALID_PRIMPATH_LIST;
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, targets, 2, &targetList).status, OVX_API_SUCCESS);
        ovstage_query_handle_t targetQuery = OVSTAGE_INVALID_QUERY_HANDLE;
        ASSERT_EQ(ovstage_query_from_path_list(stage_, targetList, &targetQuery), OVSTAGE_OK);

        int32_t countValues[] = { 5, 3 };
        int64_t countShape[] = { 2 };
        int64_t countStrides[] = { 1 };
        DLTensor countTensor{};
        countTensor.data = countValues;
        countTensor.device = { kDLCPU, 0 };
        countTensor.ndim = 1;
        countTensor.dtype = { kDLInt, 32, 1 };
        countTensor.shape = countShape;
        countTensor.strides = countStrides;
        ovstage_write_data_t write{};
        write.tensors = &countTensor;
        write.tensor_count = 1;
        write.is_array = false;
        ASSERT_TRUE(waitOp(stage_,
            ovstage_write_attribute(stage_, targetQuery, { count_, {} }, 2, write, OVSTAGE_PRIM_MODE_UPSERT),
            "write example:count"));

        floor.ordinal = 2;
        ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));

        waitOp(stage_, ovstage_release_query(stage_, targetQuery), "release_query");
        path_dictionary_release_path_list_reference(dict_, targetList);
    }

    void TearDown() override
    {
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_token_t primType_ = OVX_INVALID_TOKEN;
    ovx_token_t count_ = OVX_INVALID_TOKEN;
};

TEST_F(QueryMatrixTest, PredicateMatrix)
{
    // [snippet:query-predicate-matrix-c]
    // HAS works on any attribute; the value operators pair only with the reserved
    // metadata built-ins. Predicates in one filter AND together; values are always
    // strings. Queries resolve against the latest committed state.
    EXPECT_EQ(runFilter(stage_, "usd-prim-type", OVSTAGE_FILTER_OP_IN, "Mesh"), 2u);
    EXPECT_EQ(runFilter(stage_, "usd-path", OVSTAGE_FILTER_OP_PREFIX, "/World/Group"), 3u);
    // PREFIX is byte-prefix: "/World" also matches "/Worldwide"; trailing "/" scopes.
    EXPECT_EQ(runFilter(stage_, "usd-path", OVSTAGE_FILTER_OP_PREFIX, "/Worldwide"), 1u);
    EXPECT_GT(runFilter(stage_, "usd-path", OVSTAGE_FILTER_OP_PREFIX, "/World"),
              runFilter(stage_, "usd-path", OVSTAGE_FILTER_OP_PREFIX, "/World/"));
    EXPECT_EQ(runFilter(stage_, "usd-parent", OVSTAGE_FILTER_OP_IN, "/World/Group"), 2u);
    EXPECT_EQ(runFilter(stage_, "usd-children", OVSTAGE_FILTER_OP_CONTAINS, "/World/Group/Left"), 1u);
    EXPECT_EQ(runFilter(stage_, "usd-schemas", OVSTAGE_FILTER_OP_CONTAINS, "ShadowAPI"), 1u);

    // HAS takes the interned token for a user attribute (no value test).
    ovstage_predicate_t hasPred{};
    hasPred.attribute = { count_, {} };
    hasPred.op = OVSTAGE_FILTER_OP_HAS;
    ovstage_filter_t hasFilter{};
    hasFilter.predicates = &hasPred;
    hasFilter.count = 1;
    EXPECT_EQ(queryPrimCount(stage_, &hasFilter, nullptr, 0), 2u);  // Anchor, Group/Left
    // [/snippet:query-predicate-matrix-c]

    // usd-active appears in the header contract but is not supported (a live
    // prim is always active, so it carries no information): any predicate
    // naming it is rejected at enqueue. Subject to removal in a future release.
    ovx_string_t activeValue = str("true");
    ovstage_predicate_t activePred{};
    activePred.attribute.string = str("usd-active");
    activePred.op = OVSTAGE_FILTER_OP_IN;
    activePred.values = &activeValue;
    activePred.value_count = 1;
    ovstage_filter_t activeFilter{};
    activeFilter.predicates = &activePred;
    activeFilter.count = 1;
    ovstage_query_handle_t rejected = OVSTAGE_INVALID_QUERY_HANDLE;
    EXPECT_EQ(ovstage_query(stage_, &activeFilter, nullptr, 0, &rejected).status,
              OVSTAGE_ERROR_NOT_SUPPORTED);
}

TEST_F(QueryMatrixTest, ResultIntrospection)
{
    // [snippet:query-result-introspection-c]
    // Scoping the query's attrs to named tokens keeps the reported attribute list
    // deterministic; all_handle is the same query handle echoed into the result.
    ovstage_predicate_t hasPred{};
    hasPred.attribute = { count_, {} };
    hasPred.op = OVSTAGE_FILTER_OP_HAS;
    ovstage_filter_t filter{};
    filter.predicates = &hasPred;
    filter.count = 1;

    const ovx_token_t scoped[] = { count_, primType_ };
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ASSERT_TRUE(waitOp(stage_, ovstage_query(stage_, &filter, scoped, 2, &query), "query"));

    ovstage_query_result_t result{};
    ASSERT_EQ(ovstage_fetch_query_result(stage_, query, OVSTAGE_TIMEOUT_INFINITE, &result), OVSTAGE_OK);
    EXPECT_EQ(result.total_prim_count, 2u);
    std::set<ovx_token_t> reported(result.attributes, result.attributes + result.attribute_count);
    EXPECT_EQ(reported, (std::set<ovx_token_t>{ count_, primType_ }));
    EXPECT_EQ(result.all_handle, query);
    ovstage_release_query_result(stage_, &result);
    // [/snippet:query-result-introspection-c]

    waitOp(stage_, ovstage_release_query(stage_, query), "release_query");
}

} // namespace
