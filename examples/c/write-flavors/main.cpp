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
// ovstage write workflows (C): a tour of the higher-level write workflows —
// batched multi-attribute writes, clone, and pipelined (submit-ahead) submission.
// Each section writes at its own ordinals, seals them with advance_write_floor,
// reads back, and prints a few deterministic lines. CPU-only.
//
// The fine-grained write *contracts* (column shapes, semantics, UPSERT/INSERT
// admission, sparse index_map/mask, delete tombstones, CPU map/unmap) are asserted
// by the public tests under ../../../tests/ -- see that tree's AGENTS.md.
//
// Expected output: see README.md. Snippet markers are referenced by the skills
// under ../../../skills/ -- keep them intact.

#include <ovstage/ovstage.h>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <ovx/path_dictionary/path_dictionary.h>
#include <dlpack/dlpack.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/ovstage_example_utils.h"

// ============================================================================
// Plumbing: setup/read helpers shared by the sections of main().
// ============================================================================

// Build a string view over a NUL-terminated path or attribute name.
static ovx_string_t ovxStr(const char* text)
{
    return ovx_string_t{ text, std::strlen(text) };
}

static ovx_token_t internToken(path_dictionary_instance_t* dict, const char* name)
{
    const ovx_string_t nameString = ovxStr(name);
    ovx_token_t token = OVX_INVALID_TOKEN;
    ovx_api_result_t ovxResult = path_dictionary_create_tokens_from_strings(dict, &nameString, 1, &token);
    checkOvx(dict, ovxResult, "create-tokens");
    return token;
}

// Build a path-list query over `count` paths. When orderPaths is non-null it
// also receives the interned per-path handles (dict-lifetime), which serve as
// stable row identities for path-keyed readbacks. The caller releases the
// query and the returned list.
static ovx_primpath_list_t queryFromPaths(ovstage_instance_t* stage, path_dictionary_instance_t* dict,
                                          const char* const* paths, size_t count, ovx_primpath_t* orderPaths,
                                          ovstage_query_handle_t* query)
{
    ovx_string_t strings[4];
    if (count > 4)
    {
        std::fprintf(stderr, "queryFromPaths supports at most 4 paths\n");
        std::exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < count; ++i)
        strings[i] = ovxStr(paths[i]);
    if (orderPaths)
    {
        ovx_api_result_t ovxResult = path_dictionary_create_paths_from_strings(dict, strings, count, orderPaths);
        checkOvx(dict, ovxResult, "create-paths");
    }
    ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
    ovx_api_result_t ovxResult = path_dictionary_create_path_list_from_strings(dict, strings, count, &list);
    checkOvx(dict, ovxResult, "create-path-list");
    ovstage_api_status_t status = ovstage_query_from_path_list(stage, list, query);
    check(stage, status, "query_from_path_list");
    return list;
}

// Seal every attribute up to `ordinal` so reads can observe the writes below it.
static void advanceFloor(ovstage_instance_t* stage, ovstage_ordinal_t ordinal)
{
    ovstage_write_floor_desc_t desc{};
    desc.ordinal = ordinal;
    desc.scope = OVSTAGE_SCOPE_ALL;
    ovstage_enqueue_result_t enq = ovstage_advance_write_floor(stage, &desc);
    waitOp(stage, enq, "advance_write_floor");
}

// Resolve which prim path a group's row `local` covers (groups carry an
// optional index map over their path list).
static ovx_primpath_t primAt(path_dictionary_instance_t* dict, const ovstage_prim_group_t& prims, uint32_t local)
{
    const size_t listIndex = prims.index_map ? prims.index_map[local] : prims.offset + local;
    ovx_primpath_t path = OVX_INVALID_PRIMPATH;
    size_t got = 0;
    ovx_api_result_t ovxResult = path_dictionary_get_paths_from_path_list(dict, prims.list, listIndex, 1, &path, &got);
    checkOvx(dict, ovxResult, "get_paths_from_path_list");
    if (got != 1)
    {
        std::fprintf(stderr, "path list lookup returned %zu paths (expected 1)\n", got);
        std::exit(EXIT_FAILURE);
    }
    return path;
}

// Gather the latest committed value per prim of a 1-lane float column into
// values[] / present[] (indexed like orderPaths). A latest read may cover the
// query's prims through several groups, each carrying a prim index map (which
// prims of its list are covered) and a data index map (which tensor row holds
// each covered prim's value); delete groups (tombstones) carry no data.
static void readLatestRows(ovstage_instance_t* stage, path_dictionary_instance_t* dict, ovstage_query_handle_t query,
                           ovx_token_t attr, ovstage_ordinal_t endOrdinal, const ovx_primpath_t* orderPaths,
                           size_t pathCount, float* values, bool* present)
{
    for (size_t i = 0; i < pathCount; ++i)
        present[i] = false;

    ovstage_ordinal_range_t range{};
    range.end_ordinal = endOrdinal;
    range.has_start_ordinal = false;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_read_attributes(stage, query, &attr, 1, range, &read);
    waitOp(stage, enq, "read_attributes");

    for (;;)
    {
        ovstage_read_group_t group{};
        const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
        if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
            break; // the normal end of the group stream
        check(stage, fetched, "fetch_read_next");
        if (!group.is_delete && group.data.tensor_count == 1 && group.data.tensors[0].data)
        {
            const float* rows = static_cast<const float*>(group.data.tensors[0].data);
            for (uint32_t local = 0; local < group.prims.count; ++local)
            {
                const ovx_primpath_t path = primAt(dict, group.prims, local);
                const uint32_t row = group.data.index_map ? group.data.index_map[local] : local;
                for (size_t i = 0; i < pathCount; ++i)
                {
                    if (orderPaths[i] == path)
                    {
                        values[i] = rows[row];
                        present[i] = true;
                    }
                }
            }
        }
        ovstage_api_status_t status = ovstage_release_group(stage, &group);
        check(stage, status, "release_group");
    }
    enq = ovstage_release_read(stage, read);
    waitOp(stage, enq, "release_read");
}

int main()
{
    // ---- setup: one instance and its path dictionary, shared by every section ----
    ovstage_instance_desc_t desc{};
    desc.name = "example.write-flavors";
    ovstage_instance_t* stage = nullptr;
    ovstage_api_status_t status = ovstage_create_instance(&desc, &stage);
    check(nullptr, status, "create_instance");
    path_dictionary_instance_t* dict = getPathDictionary(stage);

    // ---- 1. batched writes ----
    {
        std::printf("== 1. batched writes ==\n");
        const char* paths[] = { "/World/Batch/A", "/World/Batch/B" };
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        const ovx_primpath_list_t list = queryFromPaths(stage, dict, paths, 2, nullptr, &query);
        const ovx_token_t heat = internToken(dict, "heat");
        const ovx_token_t tint = internToken(dict, "tint");

        // [snippet:batched-write-attributes]
        // ovstage_write_attributes lands several attribute columns in ONE
        // operation: one op id groups completion and the structural precreate
        // covers every entry (absent prims are created once for the whole
        // batch). It is a grouping, not an atomic transaction -- entries may
        // apply incrementally.
        const int64_t rows[] = { 2 };
        float heats[] = { 7.0f, 8.0f };
        float tints[] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f }; // 2 prims x 3 lanes
        DLTensor heatTensor = cpuFloatTensor(heats, rows, nullptr, 1);
        DLTensor tintTensor = cpuFloatTensor(tints, rows, nullptr, 3);
        ovstage_attribute_write_t writes[2]{};
        writes[0].attribute = { heat, {} };
        writes[0].data = writeData(&heatTensor);
        writes[1].attribute = { tint, {} };
        writes[1].data = writeData(&tintTensor, OVSTAGE_SEMANTIC_COLOR);
        ovstage_enqueue_result_t enq =
            ovstage_write_attributes(stage, query, writes, 2, /*ordinal*/ 1, OVSTAGE_PRIM_MODE_UPSERT);
        waitOp(stage, enq, "write_attributes");
        advanceFloor(stage, 1);
        // [/snippet:batched-write-attributes]

        float heatValues[2] = {};
        readColumn(stage, query, heat, /*endOrdinal*/ 1, heatValues, 2, 1);
        std::printf("heat: %.1f %.1f\n", heatValues[0], heatValues[1]);
        float tintValues[2 * 3] = {};
        readColumn(stage, query, tint, /*endOrdinal*/ 1, tintValues, 2, 3);
        std::printf("tint: (%.1f %.1f %.1f) (%.1f %.1f %.1f)\n", tintValues[0], tintValues[1], tintValues[2],
                    tintValues[3], tintValues[4], tintValues[5]);

        enq = ovstage_release_query(stage, query);
        waitOp(stage, enq, "release_query");
        path_dictionary_release_path_list_reference(dict, list);
    }

    // ---- 2. clone ----
    {
        std::printf("== 2. clone ==\n");
        const char* protoPath[] = { "/World/Proto/Rig" };
        ovstage_query_handle_t protoQuery = OVSTAGE_INVALID_QUERY_HANDLE;
        const ovx_primpath_list_t protoList = queryFromPaths(stage, dict, protoPath, 1, nullptr, &protoQuery);
        const ovx_token_t mass = internToken(dict, "mass");

        // [snippet:clone-and-requery]
        // ovstage_clone stamps the subtree under one source path onto N target
        // paths in a single ordinal-keyed call (the multi-environment pattern).
        // The source must exist; each target must not. Build the readback query
        // AFTER the clone, naming exactly the prims you expect -- clone changes
        // which prims exist, and a fresh path-list query pins the readback to
        // the clones themselves.
        float protoMass[] = { 5.0f };
        const int64_t oneRow[] = { 1 };
        DLTensor massTensor = cpuFloatTensor(protoMass, oneRow, nullptr, 1);
        ovstage_enqueue_result_t enq = ovstage_write_attribute(stage, protoQuery, { mass, {} }, /*ordinal*/ 2,
                                                               writeData(&massTensor), OVSTAGE_PRIM_MODE_UPSERT);
        waitOp(stage, enq, "write_attribute");
        advanceFloor(stage, 2);

        const char* targets[] = { "/World/Env0/Rig", "/World/Env1/Rig" };
        const ovx_string_t targetStrings[] = { ovxStr(targets[0]), ovxStr(targets[1]) };
        enq = ovstage_clone(stage, ovxStr(protoPath[0]), targetStrings, 2, /*ordinal*/ 3);
        waitOp(stage, enq, "clone");
        advanceFloor(stage, 3);

        ovx_primpath_t cloneOrder[2];
        ovstage_query_handle_t cloneQuery = OVSTAGE_INVALID_QUERY_HANDLE;
        const ovx_primpath_list_t cloneList = queryFromPaths(stage, dict, targets, 2, cloneOrder, &cloneQuery);
        float values[2] = {};
        bool present[2] = {};
        readLatestRows(stage, dict, cloneQuery, mass, /*endOrdinal*/ 3, cloneOrder, 2, values, present);
        std::printf("mass /World/Env0/Rig = %.1f\n", values[0]);
        std::printf("mass /World/Env1/Rig = %.1f\n", values[1]);
        // [/snippet:clone-and-requery]

        enq = ovstage_release_query(stage, cloneQuery);
        waitOp(stage, enq, "release_query");
        enq = ovstage_release_query(stage, protoQuery);
        waitOp(stage, enq, "release_query");
        path_dictionary_release_path_list_reference(dict, cloneList);
        path_dictionary_release_path_list_reference(dict, protoList);
    }

    {
        // [snippet:clone-duplicate-targets-c]
        // A target path may appear only once in a clone request.
        const ovx_string_t targets[] = { literal_to_ovx_string("/World/Duplicate/Rig"),
            literal_to_ovx_string("/World/Duplicate/Rig") };
        const auto operation = ovstage_clone(stage, literal_to_ovx_string("/World/Proto/Rig"), targets, 2, 4);
        if (operation.status != OVSTAGE_ERROR_INVALID_ARGUMENT)
        {
            std::fprintf(stderr, "Expected duplicate targets to be rejected\n");
            std::exit(EXIT_FAILURE);
        }
        // Submission failed, so there is no pending operation to wait for or release.
        std::printf("Duplicate targets rejected before cloning.\n");
        // Duplicate targets rejected before cloning.
        // [/snippet:clone-duplicate-targets-c]
    }

    {
        // [snippet:clone-async-report-c]
        const ovx_string_t target = literal_to_ovx_string("/World/Async/Rig");
        const auto operation = ovstage_clone(stage, literal_to_ovx_string("/World/Proto/Rig"), &target, 1, 4);
        check(stage, operation.status, "submit clone");
        // Independent application work can run here while the clone is pending.
        ovstage_op_wait_result_t wait{};
        check(stage, ovstage_wait_op(stage, operation.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait), "wait clone");
        checkWait(stage, wait, "clone completion");
        ovstage_clone_result_t report{};
        check(stage, ovstage_fetch_clone_result(stage, operation.op_index, OVSTAGE_TIMEOUT_INFINITE, &report),
              "fetch clone report");
        std::printf("Async clone copied %llu prim to %llu target\n",
            static_cast<unsigned long long>(report.prims_cloned), static_cast<unsigned long long>(report.target_count));
        // Async clone copied 1 prim to 1 target
        check(stage, ovstage_release_op(stage, operation.op_index), "release clone operation");
        // Advance the watermark before reading copied values.
        // [/snippet:clone-async-report-c]
    }

    {
        // [snippet:clone-specific-failure-c]
        const ovx_string_t target = literal_to_ovx_string("/World/MissingCopy");
        const auto operation = ovstage_clone(stage, literal_to_ovx_string("/World/Missing"), &target, 1, 4);
        check(stage, operation.status, "submit clone");
        // Fetch waits for completion and returns the clone-specific failure code.
        ovstage_clone_result_t report{};
        const auto status = ovstage_fetch_clone_result(stage, operation.op_index, OVSTAGE_TIMEOUT_INFINITE, &report);
        check(stage, ovstage_release_op(stage, operation.op_index), "release failed clone operation");
        if (status != OVSTAGE_ERROR_NOT_FOUND)
        {
            std::fprintf(stderr, "Expected a missing-source clone failure\n");
            std::exit(EXIT_FAILURE);
        }
        std::printf("Clone source was not found.\n");
        // Clone source was not found.
        // [/snippet:clone-specific-failure-c]
    }

    // ---- 3. pipelined submission ----
    {
        std::printf("== 3. pipelined submission ==\n");
        const char* paths[] = { "/World/Pipelined/A", "/World/Pipelined/B", "/World/Pipelined/C" };
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        const ovx_primpath_list_t list = queryFromPaths(stage, dict, paths, 3, nullptr, &query);
        const ovx_token_t sample = internToken(dict, "sample");

        // [snippet:pipelined-submission]
        // Every write so far enqueued and immediately waited, but the enqueue
        // itself is asynchronous: it returns an op id right away, so a
        // producer can submit several ordinals ahead WITHOUT waiting and keep
        // the CPU busy while the stage executes. This shows the programming
        // model, not a speedup -- current releases may execute enqueued
        // operations serially. Client-managed tensors must stay valid until
        // their op completes, hence one buffer/tensor per in-flight write.
        const int kAhead = 4;
        float batches[kAhead][3];
        DLTensor tensors[kAhead];
        ovstage_enqueue_result_t pending[kAhead];
        const int64_t rows[] = { 3 };
        for (int n = 0; n < kAhead; ++n)
        {
            for (int i = 0; i < 3; ++i)
                batches[n][i] = static_cast<float>(100 * (n + 1) + i);
            tensors[n] = cpuFloatTensor(batches[n], rows, nullptr, 1);
            pending[n] = ovstage_write_attribute(stage, query, { sample, {} }, /*ordinal*/ 4 + n,
                                                 writeData(&tensors[n]), OVSTAGE_PRIM_MODE_UPSERT);
            check(stage, pending[n].status, "write_attribute enqueue");
        }
        // [/snippet:pipelined-submission]
        std::printf("4 writes enqueued (ordinals 4..7) with zero waits; the CPU stays busy meanwhile\n");

        // [snippet:poll-wait-release]
        // Drain without blocking: poll each op (in submission order) with
        // ovstage_wait_op(timeout = 0). OVSTAGE_ERROR_TIMEOUT means "still
        // executing" -- a real application does more CPU work and polls
        // again (while pending, the wait result's lowest_pending_op_id names
        // the op the waited chain is stalled on). Completed ops are retired
        // with ovstage_release_op.
        for (int n = 0; n < kAhead; ++n)
        {
            ovstage_op_wait_result_t wait{};
            ovstage_api_status_t code;
            while ((code = ovstage_wait_op(stage, pending[n].op_index, /*timeout*/ 0, &wait)) ==
                   OVSTAGE_ERROR_TIMEOUT)
            {
                // still executing: do other CPU work, poll again
            }
            checkWait(stage, wait, "pipelined write");
            check(stage, code, "wait_op");
            status = ovstage_release_op(stage, pending[n].op_index);
            check(stage, status, "release_op");
        }
        advanceFloor(stage, 7);
        // [/snippet:poll-wait-release]
        std::printf("all 4 drained by zero-timeout polls and released; floor -> 7\n");

        float values[3] = {};
        readColumn(stage, query, sample, /*endOrdinal*/ 7, values, 3, 1);
        std::printf("latest sample after the pipeline: %.0f %.0f %.0f\n", values[0], values[1], values[2]);

        ovstage_enqueue_result_t enq = ovstage_release_query(stage, query);
        waitOp(stage, enq, "release_query");
        path_dictionary_release_path_list_reference(dict, list);
    }

    // Demonstrate write options over the same three prims in a known order.
    {
        const ovx_string_t names[] = { literal_to_ovx_string("/World/Writes/A"),
            literal_to_ovx_string("/World/Writes/B"), literal_to_ovx_string("/World/Writes/C") };
        ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, names, 3, &list), "create write paths");
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        check(stage, ovstage_query_from_path_list(stage, list, &query), "query write paths");
        {
            // [snippet:write-parameters-c]
            float values[] = { 10, 20, 30 };
            int64_t shape[] = { 3 };
            DLTensor tensor{};
            tensor.data = values;
            tensor.device = { kDLCPU, 0 };
            tensor.ndim = 1;
            tensor.dtype = { kDLFloat, 32, 1 };
            tensor.shape = shape;
            // Keep the source buffer and descriptors alive until completion.
            ovstage_write_data_t write{};
            write.tensors = &tensor;
            write.tensor_count = 1;
            write.is_array = false;

            waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("heat") },
                9, write, OVSTAGE_PRIM_MODE_UPSERT), "write heat");
            // [/snippet:write-parameters-c]
        }
        {
            // [snippet:write-fixed-shape-c]
            // Three rows, each containing one three-component vector.
            float values[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            int64_t shape[] = { 3, 3 };
            DLTensor tensor{};
            tensor.data = values;
            tensor.device = { kDLCPU, 0 };
            tensor.ndim = 2;
            tensor.dtype = { kDLFloat, 32, 1 };
            tensor.shape = shape;
            // Keep the source buffer and descriptors alive until completion.
            ovstage_write_data_t write{};
            write.tensors = &tensor;
            write.tensor_count = 1;
            write.is_array = false;

            waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("velocity") },
                10, write, OVSTAGE_PRIM_MODE_UPSERT), "write velocity");
            // [/snippet:write-fixed-shape-c]
        }
        {
            // [snippet:write-array-width-c]
            // Two float3 elements for A, one for B, and three for C.
            float a[] = { 0, 0, 0, 1, 0, 0 };
            float b[] = { 0, 1, 0 };
            float c[] = { 0, 0, 1, 1, 0, 1, 2, 0, 1 };
            int64_t lengths[] = { 2, 1, 3 };
            DLTensor tensors[] = {
                { a, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, &lengths[0], nullptr, 0 },
                { b, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, &lengths[1], nullptr, 0 },
                { c, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, &lengths[2], nullptr, 0 },
            };
            ovstage_write_data_t write{};
            write.tensors = tensors;
            write.tensor_count = 3;
            write.is_array = true; // Each tensor describes one prim's array.
            waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("points") },
                11, write, OVSTAGE_PRIM_MODE_UPSERT), "write points");
            // [/snippet:write-array-width-c]
        }
        {
            // [snippet:write-index-map-c]
            // A and B use row 0; C uses row 1.
            float values[] = { 100, 200 };
            int64_t shape[] = { 2 };
            DLTensor tensor{};
            tensor.data = values;
            tensor.device = { kDLCPU, 0 };
            tensor.ndim = 1;
            tensor.dtype = { kDLFloat, 32, 1 };
            tensor.shape = shape;
            // Keep the source buffer and descriptors alive until completion.
            ovstage_write_data_t write{};
            write.tensors = &tensor;
            write.tensor_count = 1;
            write.is_array = false;
            const uint32_t indexMap[] = { 0, 0, 1 };
            write.index_map = indexMap;
            write.count = 3;
            waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("heat") },
                12, write, OVSTAGE_PRIM_MODE_UPSERT), "write heat");
            // A = 100, B = 100, C = 200.
            // [/snippet:write-index-map-c]
        }
        {
            // [snippet:write-mask-c]
            // Only B receives a value, but the source still has a row for each prim.
            float values[] = { 0, 250, 0 };
            int64_t shape[] = { 3 };
            DLTensor tensor{};
            tensor.data = values;
            tensor.device = { kDLCPU, 0 };
            tensor.ndim = 1;
            tensor.dtype = { kDLFloat, 32, 1 };
            tensor.shape = shape;
            // Keep the source buffer and descriptors alive until completion.
            ovstage_write_data_t write{};
            write.tensors = &tensor;
            write.tensor_count = 1;
            write.is_array = false;
            const uint64_t mask[] = { 0b010 };
            write.mask = mask;
            write.count = 3;
            waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("heat") },
                13, write, OVSTAGE_PRIM_MODE_UPSERT), "write heat");
            // A = 100, B = 250, C = 200.
            // [/snippet:write-mask-c]
        }
        {
            // [snippet:write-count-c]
            // Address only A and B, the first two prims in query order.
            float values[] = { 300, 400 };
            int64_t shape[] = { 2 };
            DLTensor tensor{};
            tensor.data = values;
            tensor.device = { kDLCPU, 0 };
            tensor.ndim = 1;
            tensor.dtype = { kDLFloat, 32, 1 };
            tensor.shape = shape;
            // Keep the source buffer and descriptors alive until completion.
            ovstage_write_data_t write{};
            write.tensors = &tensor;
            write.tensor_count = 1;
            write.is_array = false;
            write.count = 2;
            waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("heat") },
                14, write, OVSTAGE_PRIM_MODE_UPSERT), "write heat");
            // A = 300, B = 400, C = 200.
            // [/snippet:write-count-c]
        }
        {
            // [snippet:write-semantic-c]
            // TOKEN_ID requires interned token identifiers, not character strings.
            const ovx_string_t names[] = { literal_to_ovx_string("steel"),
                literal_to_ovx_string("rubber"), literal_to_ovx_string("wood") };
            ovx_token_t values[3]{};
            checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, names, 3, values), "intern materials");
            int64_t shape[] = { 3 };
            DLTensor tensor{};
            tensor.data = values;
            tensor.device = { kDLCPU, 0 };
            tensor.ndim = 1;
            tensor.dtype = { kDLUInt, 64, 1 };
            tensor.shape = shape;
            // Keep the source buffer and descriptors alive until completion.
            ovstage_write_data_t write{};
            write.tensors = &tensor;
            write.tensor_count = 1;
            write.is_array = false;
            write.semantic = OVSTAGE_SEMANTIC_TOKEN_ID;
            waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("material") },
                15, write, OVSTAGE_PRIM_MODE_UPSERT), "write material");
            // [/snippet:write-semantic-c]
        }
        {
            // [snippet:write-watermark-c]
            // Make heat writes through ordinal 14 available for reads.
            const ovx_string_t name = literal_to_ovx_string("heat");
            ovx_token_t heat = OVX_INVALID_TOKEN;
            checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &heat), "intern heat");
            ovstage_write_floor_desc_t watermark{};
            watermark.ordinal = 14;
            watermark.scope = OVSTAGE_SCOPE_INCLUDE;
            watermark.attributes = &heat;
            watermark.attribute_count = 1;
            waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance heat watermark");
            // Finish all attribute writes through ordinal 15, including material.
            watermark = {};
            watermark.ordinal = 15;
            watermark.scope = OVSTAGE_SCOPE_ALL;
            waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance all watermarks");
            // [/snippet:write-watermark-c]
        }
        {
            // [snippet:read-copy-groups-c]
            const ovx_string_t name = literal_to_ovx_string("heat");
            ovx_token_t attribute = OVX_INVALID_TOKEN;
            checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "intern attribute");
            ovstage_ordinal_range_t range{};
            range.end_ordinal = 15;
            ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
            waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read attributes");
            std::vector<float> saved;
            for (;;)
            {
                ovstage_read_group_t group{};
                const auto status = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
                if (status == OVSTAGE_ERROR_END_OF_ITERATION)
                    break;
                check(stage, status, "fetch group");
                const DLTensor& tensor = group.data.tensors[0];
                const auto* values = reinterpret_cast<const float*>(
                    static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
                for (uint32_t i = 0; i < group.prims.count; ++i)
                {
                    const uint32_t row = group.data.index_map ? group.data.index_map[i] : i;
                    saved.push_back(values[row]); // Copy the value before releasing its group.
                }
                check(stage, ovstage_release_group(stage, &group), "release group");
            }
            waitOp(stage, ovstage_release_read(stage, read), "release read");
            // Copied values remain valid after both groups and the read are released.
            std::sort(saved.begin(), saved.end());
            std::printf("Copied heat values:");
            for (float value : saved)
                std::printf(" %.1f", value);
            std::printf("\n");
            // Copied heat values: 200.0 300.0 400.0
            // [/snippet:read-copy-groups-c]
        }
        {
            // [snippet:read-fixed-vectors-c]
            const ovx_string_t name = literal_to_ovx_string("velocity");
            ovx_token_t attribute = OVX_INVALID_TOKEN;
            checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "intern attribute");
            ovstage_ordinal_range_t range{};
            range.end_ordinal = 15;
            ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
            waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read attributes");

            for (;;)
            {
                ovstage_read_group_t group{};
                const auto status = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
                if (status == OVSTAGE_ERROR_END_OF_ITERATION)
                    break;
                check(stage, status, "fetch group");
                // Raw tensor shape is (N, 3), with scalar float32 lanes.
                const DLTensor& tensor = group.data.tensors[0];
                const auto* values = reinterpret_cast<const float*>(
                    static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
                for (uint32_t i = 0; i < group.prims.count; ++i)
                {
                    const uint32_t row = group.data.index_map ? group.data.index_map[i] : i;
                    const float* vector = values + row * 3;
                    // Use vector[0..2] here, while the group still owns the buffer.
                    (void)vector;
                }
                check(stage, ovstage_release_group(stage, &group), "release group");
            }
            waitOp(stage, ovstage_release_read(stage, read), "release read");
            // [/snippet:read-fixed-vectors-c]
        }
        {
            // [snippet:read-change-range-c]
            const ovx_string_t name = literal_to_ovx_string("heat");
            ovx_token_t attribute = OVX_INVALID_TOKEN;
            checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "intern attribute");
            ovstage_ordinal_range_t range{};
            // A and B changed at 14; C last changed at 12.
            range.has_start_ordinal = true;
            range.start_ordinal = 14;
            range.end_ordinal = 14;
            ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
            waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read attributes");
            std::vector<float> saved;
            for (;;)
            {
                ovstage_read_group_t group{};
                const auto status = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
                if (status == OVSTAGE_ERROR_END_OF_ITERATION)
                    break;
                check(stage, status, "fetch group");
                if (group.is_delete)
                {
                    // Deletions carry no value tensor; still release their group.
                    check(stage, ovstage_release_group(stage, &group), "release deletion group");
                    continue;
                }
                const DLTensor& tensor = group.data.tensors[0];
                const auto* values = reinterpret_cast<const float*>(
                    static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
                for (uint32_t i = 0; i < group.prims.count; ++i)
                {
                    const uint32_t row = group.data.index_map ? group.data.index_map[i] : i;
                    saved.push_back(values[row]); // Copy the value before releasing its group.
                }
                check(stage, ovstage_release_group(stage, &group), "release group");
            }
            waitOp(stage, ovstage_release_read(stage, read), "release read");
            std::sort(saved.begin(), saved.end());
            std::printf("Heat values changed at 14:");
            for (float value : saved)
                std::printf(" %.1f", value);
            std::printf("\n");
            // Heat values changed at 14: 300.0 400.0
            // [/snippet:read-change-range-c]
        }
        {
            // [snippet:read-retained-records-c]
            // Query the boundary before relying on exact membership of an old interval.
            ovstage_ordinal_query_handle_t ordinalQuery{};
            waitOp(stage, ovstage_get_oldest_preserved_ordinal(stage, &ordinalQuery), "query oldest ordinal");
            ovstage_ordinal_t oldest = 0;
            check(stage, ovstage_fetch_ordinal(stage, ordinalQuery, OVSTAGE_TIMEOUT_INFINITE, &oldest), "fetch ordinal");
            waitOp(stage, ovstage_release_ordinal_query(stage, ordinalQuery), "release ordinal query");
            const ovstage_ordinal_t requestedStart = 14;
            if (requestedStart < oldest)
            {
                std::fprintf(stderr, "Change records for this interval are no longer exact\n");
                std::exit(EXIT_FAILURE);
            }
            // A subsequent range read can still fail if a newer write replaced its values.
            // [/snippet:read-retained-records-c]
        }
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, list);
    }

    ovstage_destroy_instance(stage);
    return 0;
}
