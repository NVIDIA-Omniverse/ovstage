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
// ovstage queries (C): find prims with filter queries instead of explicit path
// lists, then inspect what a query found. Runs one filter query per supported
// predicate and maps the scene-graph-instancing structure of the populated
// scene.
//
// Run from this directory (loads ./queries.usda), or pass a scene path as argv[1].
// Expected output: see README.md. Snippet markers are referenced by the
// skills under ../../../skills/ -- keep them intact.

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_instancing.h>
#include <ovstage/ovstage_population.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <ovx/types.h> // literal_to_ovx_string
#include <dlpack/dlpack.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/ovstage_example_utils.h"

// What reading one column over a query yields: one entry per matched prim, in
// stage order (group order is an implementation detail). cells[i] is the raw
// column value backing paths[i], widened to 64 bits -- this example reads a
// uint64 token column (usd-prim-type) and an int32 column (example:count);
// the caller knows the dtype and casts back.
struct ColumnRows
{
    std::vector<std::string> paths;
    std::vector<uint64_t> cells;
};

// [snippet:resolve-matched-prims]
// Resolve prim handles back to path strings. Prim-path and token handles are
// dictionary-lifetime -- no per-handle release.
static std::string tokenName(path_dictionary_instance_t* dict, ovx_token_t token)
{
    ovx_string_t name{};
    ovx_api_result_t ovxResult = path_dictionary_get_strings_from_tokens(dict, &token, 1, &name);
    checkOvx(dict, ovxResult, "resolve-token");
    return std::string(name.ptr ? name.ptr : "", name.length);
}

// One "/World/..." string from one prim-path handle: decompose the path into its name tokens
// through the dictionary, then resolve each token.
static std::string resolvePath(path_dictionary_instance_t* dict, ovx_primpath_t path)
{
    ovx_token_t tokenBuffer[64] = {};
    ovx_token_t* tokensPerPath[1] = { nullptr };
    size_t numTokens[1] = { 0 };
    size_t numProcessed = 0;
    ovx_api_result_t ovxResult =
        path_dictionary_get_tokens_from_paths(dict, &path, 1, tokenBuffer, sizeof(tokenBuffer) / sizeof(tokenBuffer[0]),
                                              tokensPerPath, numTokens, &numProcessed);
    checkOvx(dict, ovxResult, "get_tokens_from_paths");
    if (numProcessed == 0 || numTokens[0] == 0 || !tokensPerPath[0])
    {
        std::fprintf(stderr, "could not decompose a prim path into tokens\n");
        std::exit(EXIT_FAILURE);
    }
    std::string result;
    for (size_t i = 0; i < numTokens[0]; ++i)
    {
        result.push_back('/');
        result += tokenName(dict, tokensPerPath[0][i]);
    }
    return result;
}

// Fetch every prim-path handle in a path list.
static std::vector<ovx_primpath_t> pathListHandles(path_dictionary_instance_t* dict, ovx_primpath_list_t list)
{
    size_t total = 0;
    ovx_api_result_t ovxResult = path_dictionary_get_num_paths_from_path_list(dict, list, &total);
    checkOvx(dict, ovxResult, "path-list-count");
    std::vector<ovx_primpath_t> handles(total);
    size_t fetched = 0;
    if (total > 0)
    {
        ovxResult = path_dictionary_get_paths_from_path_list(dict, list, 0, total, handles.data(), &fetched);
        checkOvx(dict, ovxResult, "path-list-paths");
    }
    handles.resize(fetched);
    return handles;
}

// The prim paths a read group covers: a group identifies its prims as a path-list handle plus
// offset/count (contiguous) or an index_map (sparse); the list handle is a BORROW owned by the
// read -- do not release it.
static std::vector<std::string> groupPaths(path_dictionary_instance_t* dict, const ovstage_prim_group_t& prims)
{
    const std::vector<ovx_primpath_t> handles = pathListHandles(dict, prims.list);
    std::vector<std::string> paths;
    for (uint32_t i = 0; i < prims.count; ++i)
    {
        const size_t index = prims.index_map ? prims.index_map[i] : prims.offset + i;
        if (index >= handles.size())
        {
            std::fprintf(stderr, "read group indexes past its path list\n");
            std::exit(EXIT_FAILURE);
        }
        paths.push_back(resolvePath(dict, handles[index]));
    }
    return paths;
}
// [/snippet:resolve-matched-prims]

// Read one column over a query as a snapshot at endOrdinal and return the result as-is:
// each matched prim's path and its raw column cell. The tensor row for logical prim i honors
// data.index_map when present (gather/reorder/dedup) and is the identity otherwise, in which
// case one tensor row backs each covered prim.
static ColumnRows readColumnRows(ovstage_instance_t* stage, path_dictionary_instance_t* dict,
                                 ovstage_query_handle_t query, ovx_token_t attribute, ovstage_ordinal_t endOrdinal)
{
    ovstage_ordinal_range_t range{};
    range.end_ordinal = endOrdinal;
    range.has_start_ordinal = false;

    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_read_attributes(stage, query, &attribute, 1, range, &read);
    waitOp(stage, enq, "read_attributes");

    ColumnRows rows;
    for (;;)
    {
        ovstage_read_group_t group{};
        const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
        if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        check(stage, fetched, "fetch_read_next");

        const std::vector<std::string> paths = groupPaths(dict, group.prims);
        if (group.data.tensor_count != 1 || !group.data.tensors || !group.data.tensors[0].data ||
            group.data.tensors[0].ndim < 1)
        {
            std::fprintf(stderr, "unexpected read-group layout (want one tensor row per covered prim)\n");
            std::exit(EXIT_FAILURE);
        }
        const DLTensor& tensor = group.data.tensors[0];
        const size_t rowCount = group.data.index_map ? static_cast<size_t>(group.data.count) :
                                                       static_cast<size_t>(tensor.shape[0]);
        if (rowCount != paths.size())
        {
            std::fprintf(stderr, "unexpected read-group layout (want one tensor row per covered prim)\n");
            std::exit(EXIT_FAILURE);
        }
        for (size_t i = 0; i < paths.size(); ++i)
        {
            const uint32_t row = group.data.index_map ? group.data.index_map[i] : static_cast<uint32_t>(i);
            const uint64_t cell = tensor.dtype.bits == 64 ?
                                      static_cast<const uint64_t*>(tensor.data)[row] :
                                      static_cast<uint64_t>(static_cast<const int32_t*>(tensor.data)[row]);
            rows.paths.push_back(paths[i]);
            rows.cells.push_back(cell);
        }
        ovstage_api_status_t status = ovstage_release_group(stage, &group);
        check(stage, status, "release_group");
    }
    enq = ovstage_release_read(stage, read);
    waitOp(stage, enq, "release_read");
    return rows;
}

// [snippet:filter-query]
// Run one single-predicate filter query end to end: enqueue the filter (predicates in one filter
// AND together; values are always strings), enumerate the matched prims by reading the reserved
// usd-prim-type column over the query, and release the query handle. No path list goes in --
// the stage finds the prims.
static void runFilterQuery(ovstage_instance_t* stage, path_dictionary_instance_t* dict,
                           ovx_string_or_token_t attribute, ovstage_filter_op_t op, const char* value,
                           ovx_token_t primType, ovstage_ordinal_t endOrdinal, const char* label)
{
    const ovx_string_t values[] = { { value, value ? std::strlen(value) : 0 } };
    ovstage_predicate_t pred{};
    pred.attribute = attribute;
    pred.op = op;
    if (value)
    {
        pred.values = values;
        pred.value_count = 1;
    }
    ovstage_filter_t filter{};
    filter.predicates = &pred;
    filter.count = 1;

    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_query(stage, &filter, nullptr, 0, &query);
    waitOp(stage, enq, label);

    ColumnRows rows = readColumnRows(stage, dict, query, primType, endOrdinal);

    enq = ovstage_release_query(stage, query);
    waitOp(stage, enq, "release_query");

    // Example plumbing: report the matched paths, sorted for deterministic output.
    std::sort(rows.paths.begin(), rows.paths.end());
    std::printf("%s -> %llu matched:", label, static_cast<unsigned long long>(rows.paths.size()));
    for (const std::string& p : rows.paths)
        std::printf(" %s", p.c_str());
    std::printf("\n");
}
// [/snippet:filter-query]

int main(int argc, char** argv)
{
    const char* scenePath = (argc > 1) ? argv[1] : "queries.usda";

    ovstage_instance_desc_t desc{};
    desc.name = "example.queries";
    ovstage_instance_t* stage = nullptr;
    ovstage_api_status_t status = ovstage_create_instance(&desc, &stage);
    check(nullptr, status, "create_instance");
    path_dictionary_instance_t* dict = getPathDictionary(stage);

    ovx_string_t attrNames[] = { literal_to_ovx_string("usd-prim-type"), literal_to_ovx_string("example:count") };
    ovx_token_t tokens[2] = { OVX_INVALID_TOKEN, OVX_INVALID_TOKEN };
    ovx_api_result_t ovxResult = path_dictionary_create_tokens_from_strings(dict, attrNames, 2, tokens);
    checkOvx(dict, ovxResult, "intern-tokens");
    const ovx_token_t primType = tokens[0];
    const ovx_token_t countTok = tokens[1]; // user attribute for the HAS / GT sections

    // ---- 1. populate the scene and list the populated prims ----
    // Populate everything (domain ALL) at ordinal 1 and seal it; NAN time means "default time".
    // The instanceable references land as XformInstance prims, mapped out in section 6.
    const ovx_string_t sceneStr{ scenePath, std::strlen(scenePath) };
    ovstage_population_enqueue_result_t popEnq = ovstage_population_open_usd_from_file(
        stage, sceneStr, /*ordinal*/ 1, /*time*/ NAN, OVSTAGE_POPULATION_DOMAIN_ALL);
    waitPop(stage, popEnq, "open_usd");

    ovstage_write_floor_desc_t floor1{};
    floor1.ordinal = 1;
    floor1.scope = OVSTAGE_SCOPE_ALL;
    ovstage_enqueue_result_t enq = ovstage_advance_write_floor(stage, &floor1);
    waitOp(stage, enq, "advance_write_floor");

    const ovx_string_t scene[] = { literal_to_ovx_string("/World"),
                                   literal_to_ovx_string("/World/Group"),
                                   literal_to_ovx_string("/World/Group/Left"),
                                   literal_to_ovx_string("/World/Group/Right"),
                                   literal_to_ovx_string("/World/Anchor"),
                                   literal_to_ovx_string("/World/Prototype"),
                                   literal_to_ovx_string("/World/Prototype/Box"),
                                   literal_to_ovx_string("/World/InstanceA"),
                                   literal_to_ovx_string("/World/InstanceB") };
    ovx_primpath_list_t scenePaths = OVX_INVALID_PRIMPATH_LIST;
    ovxResult = path_dictionary_create_path_list_from_strings(dict, scene, 9, &scenePaths);
    checkOvx(dict, ovxResult, "path-list");
    ovstage_query_handle_t sceneQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    status = ovstage_query_from_path_list(stage, scenePaths, &sceneQuery);
    check(stage, status, "query_from_path_list");

    const ColumnRows sceneRows = readColumnRows(stage, dict, sceneQuery, primType, /*endOrdinal*/ 1);

    // Example plumbing: the usd-prim-type cells are uint64 tokens -- resolve each
    // to its name and print "path = Type", sorted for deterministic output.
    std::vector<std::string> sceneLines;
    for (size_t i = 0; i < sceneRows.paths.size(); ++i)
        sceneLines.push_back(sceneRows.paths[i] + " = " +
                             tokenName(dict, static_cast<ovx_token_t>(sceneRows.cells[i])));
    std::sort(sceneLines.begin(), sceneLines.end());
    std::printf("populated prims (usd-prim-type):\n");
    for (const std::string& line : sceneLines)
        std::printf("  %s\n", line.c_str());

    // ---- 2. write example:count (filtered on in sections 3 and 4) ----
    // A user attribute (int32) on two prims, written at ordinal 2 and sealed. No query
    // here yet: the HAS filters in sections 3 and 4 match the prims this write touches.
    const ovx_string_t targets[] = { literal_to_ovx_string("/World/Anchor"),
                                     literal_to_ovx_string("/World/Group/Left") };
    ovx_primpath_list_t targetPaths = OVX_INVALID_PRIMPATH_LIST;
    ovxResult = path_dictionary_create_path_list_from_strings(dict, targets, 2, &targetPaths);
    checkOvx(dict, ovxResult, "path-list");
    ovstage_query_handle_t targetQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    status = ovstage_query_from_path_list(stage, targetPaths, &targetQuery);
    check(stage, status, "query_from_path_list");

    int32_t countValues[2] = { 5, 3 };
    int64_t countShape[] = { 2 };
    int64_t countStrides[] = { 1 };
    DLTensor countTensor{};
    countTensor.data = countValues;
    countTensor.device = { kDLCPU, 0 };
    countTensor.ndim = 1;
    countTensor.dtype = { kDLInt, 32, 1 };
    countTensor.shape = countShape;
    countTensor.strides = countStrides;
    enq = ovstage_write_attribute(stage, targetQuery, { countTok, {} }, /*ordinal*/ 2, writeData(&countTensor),
                                  OVSTAGE_PRIM_MODE_UPSERT);
    waitOp(stage, enq, "write_attribute");

    ovstage_write_floor_desc_t floor2{};
    floor2.ordinal = 2;
    floor2.scope = OVSTAGE_SCOPE_ALL;
    enq = ovstage_advance_write_floor(stage, &floor2);
    waitOp(stage, enq, "advance_write_floor");

    {
        // [snippet:query-prim-exists-c]
        // Match the exact path, not a prefix that could include other prims.
        const ovx_string_t path = literal_to_ovx_string("/World/Anchor");
        ovstage_predicate_t predicate{};
        predicate.attribute.string = literal_to_ovx_string("usd-path");
        predicate.op = OVSTAGE_FILTER_OP_IN;
        predicate.values = &path;
        predicate.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &predicate;
        filter.count = 1;

        // Wait for the query before inspecting its result.
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query prim existence");
        ovstage_query_result_t result{};
        check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
              "fetch_query_result");
        const bool primExists = result.total_prim_count != 0;
        std::printf("/World/Anchor exists: %s\n", primExists ? "True" : "False");
        // /World/Anchor exists: True

        // The result and query handle have separate lifetimes; release both.
        check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
        waitOp(stage, ovstage_release_query(stage, query), "release_query");
        // [/snippet:query-prim-exists-c]
    }

    {
        // [snippet:query-attribute-exists-c]
        // Both conditions must match: this path AND presence of this attribute.
        const ovx_string_t path = literal_to_ovx_string("/World/Anchor");
        ovstage_predicate_t predicates[2]{};
        predicates[0].attribute.string = literal_to_ovx_string("usd-path");
        predicates[0].op = OVSTAGE_FILTER_OP_IN;
        predicates[0].values = &path;
        predicates[0].value_count = 1;
        predicates[1].attribute.string = literal_to_ovx_string("example:count");
        predicates[1].op = OVSTAGE_FILTER_OP_HAS; // No comparison value is needed.
        ovstage_filter_t filter{};
        filter.predicates = predicates;
        filter.count = 2;

        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query attribute existence");
        ovstage_query_result_t result{};
        check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
              "fetch_query_result");
        const bool attributeExists = result.total_prim_count != 0;
        std::printf("/World/Anchor has example:count: %s\n", attributeExists ? "True" : "False");
        // /World/Anchor has example:count: True

        // Release the result and query even when no prim matched.
        check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
        waitOp(stage, ovstage_release_query(stage, query), "release_query");
        // [/snippet:query-attribute-exists-c]
    }

    {
        // [snippet:query-type-subtree-c]
        // Match Mesh prims below /World/Group; the trailing slash excludes the root.
        const ovx_string_t type = literal_to_ovx_string("Mesh");
        const ovx_string_t prefix = literal_to_ovx_string("/World/Group/");
        ovstage_predicate_t predicates[2]{};
        predicates[0].attribute.string = literal_to_ovx_string("usd-prim-type");
        predicates[0].op = OVSTAGE_FILTER_OP_IN;
        predicates[0].values = &type;
        predicates[0].value_count = 1;
        predicates[1].attribute.string = literal_to_ovx_string("usd-path");
        predicates[1].op = OVSTAGE_FILTER_OP_PREFIX;
        predicates[1].values = &prefix;
        predicates[1].value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = predicates;
        filter.count = 2;

        // Wait, inspect the match count, and release the result and query.
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query");
        ovstage_query_result_t result{};
        check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
              "fetch_query_result");
        std::printf("Meshes below /World/Group: %llu\n", static_cast<unsigned long long>(result.total_prim_count));
        // Meshes below /World/Group: 2
        check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
        waitOp(stage, ovstage_release_query(stage, query), "release_query");
        // [/snippet:query-type-subtree-c]
    }

    {
        // [snippet:query-direct-children-c]
        // Match the parent exactly; grandchildren have a different parent.
        const ovx_string_t parent = literal_to_ovx_string("/World/Group");
        ovstage_predicate_t predicate{};
        predicate.attribute.string = literal_to_ovx_string("usd-parent");
        predicate.op = OVSTAGE_FILTER_OP_IN;
        predicate.values = &parent;
        predicate.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &predicate;
        filter.count = 1;

        // Wait, inspect the match count, and release the result and query.
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query");
        ovstage_query_result_t result{};
        check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
              "fetch_query_result");
        std::printf("Direct children of /World/Group: %llu\n", static_cast<unsigned long long>(result.total_prim_count));
        // Direct children of /World/Group: 2
        check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
        waitOp(stage, ovstage_release_query(stage, query), "release_query");
        // [/snippet:query-direct-children-c]
    }

    {
        // [snippet:query-required-attributes-c]
        // Separate HAS predicates require both attributes, regardless of their values.
        ovstage_predicate_t predicates[2]{};
        predicates[0].attribute.string = literal_to_ovx_string("points");
        predicates[0].op = OVSTAGE_FILTER_OP_HAS;
        predicates[1].attribute.string = literal_to_ovx_string("faceVertexCounts");
        predicates[1].op = OVSTAGE_FILTER_OP_HAS;
        ovstage_filter_t filter{};
        filter.predicates = predicates;
        filter.count = 2;

        // Wait, inspect the match count, and release the result and query.
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query");
        ovstage_query_result_t result{};
        check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
              "fetch_query_result");
        std::printf("Prims with points and faceVertexCounts: %llu\n", static_cast<unsigned long long>(result.total_prim_count));
        // Prims with points and faceVertexCounts: 2
        check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
        waitOp(stage, ovstage_release_query(stage, query), "release_query");
        // [/snippet:query-required-attributes-c]
    }

    {
        // [snippet:query-matched-paths-c]
        // Query the direct children of /World/Group.
        const ovx_string_t parent = literal_to_ovx_string("/World/Group");
        ovstage_predicate_t predicate{};
        predicate.attribute.string = literal_to_ovx_string("usd-parent");
        predicate.op = OVSTAGE_FILTER_OP_IN;
        predicate.values = &parent;
        predicate.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &predicate;
        filter.count = 1;
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query paths");

        // usd-path contains prim-path identifiers resolved through the stage dictionary.
        const ovx_string_t name = literal_to_ovx_string("usd-path");
        ovx_token_t pathAttribute = OVX_INVALID_TOKEN;
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &pathAttribute),
                 "intern usd-path");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2; // No start ordinal: read current paths.
        ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
        waitOp(stage, ovstage_read_attributes(stage, query, &pathAttribute, 1, range, &read), "read paths");

        std::vector<std::string> matchedPaths;
        for (;;)
        {
            ovstage_read_group_t group{};
            const ovstage_api_status_t fetched =
                ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
            if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
                break;
            check(stage, fetched, "fetch_read_next");

            const DLTensor& tensor = group.data.tensors[0];
            const auto* pathIds = reinterpret_cast<const uint64_t*>(
                static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
            for (uint32_t i = 0; i < group.prims.count; ++i)
            {
                const uint32_t row = group.data.index_map ? group.data.index_map[i] : i;
                matchedPaths.push_back(resolvePath(dict, pathIds[row]));
            }
            check(stage, ovstage_release_group(stage, &group), "release_group");
        }
        waitOp(stage, ovstage_release_read(stage, read), "release_read");
        waitOp(stage, ovstage_release_query(stage, query), "release_query");

        std::sort(matchedPaths.begin(), matchedPaths.end());
        std::printf("Matched paths:");
        for (const std::string& path : matchedPaths)
            std::printf(" %s", path.c_str());
        std::printf("\n");
        // Matched paths: /World/Group/Left /World/Group/Right
        // [/snippet:query-matched-paths-c]
    }

    {
        // [snippet:query-applied-schema-c]
        // Select prims carrying this applied API schema.
        const ovx_string_t schema = literal_to_ovx_string("ShadowAPI");
        ovstage_predicate_t predicate{};
        predicate.attribute.string = literal_to_ovx_string("usd-schemas");
        predicate.op = OVSTAGE_FILTER_OP_CONTAINS;
        predicate.values = &schema;
        predicate.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &predicate;
        filter.count = 1;

        // Wait for the query before inspecting its result.
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query applied schema");
        ovstage_query_result_t result{};
        check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
              "fetch_query_result");
        std::printf("Prims with ShadowAPI: %llu\n",
                    static_cast<unsigned long long>(result.total_prim_count));
                    // Prims with ShadowAPI: 1

        // The result and query handle have separate lifetimes; release both.
        check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
        waitOp(stage, ovstage_release_query(stage, query), "release_query");
        // [/snippet:query-applied-schema-c]
    }

    {
        // [snippet:query-list-schemas-c]
        // Select the prim whose applied schemas we want to inspect.
        const ovx_string_t primPath = literal_to_ovx_string("/World/Group/Right");
        ovstage_predicate_t predicate{};
        predicate.attribute.string = literal_to_ovx_string("usd-path");
        predicate.op = OVSTAGE_FILTER_OP_IN;
        predicate.values = &primPath;
        predicate.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &predicate;
        filter.count = 1;
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query prim schemas");

        // usd-schemas contains an array of token identifiers for each prim.
        const ovx_string_t name = literal_to_ovx_string("usd-schemas");
        ovx_token_t schemaAttribute = OVX_INVALID_TOKEN;
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &schemaAttribute),
                 "intern usd-schemas");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2; // Snapshot at 2 (no start); use UINT64_MAX for current schemas.
        ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
        waitOp(stage, ovstage_read_attributes(stage, query, &schemaAttribute, 1, range, &read), "read schemas");

        std::vector<std::string> schemaNames;
        for (;;)
        {
            ovstage_read_group_t group{};
            const ovstage_api_status_t fetched =
                ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
            if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
                break;
            check(stage, fetched, "fetch_read_next");

            // Array attributes return a separate tensor for each prim.
            for (uint32_t i = 0; i < group.data.tensor_count; ++i)
            {
                const DLTensor& tensor = group.data.tensors[i];
                const auto* tokens = reinterpret_cast<const ovx_token_t*>(
                    static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
                for (int64_t j = 0; j < tensor.shape[0]; ++j)
                    schemaNames.push_back(tokenName(dict, tokens[j]));
            }
            check(stage, ovstage_release_group(stage, &group), "release_group");
        }
        waitOp(stage, ovstage_release_read(stage, read), "release_read");
        waitOp(stage, ovstage_release_query(stage, query), "release_query");

        std::sort(schemaNames.begin(), schemaNames.end());
        std::printf("APIs on /World/Group/Right:");
        for (const std::string& schemaName : schemaNames)
            std::printf(" %s", schemaName.c_str());
        std::printf("\n");
        // APIs on /World/Group/Right: MaterialBindingAPI MeshSubdivision ShadowAPI
        // [/snippet:query-list-schemas-c]
    }

    // ---- 3. one filter query per supported predicate ----
    // [snippet:filter-predicates]
    // The support matrix is narrow: HAS works on any attribute; the value operators only pair
    // with the reserved metadata built-ins shown here (the header describes more operators than
    // the current implementation accepts; anything else is rejected at enqueue). ShadowAPI is authored on /World/Group/Right only (population applies
    // schemas of its own, e.g. MaterialBindingAPI on every gprim, so filter on a schema that is
    // selective in your scene).
    const ovstage_ordinal_t endOrdinal = 2;
    runFilterQuery(stage, dict, { 0, literal_to_ovx_string("usd-prim-type") }, OVSTAGE_FILTER_OP_IN, "Mesh",
                   primType, endOrdinal, "usd-prim-type IN {Mesh}");
    runFilterQuery(stage, dict, { 0, literal_to_ovx_string("usd-path") }, OVSTAGE_FILTER_OP_PREFIX, "/World/Group",
                   primType, endOrdinal, "usd-path PREFIX {/World/Group}"); // subtree select
    runFilterQuery(stage, dict, { 0, literal_to_ovx_string("usd-parent") }, OVSTAGE_FILTER_OP_IN, "/World/Group",
                   primType, endOrdinal, "usd-parent IN {/World/Group}"); // direct children
    runFilterQuery(stage, dict, { 0, literal_to_ovx_string("usd-children") }, OVSTAGE_FILTER_OP_CONTAINS,
                   "/World/Group/Left", primType, endOrdinal, "usd-children CONTAINS {/World/Group/Left}");
    runFilterQuery(stage, dict, { 0, literal_to_ovx_string("usd-schemas") }, OVSTAGE_FILTER_OP_CONTAINS, "ShadowAPI",
                   primType, endOrdinal, "usd-schemas CONTAINS {ShadowAPI}"); // applied-schema membership
    runFilterQuery(stage, dict, { countTok, {} }, OVSTAGE_FILTER_OP_HAS, /*value*/ nullptr, primType, endOrdinal,
                   "HAS example:count"); // presence, no values; an interned token works too
    // [/snippet:filter-predicates]

    // ---- 4. query introspection ----
    // [snippet:query-introspection]
    // fetch_query_result reports what a query found. Scoping the attrs argument to named tokens
    // keeps the reported attribute list deterministic (without it the result reports every
    // column discovered on the matched prims). all_handle is the same query handle echoed back
    // into the result, so a consumer handed only the ovstage_query_result_t can still read from
    // the matched set. release_query_result frees the payload; the query handle outlives it.
    ovstage_predicate_t hasPred{};
    hasPred.attribute = ovx_string_or_token_t{ countTok, {} };
    hasPred.op = OVSTAGE_FILTER_OP_HAS;
    ovstage_filter_t hasFilter{};
    hasFilter.predicates = &hasPred;
    hasFilter.count = 1;
    const ovx_token_t scopedAttrs[] = { countTok, primType };
    ovstage_query_handle_t introQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    enq = ovstage_query(stage, &hasFilter, scopedAttrs, 2, &introQuery);
    waitOp(stage, enq, "query(scoped)");

    ovstage_query_result_t result{};
    status = ovstage_fetch_query_result(stage, introQuery, OVSTAGE_TIMEOUT_INFINITE, &result);
    check(stage, status, "fetch_query_result");

    std::vector<std::string> reported;
    for (size_t i = 0; i < result.attribute_count; ++i)
        reported.push_back(tokenName(dict, result.attributes[i]));
    std::sort(reported.begin(), reported.end());

    std::printf("query introspection (HAS example:count, scoped to two attributes):\n");
    std::printf("  total_prim_count: %llu\n", static_cast<unsigned long long>(result.total_prim_count));
    std::printf("  reported attributes:");
    for (const std::string& name : reported)
        std::printf(" %s", name.c_str());
    std::printf("\n  all_handle == query handle: %s\n", result.all_handle == introQuery ? "yes" : "no");

    const ColumnRows countRows = readColumnRows(stage, dict, result.all_handle, countTok, endOrdinal);
    status = ovstage_release_query_result(stage, &result);
    check(stage, status, "release_query_result");

    // Example plumbing: the example:count cells are int32 -- print "path=value",
    // sorted for deterministic output.
    std::vector<std::string> countLines;
    for (size_t i = 0; i < countRows.paths.size(); ++i)
        countLines.push_back(countRows.paths[i] + "=" +
                             std::to_string(static_cast<int32_t>(countRows.cells[i])));
    std::sort(countLines.begin(), countLines.end());
    std::printf("  example:count via all_handle:");
    for (const std::string& row : countLines)
        std::printf(" %s", row.c_str());
    std::printf("\n");
    // [/snippet:query-introspection]

    // ---- 5. map scene-graph instancing ----
    // [snippet:instancing-queries-c]
    // Enumerate generated prototype paths; do not hard-code their names.
    ovx_primpath_list_t prototypeRoots = OVX_INVALID_PRIMPATH_LIST;
    status = ovstage_instancing_get_prototype_roots(stage, &prototypeRoots);
    check(stage, status, "get_prototype_roots");
    const std::vector<ovx_primpath_t> protoHandles = pathListHandles(dict, prototypeRoots);
    if (protoHandles.empty())
    {
        std::fprintf(stderr, "expected at least one prototype root\n");
        return EXIT_FAILURE;
    }
    std::printf("prototype roots: %llu\n", static_cast<unsigned long long>(protoHandles.size()));
    // prototype roots: 1

    // One prototype -> its instance roots (both instanceable references).
    const ovx_primpath_t prototypeRoot = protoHandles[0];
    ovx_primpath_list_t instanceRoots = OVX_INVALID_PRIMPATH_LIST;
    status = ovstage_instancing_get_instance_roots(stage, prototypeRoot, &instanceRoots);
    check(stage, status, "get_instance_roots");
    std::vector<std::string> instancePaths;
    for (const ovx_primpath_t handle : pathListHandles(dict, instanceRoots))
        instancePaths.push_back(resolvePath(dict, handle));
    std::sort(instancePaths.begin(), instancePaths.end());
    std::printf("instance roots of the prototype:");
    // instance roots of the prototype: /World/InstanceA /World/InstanceB
    for (const std::string& p : instancePaths)
        std::printf(" %s", p.c_str());
    std::printf("\n");

    // One instance -> back to its prototype root. Prim paths are dictionary-interned, so equal
    // path means equal handle.
    const ovx_string_t instanceAName = literal_to_ovx_string("/World/InstanceA");
    ovx_primpath_t instanceA = OVX_INVALID_PRIMPATH;
    ovxResult = path_dictionary_create_paths_from_strings(dict, &instanceAName, 1, &instanceA);
    checkOvx(dict, ovxResult, "intern-path");
    ovx_primpath_t backToPrototype = OVX_INVALID_PRIMPATH;
    status = ovstage_instancing_get_prototype_root(stage, instanceA, &backToPrototype);
    check(stage, status, "get_prototype_root");
    std::printf("/World/InstanceA maps back to the same prototype root: %s\n",
                backToPrototype == prototypeRoot ? "yes" : "no");
    // /World/InstanceA maps back to the same prototype root: yes

    // Lists own references; individual path handles need no release.
    checkOvx(dict, path_dictionary_release_path_list_reference(dict, instanceRoots), "release instances");
    checkOvx(dict, path_dictionary_release_path_list_reference(dict, prototypeRoots), "release prototypes");
    // [/snippet:instancing-queries-c]

    // Release every handle, then destroy: ovstage_destroy_instance requires all ops and handles
    // released first. The instancing path lists were released above.
    enq = ovstage_release_query(stage, introQuery);
    waitOp(stage, enq, "release_query");
    enq = ovstage_release_query(stage, targetQuery);
    waitOp(stage, enq, "release_query");
    enq = ovstage_release_query(stage, sceneQuery);
    waitOp(stage, enq, "release_query");
    path_dictionary_release_path_list_reference(dict, targetPaths);
    path_dictionary_release_path_list_reference(dict, scenePaths);
    ovstage_destroy_instance(stage);

    // Load the nested fixture into a separate stage so the examples above stay independent.
    check(nullptr, ovstage_create_instance(&desc, &stage), "create nested stage");
    dict = getPathDictionary(stage);
    const std::string sceneFile(scenePath);
    const auto slash = sceneFile.find_last_of("/\\");
    const std::string nestedFile = (slash == std::string::npos ? "" : sceneFile.substr(0, slash + 1)) + "nested.usda";
    // [snippet:instancing-timing-c]
    const ovx_string_t nestedPath{ nestedFile.data(), nestedFile.size() };
    waitPop(stage, ovstage_population_open_usd_from_file(
        stage, nestedPath, 1, NAN, OVSTAGE_POPULATION_DOMAIN_PHYSICS), "populate nested scene");
    // Wait for population, then advance the watermark before querying.
    waitOp(stage, ovstage_advance_write_floor(stage, &floor1), "advance watermark");
    // [/snippet:instancing-timing-c]

    // [snippet:instancing-nested-c]
    const ovx_string_t names[] = { literal_to_ovx_string("/World/OuterSource/NestedLeaf"),
                                  literal_to_ovx_string("/World/OuterA/NestedLeaf") };
    ovx_primpath_t nestedPaths[2];
    checkOvx(dict, path_dictionary_create_paths_from_strings(dict, names, 2, nestedPaths), "intern paths");
    ovx_primpath_t leaf = OVX_INVALID_PRIMPATH, nestedLeaf = OVX_INVALID_PRIMPATH;
    check(stage, ovstage_instancing_get_prototype_root(stage, nestedPaths[0], &leaf), "leaf prototype");
    ovx_primpath_list_t leafInstances = OVX_INVALID_PRIMPATH_LIST;
    check(stage, ovstage_instancing_get_instance_roots(stage, leaf, &leafInstances), "leaf instances");
    std::vector<std::string> leafPaths;
    for (auto path : pathListHandles(dict, leafInstances))
        leafPaths.push_back(resolvePath(dict, path));
    std::sort(leafPaths.begin(), leafPaths.end());
    for (const auto& path : leafPaths)
        std::printf("%s\n", path.c_str());
        // /World/OuterA/NestedLeaf
        // /World/OuterSource/NestedLeaf
    check(stage, ovstage_instancing_get_prototype_root(stage, nestedPaths[1], &nestedLeaf), "nested prototype");
    std::printf("same leaf prototype: %s\n", nestedLeaf == leaf ? "yes" : "no");
    // same leaf prototype: yes
    checkOvx(dict, path_dictionary_release_path_list_reference(dict, leafInstances), "release leaf instances");
    // [/snippet:instancing-nested-c]

    // [snippet:instancing-not-found-c]
    const ovx_string_t sourceName = literal_to_ovx_string("/World/LeafSource");
    ovx_primpath_t source = OVX_INVALID_PRIMPATH, unusedPrototype = OVX_INVALID_PRIMPATH;
    checkOvx(dict, path_dictionary_create_paths_from_strings(dict, &sourceName, 1, &source), "intern source");
    status = ovstage_instancing_get_prototype_root(stage, source, &unusedPrototype);
    if (status == OVSTAGE_ERROR_NOT_FOUND)
    {
        std::printf("not an instance root\n");
        // not an instance root
    }
    else
        check(stage, status, "get prototype");
    // [/snippet:instancing-not-found-c]
    ovstage_destroy_instance(stage);
    return 0;
}
