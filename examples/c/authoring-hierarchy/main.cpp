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
// Client-side authoring + hierarchy (C): build a multi-environment world with
// ZERO USD -- prims come into existence via attribute writes to nested paths --
// then show that derived world transforms are pull-computed: they stay stale
// until a hierarchy computation model runs. The world-transform compute runs
// on the GPU in this ovstage revision, so a CUDA-capable device is required.
//
// Expected output: see README.md. Snippet markers are referenced by the
// skills under ../../../skills/ -- keep them intact.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../common/ovstage_example_utils.h"

// The prototype subtree: nested paths only -- hierarchy derives from the path
// structure, no parenting API involved. Arm and Body are siblings under Proto.
static const char* kProtoPaths[] = {
    "/World", "/World/Proto", "/World/Proto/Arm", "/World/Proto/Body", "/World/Proto/Body/Tip",
};
static const size_t kProtoCount = sizeof(kProtoPaths) / sizeof(kProtoPaths[0]);

// One USD prim type per prototype prim (interned to tokens below).
static const char* kProtoTypes[] = { "Xform", "Xform", "Cube", "Xform", "Cube" };

// Distinct local translation per hierarchy level (row-vector convention:
// translation in matrix elements [12..14]). Tip world = sum of its chain.
static const double kProtoLocals[kProtoCount][3] = {
    { 0.0, 100.0, 0.0 },  // /World
    { 10.0, 0.0, 0.0 },   // /World/Proto
    { 0.0, 0.0, 3.0 },    // /World/Proto/Arm
    { 0.0, 0.0, 5.0 },    // /World/Proto/Body
    { 0.0, 0.0, 2.0 },    // /World/Proto/Body/Tip
};

static const char* kEnvPaths[] = { "/World/Env_0", "/World/Env_1", "/World/Env_2" };
static const size_t kEnvCount = sizeof(kEnvPaths) / sizeof(kEnvPaths[0]);

// Derived world-transform column maintained by the hierarchy computation models.
static const char* kWorldMatrixAttr = "omni:fabric:worldMatrix";

// The Tip prims whose world translations are printed after the compute.
static const char* kTipPaths[] = {
    "/World/Proto/Body/Tip", "/World/Env_0/Body/Tip", "/World/Env_1/Body/Tip", "/World/Env_2/Body/Tip",
};
static const size_t kTipCount = sizeof(kTipPaths) / sizeof(kTipPaths[0]);

static const int64_t kProtoShape[] = { static_cast<int64_t>(kProtoCount) };
static const int64_t kUnitStrides[] = { 1 };

// Fill a row-major 4x4 translation matrix (row-vector convention: translation
// in the last row, elements [12..14]).
static void translationMatrix(double tx, double ty, double tz, double* m)
{
    static const double identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    std::memcpy(m, identity, sizeof(identity));
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
}

// cpuFloatTensor's sibling for token-id columns: uint64 ids, one lane.
static DLTensor cpuTokenTensor(uint64_t* ids, const int64_t* shape, const int64_t* strides)
{
    DLTensor tensor{};
    tensor.data = ids;
    tensor.device = { kDLCPU, 0 };
    tensor.ndim = 1;
    tensor.dtype = { kDLUInt, 64, 1 };
    tensor.shape = const_cast<int64_t*>(shape);
    tensor.strides = const_cast<int64_t*>(strides);
    return tensor;
}

// Write `rows` 4x4 double matrices (16 doubles each, contiguous in `data`)
// into a matrix column over `query`. This helper uses the canonical transport
// form: ONE 16-lane element per prim, dtype lanes=16 / shape={rows}. A compact
// 4x4-of-lanes=1 copy-in is accepted but normalized back to this raw form.
static void writeMatrixRows(ovstage_instance_t* stage, ovstage_query_handle_t query, ovx_token_t attribute,
                            double* data, size_t rows, ovstage_ordinal_t ordinal, const char* what)
{
    const int64_t shape[] = { static_cast<int64_t>(rows) };
    DLTensor tensor{};
    tensor.data = data;
    tensor.device = { kDLCPU, 0 };
    tensor.ndim = 1;
    tensor.dtype = { kDLFloat, 64, 16 };
    tensor.shape = const_cast<int64_t*>(shape);
    tensor.strides = const_cast<int64_t*>(kUnitStrides);
    ovstage_write_data_t write = writeData(&tensor, OVSTAGE_SEMANTIC_MATRIX);
    ovstage_enqueue_result_t enq =
        ovstage_write_attribute(stage, query, { attribute, {} }, ordinal, write, OVSTAGE_PRIM_MODE_UPSERT);
    waitOp(stage, enq, what);
}

// Write one prim's local transform (omni:xform) as a pure translation.
static void writeLocalTranslation(ovstage_instance_t* stage, path_dictionary_instance_t* dict, ovx_token_t xform,
                                  const char* path, double tx, double ty, double tz, ovstage_ordinal_t ordinal)
{
    const ovx_string_t pathStr{ path, std::strlen(path) };
    ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
    ovx_api_result_t ovxResult = path_dictionary_create_path_list_from_strings(dict, &pathStr, 1, &list);
    checkOvx(dict, ovxResult, "path-list");
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_api_status_t status = ovstage_query_from_path_list(stage, list, &query);
    check(stage, status, "query_from_path_list");

    double matrix[16];
    translationMatrix(tx, ty, tz, matrix);
    writeMatrixRows(stage, query, xform, matrix, 1, ordinal, "write omni:xform");

    ovstage_enqueue_result_t enq = ovstage_release_query(stage, query);
    waitOp(stage, enq, "release_query");
    path_dictionary_release_path_list_reference(dict, list);
}

// Materialize the derived world-matrix output column client-side: one identity
// placeholder row per prim (prototype + every cloned environment prim). USD
// population creates this column automatically; a pure client-side stage must
// author it before a hierarchy computation model has an output to fill.
static void seedWorldMatrixRows(ovstage_instance_t* stage, path_dictionary_instance_t* dict, ovx_token_t worldMatrix,
                                ovstage_ordinal_t ordinal)
{
    // 5 prototype prims + 4 prims per cloned environment (Env_i, Arm, Body, Tip).
    static const char* kEnvSuffixes[] = { "", "/Arm", "/Body", "/Body/Tip" };
    char envBuffers[kEnvCount * 4][48];
    ovx_string_t allPaths[kProtoCount + kEnvCount * 4];
    size_t count = 0;
    for (size_t i = 0; i < kProtoCount; ++i)
        allPaths[count++] = ovx_string_t{ kProtoPaths[i], std::strlen(kProtoPaths[i]) };
    for (size_t e = 0; e < kEnvCount; ++e)
    {
        for (size_t s = 0; s < 4; ++s)
        {
            char* buffer = envBuffers[e * 4 + s];
            std::snprintf(buffer, sizeof(envBuffers[0]), "%s%s", kEnvPaths[e], kEnvSuffixes[s]);
            allPaths[count++] = ovx_string_t{ buffer, std::strlen(buffer) };
        }
    }

    ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
    ovx_api_result_t ovxResult = path_dictionary_create_path_list_from_strings(dict, allPaths, count, &list);
    checkOvx(dict, ovxResult, "path-list");
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_api_status_t status = ovstage_query_from_path_list(stage, list, &query);
    check(stage, status, "query_from_path_list");

    double placeholders[(kProtoCount + kEnvCount * 4) * 16];
    for (size_t i = 0; i < count; ++i)
        translationMatrix(0.0, 0.0, 0.0, &placeholders[i * 16]);
    writeMatrixRows(stage, query, worldMatrix, placeholders, count, ordinal, "seed worldMatrix");

    ovstage_enqueue_result_t enq = ovstage_release_query(stage, query);
    waitOp(stage, enq, "release_query");
    path_dictionary_release_path_list_reference(dict, list);
}

// Advance the global write floor to `ordinal`, sealing it for readers.
static void sealOrdinal(ovstage_instance_t* stage, ovstage_ordinal_t ordinal)
{
    ovstage_write_floor_desc_t floor{};
    floor.ordinal = ordinal;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ovstage_enqueue_result_t enq = ovstage_advance_write_floor(stage, &floor);
    waitOp(stage, enq, "advance_write_floor");
}

// Read one prim's derived world translation (`omni:fabric:worldMatrix` elements
// [12..14]) at `endOrdinal`. Returns false when the derived column has no row
// for this prim -- a normal state before any compute, not an error.
static bool tryReadWorldTranslation(ovstage_instance_t* stage, path_dictionary_instance_t* dict,
                                    ovx_token_t worldMatrix, const char* path, ovstage_ordinal_t endOrdinal,
                                    double out[3])
{
    const ovx_string_t pathStr{ path, std::strlen(path) };
    ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
    ovx_api_result_t ovxResult = path_dictionary_create_path_list_from_strings(dict, &pathStr, 1, &list);
    checkOvx(dict, ovxResult, "path-list");
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_api_status_t status = ovstage_query_from_path_list(stage, list, &query);
    check(stage, status, "query_from_path_list");

    ovstage_ordinal_range_t range{};
    range.end_ordinal = endOrdinal;
    range.has_start_ordinal = false;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_read_attributes(stage, query, &worldMatrix, 1, range, &read);
    waitOp(stage, enq, "read_attributes");

    bool found = false;
    ovstage_read_group_t group{};
    const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
    if (fetched == OVSTAGE_OK)
    {
        // This single-prim query must yield exactly one 16-lane float64 row.
        if (group.data.tensor_count != 1 || !group.data.tensors[0].data || group.data.tensors[0].ndim != 1 ||
            group.data.tensors[0].shape[0] != 1 || group.data.tensors[0].dtype.code != kDLFloat ||
            group.data.tensors[0].dtype.bits != 64 || group.data.tensors[0].dtype.lanes != 16)
        {
            std::fprintf(stderr, "unexpected %s read-group shape for %s\n", kWorldMatrixAttr, path);
            std::exit(EXIT_FAILURE);
        }
        const double* m = static_cast<const double*>(group.data.tensors[0].data);
        out[0] = m[12];
        out[1] = m[13];
        out[2] = m[14];
        found = true;
        ovstage_release_group(stage, &group);
    }
    else if (fetched != OVSTAGE_ERROR_END_OF_ITERATION)
    {
        check(stage, fetched, "fetch_read_next"); // exits: a real fetch error, not "no rows"
    }

    enq = ovstage_release_read(stage, read);
    waitOp(stage, enq, "release_read");
    enq = ovstage_release_query(stage, query);
    waitOp(stage, enq, "release_query");
    path_dictionary_release_path_list_reference(dict, list);
    return found;
}

// Fail-fast sibling of tryReadWorldTranslation for reads that must succeed
// (every read after the placeholder rows were seeded).
static void readWorldTranslation(ovstage_instance_t* stage, path_dictionary_instance_t* dict, ovx_token_t worldMatrix,
                                 const char* path, ovstage_ordinal_t endOrdinal, double out[3])
{
    if (!tryReadWorldTranslation(stage, dict, worldMatrix, path, endOrdinal, out))
    {
        std::fprintf(stderr, "missing %s row for %s\n", kWorldMatrixAttr, path);
        std::exit(EXIT_FAILURE);
    }
}

// Read a token-id column (uint64 ids, scalar or array rows) over `query` and
// return the resolved names ordered by each covered prim's position in the
// group's path list. A group may cover its prims contiguously from an offset
// or sparsely via prims.index_map, and its tensor rows may be gathered /
// reordered / deduplicated via data.index_map (see ovstage_data_t) -- honor
// both mappings instead of dumping raw tensor rows. usd-schemas rows are
// whole-set assignments with no guaranteed element order, so that caller
// sorts before printing. `what` only labels the fail-fast diagnostics.
static std::vector<std::string> readTokenColumnNames(ovstage_instance_t* stage, path_dictionary_instance_t* dict,
                                                     ovstage_query_handle_t query, ovx_token_t attribute,
                                                     ovstage_ordinal_t endOrdinal, const char* what)
{
    ovstage_ordinal_range_t range{};
    range.end_ordinal = endOrdinal;
    range.has_start_ordinal = false;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_read_attributes(stage, query, &attribute, 1, range, &read);
    waitOp(stage, enq, "read_attributes");

    const auto layoutError = [what]() {
        std::fprintf(stderr, "unexpected token-column read-group shape (%s)\n", what);
        std::exit(EXIT_FAILURE);
    };
    // Token-id columns store one uint64 interned id per element (TOKEN_ID
    // semantic); anything else here means the wrong column was read.
    const auto isTokenTensor = [](const DLTensor& tensor) {
        return tensor.data && tensor.ndim >= 1 && tensor.dtype.code == kDLUInt && tensor.dtype.bits == 64 &&
               tensor.dtype.lanes == 1;
    };

    // One (path-list position, resolved names) entry per covered prim; sorted
    // after the loop so multi-group results still return in path-list order.
    std::vector<std::pair<size_t, std::vector<std::string>>> perPrim;
    for (;;)
    {
        ovstage_read_group_t group{};
        const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
        if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        check(stage, fetched, "fetch_read_next");
        if (group.is_delete)
        {
            ovstage_release_group(stage, &group); // tombstone group: no data, no names
            continue;
        }
        // Every covered prim must resolve to a transported data row, directly
        // or through data.index_map. Fixed rows are stacked in one tensor;
        // array attributes use one tensor per data row.
        if (!group.data.tensors)
            layoutError();
        if (!group.is_array && (group.data.tensor_count != 1 || !isTokenTensor(group.data.tensors[0])))
            layoutError();
        const size_t logicalCount = group.data.index_map ?
                                        group.data.count :
                                        (group.is_array ? group.data.tensor_count :
                                                          static_cast<size_t>(group.data.tensors[0].shape[0]));
        if (logicalCount != group.prims.count)
            layoutError();

        for (uint32_t local = 0; local < group.prims.count; ++local)
        {
            const size_t listIndex =
                group.prims.index_map ? group.prims.index_map[local] : group.prims.offset + local;
            const uint32_t row = group.data.index_map ? group.data.index_map[local] : local;
            const uint64_t* ids = nullptr;
            int64_t elementCount = 0;
            if (group.is_array)
            {
                // Per-row transport: the data index map picks which tensor
                // holds this prim's variable-length row.
                if (row >= group.data.tensor_count || !isTokenTensor(group.data.tensors[row]))
                    layoutError();
                ids = static_cast<const uint64_t*>(group.data.tensors[row].data);
                elementCount = group.data.tensors[row].shape[0];
            }
            else
            {
                // Stacked rows: the data index map picks this prim's row in
                // the single tensor.
                if (static_cast<int64_t>(row) >= group.data.tensors[0].shape[0])
                    layoutError();
                ids = static_cast<const uint64_t*>(group.data.tensors[0].data) + row;
                elementCount = 1;
            }
            std::vector<std::string> primNames;
            primNames.reserve(static_cast<size_t>(elementCount));
            for (int64_t i = 0; i < elementCount; ++i)
            {
                ovx_token_t token = static_cast<ovx_token_t>(ids[i]);
                ovx_string_t name{};
                ovx_api_result_t ovxResult = path_dictionary_get_strings_from_tokens(dict, &token, 1, &name);
                checkOvx(dict, ovxResult, "resolve-token");
                primNames.emplace_back(name.ptr ? name.ptr : "", name.length);
            }
            perPrim.emplace_back(listIndex, std::move(primNames));
        }
        ovstage_release_group(stage, &group);
    }
    enq = ovstage_release_read(stage, read);
    waitOp(stage, enq, "release_read");

    std::stable_sort(perPrim.begin(), perPrim.end(),
                     [](const std::pair<size_t, std::vector<std::string>>& a,
                        const std::pair<size_t, std::vector<std::string>>& b) { return a.first < b.first; });
    std::vector<std::string> names;
    for (auto& entry : perPrim)
        for (std::string& n : entry.second)
            names.push_back(std::move(n));
    return names;
}

// [snippet:hierarchy-lookups]
// Backend hierarchy lookup for one prim: enqueue ovstage_get_hierarchy over a
// path list, fetch the per-input result, copy out the resolved relation paths
// (returned as-is -- the backend reports them unordered), then release the
// result payload and the lookup handle. A missing input prim reports per-item
// status OVSTAGE_ERROR_NOT_FOUND instead of failing the batch.
static std::vector<std::string> lookupHierarchyPaths(ovstage_instance_t* stage, path_dictionary_instance_t* dict,
                                                     const char* path, ovstage_hierarchy_relation_t relation,
                                                     ovstage_ordinal_t ordinal)
{
    const ovx_string_t pathStr{ path, std::strlen(path) };
    ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
    ovx_api_result_t ovxResult = path_dictionary_create_path_list_from_strings(dict, &pathStr, 1, &list);
    checkOvx(dict, ovxResult, "path-list");

    ovstage_hierarchy_handle_t handle = OVSTAGE_INVALID_HIERARCHY_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_get_hierarchy(stage, list, ordinal, relation, &handle);
    waitOp(stage, enq, "get_hierarchy");
    ovstage_hierarchy_result_t result{};
    ovstage_api_status_t status = ovstage_fetch_hierarchy_result(stage, handle, &result);
    check(stage, status, "fetch_hierarchy_result");

    std::vector<std::string> paths;
    if (result.input_count == 1 && result.items[0].status == OVSTAGE_OK)
    {
        for (size_t i = 0; i < result.items[0].path_count; ++i)
        {
            const ovx_string_t& p = result.paths[result.items[0].path_offset + i].string;
            paths.emplace_back(p.ptr ? p.ptr : "", p.length);
        }
    }

    status = ovstage_release_hierarchy_result(stage, &result);
    check(stage, status, "release_hierarchy_result");
    enq = ovstage_release_hierarchy(stage, handle);
    waitOp(stage, enq, "release_hierarchy");
    path_dictionary_release_path_list_reference(dict, list);
    return paths;
}
// [/snippet:hierarchy-lookups]

// Example plumbing: print `label:` followed by each name on one line. No API
// calls here -- callers whose names come back unordered (usd-schemas rows,
// hierarchy relation paths) pass sortNames for deterministic output.
static void printNameList(const char* label, std::vector<std::string> names, bool sortNames)
{
    if (sortNames)
        std::sort(names.begin(), names.end());
    std::printf("%s:", label);
    for (const std::string& n : names)
        std::printf(" %s", n.c_str());
    std::printf("\n");
}

int main()
{
    // ---- 1. setup: instance, dictionary, interned attribute tokens ----
    ovstage_instance_desc_t desc{};
    desc.name = "example.authoring-hierarchy";
    ovstage_instance_t* stage = nullptr;
    ovstage_api_status_t status = ovstage_create_instance(&desc, &stage);
    check(nullptr, status, "create_instance");
    path_dictionary_instance_t* dict = getPathDictionary(stage);

    ovx_string_t attrNames[] = { literal_to_ovx_string("proto:part"), literal_to_ovx_string("usd-prim-type"),
                                 literal_to_ovx_string("usd-schemas"), literal_to_ovx_string("omni:xform"),
                                 ovx_string_t{ kWorldMatrixAttr, std::strlen(kWorldMatrixAttr) } };
    ovx_token_t attrTokens[5] = {};
    ovx_api_result_t ovxResult = path_dictionary_create_tokens_from_strings(dict, attrNames, 5, attrTokens);
    checkOvx(dict, ovxResult, "intern-tokens");
    const ovx_token_t protoPart = attrTokens[0];
    const ovx_token_t primType = attrTokens[1];
    const ovx_token_t schemas = attrTokens[2];
    const ovx_token_t xform = attrTokens[3];
    const ovx_token_t worldMatrix = attrTokens[4];

    // ---- 2. author at ordinal 1: the prototype subtree, zero USD ----

    // [snippet:insert-author-subtree]
    // An INSERT write of any attribute to nested paths materializes the prims
    // (INSERT = create-only; it fails with PRIM_NOT_FOUND if a prim already
    // exists, so it doubles as an "author exactly once" guard). Hierarchy
    // derives purely from the path structure -- there is no parenting call.
    ovx_string_t protoPathStrs[kProtoCount];
    for (size_t i = 0; i < kProtoCount; ++i)
        protoPathStrs[i] = ovx_string_t{ kProtoPaths[i], std::strlen(kProtoPaths[i]) };
    ovx_primpath_list_t protoList = OVX_INVALID_PRIMPATH_LIST;
    ovxResult = path_dictionary_create_path_list_from_strings(dict, protoPathStrs, kProtoCount, &protoList);
    checkOvx(dict, ovxResult, "path-list");
    ovstage_query_handle_t protoQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    status = ovstage_query_from_path_list(stage, protoList, &protoQuery);
    check(stage, status, "query_from_path_list");

    float partIds[kProtoCount] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f };
    DLTensor partTensor = cpuFloatTensor(partIds, kProtoShape, kUnitStrides, 1);
    ovstage_write_data_t partWrite = writeData(&partTensor);
    ovstage_enqueue_result_t enq = ovstage_write_attribute(stage, protoQuery, { protoPart, {} }, /*ordinal*/ 1,
                                                           partWrite, OVSTAGE_PRIM_MODE_INSERT);
    waitOp(stage, enq, "insert-author write");
    // [/snippet:insert-author-subtree]

    // [snippet:author-prim-types]
    // Stamp USD prim types client-side via the reserved "usd-prim-type"
    // attribute: one interned token id per prim (scalar uint64 column, see
    // cpuTokenTensor). The same ids later serve usd-prim-type IN query filters.
    ovx_string_t typeNames[kProtoCount];
    for (size_t i = 0; i < kProtoCount; ++i)
        typeNames[i] = ovx_string_t{ kProtoTypes[i], std::strlen(kProtoTypes[i]) };
    ovx_token_t typeTokens[kProtoCount] = {};
    ovxResult = path_dictionary_create_tokens_from_strings(dict, typeNames, kProtoCount, typeTokens);
    checkOvx(dict, ovxResult, "intern-types");

    uint64_t typeIds[kProtoCount];
    for (size_t i = 0; i < kProtoCount; ++i)
        typeIds[i] = typeTokens[i];
    DLTensor typeTensor = cpuTokenTensor(typeIds, kProtoShape, kUnitStrides);
    ovstage_write_data_t typeWrite = writeData(&typeTensor);
    enq = ovstage_write_attribute(stage, protoQuery, { primType, {} }, /*ordinal*/ 1, typeWrite,
                                  OVSTAGE_PRIM_MODE_UPSERT);
    waitOp(stage, enq, "write usd-prim-type");
    // [/snippet:author-prim-types]

    // [snippet:author-applied-schemas]
    // Apply API schemas via the reserved "usd-schemas" attribute: an ARRAY of
    // interned token ids per prim (is_array=true). Each row is a whole-set
    // assignment -- writing a row replaces that prim's full schema set, and an
    // empty row clears it.
    const ovx_string_t bodyPath = literal_to_ovx_string("/World/Proto/Body");
    ovx_primpath_list_t bodyList = OVX_INVALID_PRIMPATH_LIST;
    ovxResult = path_dictionary_create_path_list_from_strings(dict, &bodyPath, 1, &bodyList);
    checkOvx(dict, ovxResult, "path-list");
    ovstage_query_handle_t bodyQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    status = ovstage_query_from_path_list(stage, bodyList, &bodyQuery);
    check(stage, status, "query_from_path_list");

    ovx_string_t schemaNames[] = { literal_to_ovx_string("PhysicsRigidBodyAPI"),
                                   literal_to_ovx_string("PhysicsMassAPI") };
    ovx_token_t schemaTokens[2] = {};
    ovxResult = path_dictionary_create_tokens_from_strings(dict, schemaNames, 2, schemaTokens);
    checkOvx(dict, ovxResult, "intern-schemas");

    uint64_t schemaIds[2] = { schemaTokens[0], schemaTokens[1] };
    const int64_t schemaShape[] = { 2 };
    DLTensor schemaTensor = cpuTokenTensor(schemaIds, schemaShape, kUnitStrides);
    ovstage_write_data_t schemaWrite = writeData(&schemaTensor, OVSTAGE_SEMANTIC_NONE, /*isArray*/ true);
    enq = ovstage_write_attribute(stage, bodyQuery, { schemas, {} }, /*ordinal*/ 1, schemaWrite,
                                  OVSTAGE_PRIM_MODE_UPSERT);
    waitOp(stage, enq, "write usd-schemas");
    // [/snippet:author-applied-schemas]

    // [snippet:author-local-transforms]
    // Author local transforms: omni:xform is the canonical 4x4 double
    // local-matrix column (an alias of the derived `omni:fabric:localMatrix`);
    // The current implementation stores each matrix as ONE 16-lane float64 element per prim (see
    // writeMatrixRows). Each level gets a distinct translation so world
    // compositions are recognizable sums down the chain.
    double protoMatrices[kProtoCount * 16];
    for (size_t i = 0; i < kProtoCount; ++i)
        translationMatrix(kProtoLocals[i][0], kProtoLocals[i][1], kProtoLocals[i][2], &protoMatrices[i * 16]);
    writeMatrixRows(stage, protoQuery, xform, protoMatrices, kProtoCount, /*ordinal*/ 1, "write omni:xform");
    sealOrdinal(stage, /*ordinal*/ 1);
    // [/snippet:author-local-transforms]

    // ---- 3. read back prim types + applied schemas ----
    const std::vector<std::string> protoTypeNames =
        readTokenColumnNames(stage, dict, protoQuery, primType, /*endOrdinal*/ 1, "prototype prim types");
    const std::vector<std::string> bodySchemaNames = readTokenColumnNames(
        stage, dict, bodyQuery, schemas, /*endOrdinal*/ 1, "applied schemas on /World/Proto/Body");
    // Example plumbing: prim types read back in row order (one per kProtoPaths
    // entry); usd-schemas rows carry no guaranteed element order, so the schema
    // names print sorted for deterministic output.
    printNameList("prototype prim types", protoTypeNames, /*sortNames*/ false);
    printNameList("applied schemas on /World/Proto/Body", bodySchemaNames, /*sortNames*/ true);

    // ---- 4. clone at ordinal 2: /World/Proto -> /World/Env_0..2 ----

    // [snippet:clone-prototype-envs]
    // Stamp out the environments: clone the prototype subtree to three targets
    // in ONE call (clone is an ordinal-keyed write; the source must exist, the
    // targets must not), then give each environment root a distinct
    // translation so the environments tile along +X.
    ovx_string_t cloneTargets[kEnvCount];
    for (size_t i = 0; i < kEnvCount; ++i)
        cloneTargets[i] = ovx_string_t{ kEnvPaths[i], std::strlen(kEnvPaths[i]) };
    enq = ovstage_clone(stage, literal_to_ovx_string("/World/Proto"), cloneTargets, kEnvCount, /*ordinal*/ 2);
    waitOp(stage, enq, "clone");
    for (size_t i = 0; i < kEnvCount; ++i)
        writeLocalTranslation(stage, dict, xform, kEnvPaths[i], 100.0 * static_cast<double>(i + 1), 0.0, 0.0,
                              /*ordinal*/ 2);
    sealOrdinal(stage, /*ordinal*/ 2);
    std::printf("cloned /World/Proto -> %s %s %s\n", kEnvPaths[0], kEnvPaths[1], kEnvPaths[2]);
    // [/snippet:clone-prototype-envs]

    // ---- 5. enumerate hierarchy computation models ----

    // [snippet:hierarchy-computation-models]
    // The descriptor array is implementation-owned (valid for the instance
    // lifetime; nothing to release). This example computes with the
    // GPU_INCREMENTAL model: in this ovstage revision the GPU models recompute
    // world transforms for client-authored prims, while cpu-incremental only
    // derives world transforms during USD population flows.
    const ovstage_hierarchy_computation_model_desc_t* models = nullptr;
    size_t modelCount = 0;
    status = ovstage_get_hierarchy_computation_models(stage, &models, &modelCount);
    check(stage, status, "get_hierarchy_computation_models");
    std::printf("hierarchy computation models:");
    for (size_t i = 0; i < modelCount; ++i)
        std::printf(" %.*s", static_cast<int>(models[i].name.length), models[i].name.ptr ? models[i].name.ptr : "");
    std::printf("\n");
    // [/snippet:hierarchy-computation-models]

    // ---- 6. staleness demo (the heart of this example): world transforms are pull-computed ----

    // [snippet:world-matrix-staleness]
    // Before any compute the derived column does not even exist: reading
    // `omni:fabric:worldMatrix` yields no rows (a normal state, not an error). A
    // pure client-side stage then materializes the output column itself
    // (identity placeholder rows -- USD population would have created them);
    // the placeholders stay STALE until a computation model runs.
    // ovstage_compute_hierarchy takes the ordinal to compute from (input) and
    // the ordinal stamped onto derived results (output).
    double t[3] = {};
    const bool present = tryReadWorldTranslation(stage, dict, worldMatrix, "/World/Env_0/Body/Tip", /*endOrdinal*/ 2,
                                                 t);
    std::printf("world matrix rows before compute_hierarchy: %s\n",
                present ? "unexpectedly present" : "absent (derived rows are pull-computed)");

    seedWorldMatrixRows(stage, dict, worldMatrix, /*ordinal*/ 3);
    sealOrdinal(stage, /*ordinal*/ 3);
    readWorldTranslation(stage, dict, worldMatrix, "/World/Env_0/Body/Tip", /*endOrdinal*/ 3, t);
    std::printf("stale placeholder world translation /World/Env_0/Body/Tip: %.1f %.1f %.1f\n", t[0], t[1], t[2]);

    // The compute runs on the GPU; without a CUDA-capable device the op fails
    // (waitOp prints the op error and exits).
    enq = ovstage_compute_hierarchy(stage, OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_INCREMENTAL,
                                    /*input_ordinal*/ 3, /*output_ordinal*/ 3);
    waitOp(stage, enq, "compute_hierarchy");

    // Now the derived rows are correct compositions: each Tip world
    // translation is the sum of the pure translations down its chain.
    for (size_t i = 0; i < kTipCount; ++i)
    {
        readWorldTranslation(stage, dict, worldMatrix, kTipPaths[i], /*endOrdinal*/ 3, t);
        std::printf("world translation %s: %.1f %.1f %.1f\n", kTipPaths[i], t[0], t[1], t[2]);
    }
    // [/snippet:world-matrix-staleness]

    // ---- 7. late edit: move /World/Env_1, compute again ----

    // [snippet:recompute-after-edit]
    // Update ONE environment's local transform at a later ordinal, then run
    // compute_hierarchy again with the new input ordinal to fold the edit into
    // the derived rows (the tip's z becomes 47.0). Reading the derived column
    // between the edit and the recompute is NOT a reliable staleness probe:
    // the read itself can pull an incremental refresh. Only the explicit
    // compute guarantees derived rows consistent with the new input ordinal --
    // which is why this example reads only after it.
    writeLocalTranslation(stage, dict, xform, "/World/Env_1", 200.0, 0.0, 40.0, /*ordinal*/ 4);
    sealOrdinal(stage, /*ordinal*/ 4);
    std::printf("moved /World/Env_1 at ordinal 4\n");

    enq = ovstage_compute_hierarchy(stage, OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_INCREMENTAL,
                                    /*input_ordinal*/ 4, /*output_ordinal*/ 4);
    waitOp(stage, enq, "compute_hierarchy");
    readWorldTranslation(stage, dict, worldMatrix, "/World/Env_1/Body/Tip", /*endOrdinal*/ 4, t);
    std::printf("recomputed world translation /World/Env_1/Body/Tip: %.1f %.1f %.1f\n", t[0], t[1], t[2]);
    // [/snippet:recompute-after-edit]

    // ---- 8. hierarchy lookups: parent / children / siblings ----
    const std::vector<std::string> parentPaths =
        lookupHierarchyPaths(stage, dict, "/World/Proto/Body", OVSTAGE_HIERARCHY_PARENT, /*ordinal*/ 4);
    const std::vector<std::string> childPaths =
        lookupHierarchyPaths(stage, dict, "/World/Proto", OVSTAGE_HIERARCHY_CHILDREN, /*ordinal*/ 4);
    const std::vector<std::string> siblingPaths =
        lookupHierarchyPaths(stage, dict, "/World/Proto/Body", OVSTAGE_HIERARCHY_SIBLINGS, /*ordinal*/ 4);
    // Example plumbing: each lookup's relation paths come back unordered --
    // print them sorted for deterministic output.
    printNameList("parent of /World/Proto/Body", parentPaths, /*sortNames*/ true);
    printNameList("children of /World/Proto", childPaths, /*sortNames*/ true);
    printNameList("siblings of /World/Proto/Body", siblingPaths, /*sortNames*/ true);

    // Release every handle, then destroy: ovstage_destroy_instance requires
    // all ops and handles released first.
    enq = ovstage_release_query(stage, bodyQuery);
    waitOp(stage, enq, "release_query");
    enq = ovstage_release_query(stage, protoQuery);
    waitOp(stage, enq, "release_query");
    path_dictionary_release_path_list_reference(dict, bodyList);
    path_dictionary_release_path_list_reference(dict, protoList);
    ovstage_destroy_instance(stage);
    return 0;
}
