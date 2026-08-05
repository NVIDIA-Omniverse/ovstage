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
// A producer writes a "temperature" column one tick at a time, sealing each
// tick by advancing the write floor; a pull-based consumer polls that floor
// and range-reads only the delta -- "what changed since the last ordinal it
// saw" -- including a whole-prim delete that arrives as an is_delete
// tombstone group. Run with no arguments for the deterministic interleaved
// mode, or with --threads for the concurrent mode (output varies run to run).
//
// Expected output: see README.md. Snippet markers are referenced by the
// skills under ../../../skills/ -- keep them intact.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../common/ovstage_example_utils.h"

// Four "sensor" prims, created on demand by the first UPSERT write that
// targets them. S0..S3 in the prints = last path component.
static const size_t kSensorCount = 4;
static const char* kSensorPaths[kSensorCount] = {
    "/World/Sensors/S0",
    "/World/Sensors/S1",
    "/World/Sensors/S2",
    "/World/Sensors/S3",
};

// Six ticks, each writing a rotating two-prim subset -- a CHANGING subset per
// tick is what makes the consumer's change membership interesting.
static const int kTickCount = 6;
static const int kTickSubsets[kTickCount][2] = {
    { 0, 1 },   // tick 1
    { 1, 2 },   // tick 2
    { 2, 0 },   // tick 3   <- first consumer catch-up reads [1, 3]
    { 3, 0 },   // tick 4
    { -1, -1 }, // tick 5: whole-prim delete of S2 (no value writes)
    { 1, 3 },   // tick 6   <- second consumer catch-up reads [4, 6]
};
static const int kDeleteTick = 5;
static const size_t kDeletedSensor = 2;
static const int64_t kScalarShape[] = { 1 };
static const int64_t kScalarStrides[] = { 1 };

// State shared by both roles: the instance, the interned attribute token, the
// consumer's query over all four sensors, and one single-prim query per sensor
// so each producer tick can target just its subset.
struct Example
{
    ovstage_instance_t* stage = nullptr;
    path_dictionary_instance_t* dict = nullptr;
    ovx_token_t temperature = OVX_INVALID_TOKEN;
    ovstage_query_handle_t allQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_query_handle_t sensorQueries[kSensorCount] = {};
};

// Non-exiting sibling of waitOp: drive the op to completion and report any
// failure, but RETURN it instead of exiting. In --threads mode a producer
// write can be rejected when it overlaps an outstanding consumer read (an
// expected race under load), and the failing role -- EITHER role -- must
// coordinate a shutdown with its peer thread: std::exit from one thread would
// tear the process down under the other's blocking ovstage wait. Both roles'
// ovstage calls therefore route through this and tryCheck below.
static bool tryWaitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq, const char* what)
{
    if (enq.status != OVSTAGE_OK)
    {
        std::fprintf(stderr, "ovstage %s failed (code %u): %s\n", what, enq.status,
                     ovstage_get_error_string(stage, enq.status));
        return false;
    }
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t code = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    for (size_t i = 0; i < wait.error_op_id_count; ++i)
    {
        const ovx_string_t e = ovstage_get_last_op_error(stage, wait.error_op_ids[i]);
        std::fprintf(stderr, "ovstage %s op %llu failed: %.*s\n", what,
                     (unsigned long long)wait.error_op_ids[i], (int)e.length, e.ptr ? e.ptr : "");
    }
    ovstage_release_op(stage, enq.op_index);
    if (code != OVSTAGE_OK)
        std::fprintf(stderr, "ovstage %s wait failed (code %u): %s\n", what, code,
                     ovstage_get_error_string(stage, code));
    return code == OVSTAGE_OK && wait.error_op_id_count == 0;
}

// Non-exiting sibling of check, for the synchronous fetch/release calls on
// the consumer path: report a failed status and return it, for the same
// coordinated-shutdown reason as tryWaitOp.
static bool tryCheck(ovstage_instance_t* stage, ovstage_api_status_t status, const char* what)
{
    if (status == OVSTAGE_OK)
        return true;
    std::fprintf(stderr, "ovstage %s failed (code %u): %s\n", what, status,
                 ovstage_get_error_string(stage, status));
    return false;
}

// [snippet:producer-tick]
// One producer value tick: write this tick's subset at ordinal `tick` (several
// writes may land at the same ordinal), then seal the tick by advancing the
// GLOBAL write floor to it. The floor is the producer's publish signal -- data
// at ordinals <= floor is sealed and will never change -- and the application
// owns the ordinal counter; the store never mints ordinals. Returns false on
// a rejected write (reported by tryWaitOp) so the caller decides the policy.
static bool producerValueTick(const Example& ex, int tick)
{
    const int* sensors = kTickSubsets[tick - 1];
    float values[2] = { 10.0f * tick + sensors[0], 10.0f * tick + sensors[1] };
    for (int slot = 0; slot < 2; ++slot)
    {
        DLTensor tensor = cpuFloatTensor(&values[slot], kScalarShape, kScalarStrides, 1);
        ovstage_write_data_t write = writeData(&tensor);
        ovstage_enqueue_result_t enq =
            ovstage_write_attribute(ex.stage, ex.sensorQueries[sensors[slot]], { ex.temperature, {} },
                                    static_cast<ovstage_ordinal_t>(tick), write, OVSTAGE_PRIM_MODE_UPSERT);
        if (!tryWaitOp(ex.stage, enq, "write_attribute"))
            return false;
    }

    ovstage_write_floor_desc_t floorDesc{};
    floorDesc.ordinal = static_cast<ovstage_ordinal_t>(tick);
    floorDesc.scope = OVSTAGE_SCOPE_ALL;
    ovstage_enqueue_result_t enq = ovstage_advance_write_floor(ex.stage, &floorDesc);
    if (!tryWaitOp(ex.stage, enq, "advance_write_floor"))
        return false;

    std::printf("producer tick %d: wrote S%d = %.1f, S%d = %.1f (floor -> %d)\n", tick, sensors[0], values[0],
                sensors[1], values[1], tick);
    return true;
}
// [/snippet:producer-tick]

// [snippet:tombstone-delete]
// The tombstone tick: delete one sensor prim ENTIRELY. delete_attributes with
// an EMPTY attribute list (NULL, 0) is the whole-prim delete; like any write
// it is ordinal-keyed and sealed by the same floor advance. Consumers see it
// as an is_delete read group (no tensors). Returns false on a rejected write
// (reported by tryWaitOp) so the caller decides the policy.
static bool producerDeleteTick(const Example& ex, int tick)
{
    ovstage_enqueue_result_t enq =
        ovstage_delete_attributes(ex.stage, ex.sensorQueries[kDeletedSensor], /*attributes*/ nullptr,
                                  /*attribute_count*/ 0, static_cast<ovstage_ordinal_t>(tick));
    if (!tryWaitOp(ex.stage, enq, "delete_attributes"))
        return false;

    ovstage_write_floor_desc_t floorDesc{};
    floorDesc.ordinal = static_cast<ovstage_ordinal_t>(tick);
    floorDesc.scope = OVSTAGE_SCOPE_ALL;
    enq = ovstage_advance_write_floor(ex.stage, &floorDesc);
    if (!tryWaitOp(ex.stage, enq, "advance_write_floor"))
        return false;

    std::printf("producer tick %d: deleted S%zu entirely (floor -> %d)\n", tick, kDeletedSensor, tick);
    return true;
}
// [/snippet:tombstone-delete]

// Value ticks write their subset; the delete tick tombstones S2 instead.
// False means the tick's write was rejected and already reported.
static bool runProducerTick(const Example& ex, int tick)
{
    if (tick == kDeleteTick)
        return producerDeleteTick(ex, tick);
    return producerValueTick(ex, tick);
}

// [snippet:poll-write-floor]
// The consumer's poll: ask for the GLOBAL write floor (attribute = zero token
// + empty string), fetch the ordinal, release the ordinal-query handle. This
// is the producer's publish cursor -- everything at or below the floor is
// sealed for range membership. Payload storage is latest-only, so a later
// change to the same selected (attribute, path) can make the fixed-range read
// return OUT_OF_RANGE.
// Returns false on a failed op (already reported) so the caller decides the
// policy; the handle is released either way.
static bool fetchGlobalFloor(ovstage_instance_t* stage, ovstage_ordinal_t* outFloor)
{
    ovx_string_or_token_t globalAttr{}; // token 0 + empty string selects the global floor
    ovstage_ordinal_query_handle_t handle = OVSTAGE_INVALID_ORDINAL_QUERY_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_get_attribute_write_floor(stage, globalAttr, &handle);
    if (!tryWaitOp(stage, enq, "get_attribute_write_floor"))
        return false;
    const ovstage_api_status_t status = ovstage_fetch_ordinal(stage, handle, OVSTAGE_TIMEOUT_INFINITE, outFloor);
    bool ok = tryCheck(stage, status, "fetch_ordinal");
    enq = ovstage_release_ordinal_query(stage, handle);
    ok = tryWaitOp(stage, enq, "release_ordinal_query") && ok;
    return ok;
}
// [/snippet:poll-write-floor]

// [snippet:consume-delta]
// Resolve the local-th prim of a read group to its display name (the last
// path component, "S0".."S3"). The list-relative position is
// prims.index_map[local] when an index_map is present, else
// prims.offset + local; the group's list handle is the query's pinned copy
// (not necessarily the query's own path-list handle), so look the path id up
// in the group's list, then decompose it into its name tokens and resolve the
// last one.
static std::string sensorName(const Example& ex, const ovstage_prim_group_t& prims, uint32_t local)
{
    const uint32_t listIndex = prims.index_map ? prims.index_map[local] : prims.offset + local;
    ovx_primpath_t pathId = OVX_INVALID_PRIMPATH;
    size_t fetched = 0;
    ovx_api_result_t ovxResult =
        path_dictionary_get_paths_from_path_list(ex.dict, prims.list, listIndex, 1, &pathId, &fetched);
    checkOvx(ex.dict, ovxResult, "get_paths_from_path_list");
    ovx_token_t tokenBuffer[16] = {};
    ovx_token_t* tokensPerPath[1] = { nullptr };
    size_t numTokens[1] = { 0 };
    size_t numProcessed = 0;
    ovxResult =
        path_dictionary_get_tokens_from_paths(ex.dict, &pathId, 1, tokenBuffer,
                                              sizeof(tokenBuffer) / sizeof(tokenBuffer[0]), tokensPerPath, numTokens,
                                              &numProcessed);
    checkOvx(ex.dict, ovxResult, "get_tokens_from_paths");
    if (numProcessed == 0 || numTokens[0] == 0 || !tokensPerPath[0])
    {
        std::fprintf(stderr, "could not decompose a prim path into tokens\n");
        std::exit(EXIT_FAILURE);
    }
    ovx_string_t name{};
    ovxResult = path_dictionary_get_strings_from_tokens(ex.dict, &tokensPerPath[0][numTokens[0] - 1], 1, &name);
    checkOvx(ex.dict, ovxResult, "get_strings_from_tokens");
    return std::string(name.ptr ? name.ptr : "", name.length);
}

// What one fetched change group yields, as-is: the covered prims resolved to
// display names, plus current values for a successful value group. A tombstone
// group (is_delete = true, no tensors) carries no values -- the listed prims
// were deleted somewhere in the delta range.
struct DeltaGroup
{
    ovstage_ordinal_t ordinal = 0;
    bool isDelete = false;
    std::vector<std::string> names;
    std::vector<float> values; // one per covered prim; empty for a tombstone group
};

// Range-read [startOrdinal, endOrdinal] over all four sensors and return the
// fetched groups as-is in outGroups. The EXPLICIT-BEGIN range makes it a
// delta ("what changed since startOrdinal - 1"); an open begin would be a
// snapshot read. Each fetched group covers the prims that changed in the
// range for one attribute. If a selected (attribute, path) changed again after
// endOrdinal, the read returns OUT_OF_RANGE. The value row for the local-th
// prim honors data.index_map when present.
// Returns false on a failed op (already reported) so the caller decides the
// policy; the group and read handles are released either way.
static bool consumeDelta(const Example& ex, ovstage_ordinal_t startOrdinal, ovstage_ordinal_t endOrdinal,
                         std::vector<DeltaGroup>* outGroups)
{
    ovstage_ordinal_range_t delta{};
    delta.start_ordinal = startOrdinal; // explicit begin: only changes AT or AFTER startOrdinal
    delta.end_ordinal = endOrdinal;     // inclusive end of the selected changes
    delta.has_start_ordinal = true;

    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_read_attributes(ex.stage, ex.allQuery, &ex.temperature, 1, delta, &read);
    if (!tryWaitOp(ex.stage, enq, "read_attributes"))
        return false;

    bool ok = true;
    for (;;)
    {
        ovstage_read_group_t group{};
        const ovstage_api_status_t fetch = ovstage_fetch_read_next(ex.stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
        if (fetch == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        ok = tryCheck(ex.stage, fetch, "fetch_read_next");
        if (!ok)
            break;

        DeltaGroup rows;
        rows.ordinal = group.ordinal;
        rows.isDelete = group.is_delete;
        for (uint32_t local = 0; local < group.prims.count; ++local)
            rows.names.push_back(sensorName(ex, group.prims, local));
        if (!group.is_delete)
        {
            if (group.data.tensor_count != 1 || !group.data.tensors[0].data)
            {
                std::fprintf(stderr, "unexpected read-group layout (want one tensor row per covered prim)\n");
                ok = false; // shut down like any failed op: the group still gets released below
            }
            else
            {
                const float* cells = static_cast<const float*>(group.data.tensors[0].data);
                for (uint32_t local = 0; local < group.prims.count; ++local)
                {
                    const uint32_t row = group.data.index_map ? group.data.index_map[local] : local;
                    rows.values.push_back(cells[row]);
                }
            }
        }
        if (ok)
            outGroups->push_back(rows);
        const ovstage_api_status_t status = ovstage_release_group(ex.stage, &group);
        ok = tryCheck(ex.stage, status, "release_group") && ok;
        if (!ok)
            break;
    }
    enq = ovstage_release_read(ex.stage, read);
    ok = tryWaitOp(ex.stage, enq, "release_read") && ok;
    return ok;
}

// One consumer catch-up (both catch-ups and the threaded consumer run this):
// fetch the global floor; if it moved past last_seen, consume the delta
// [last_seen + 1, floor] and advance the cursor. False means an op failed
// and was already reported.
static bool consumerCatchUp(const Example& ex, ovstage_ordinal_t* lastSeen)
{
    ovstage_ordinal_t floor = 0;
    if (!fetchGlobalFloor(ex.stage, &floor))
        return false;
    if (floor <= *lastSeen)
        return true; // nothing sealed since the last catch-up

    std::vector<DeltaGroup> groups;
    if (!consumeDelta(ex, *lastSeen + 1, floor, &groups))
        return false;

    // Example plumbing: report the range this catch-up covered, each fetched
    // group (value or tombstone), and the batch size in prim changes.
    std::printf("consumer: floor %llu, last_seen %llu -> reading delta [%llu, %llu]\n", (unsigned long long)floor,
                (unsigned long long)*lastSeen, (unsigned long long)(*lastSeen + 1), (unsigned long long)floor);
    uint32_t changes = 0;
    for (const DeltaGroup& group : groups)
    {
        if (group.isDelete)
        {
            std::printf("  tombstone group (ordinal %llu):", (unsigned long long)group.ordinal);
            for (size_t i = 0; i < group.names.size(); ++i)
                std::printf(i ? ", %s" : " %s", group.names[i].c_str());
            std::printf(" deleted\n");
        }
        else
        {
            std::printf("  value group (ordinal %llu):", (unsigned long long)group.ordinal);
            for (size_t i = 0; i < group.names.size(); ++i)
                std::printf(i ? ", %s = %.1f" : " %s = %.1f", group.names[i].c_str(), group.values[i]);
            std::printf("\n");
        }
        changes += static_cast<uint32_t>(group.names.size());
    }
    std::printf("  delta batch: %u prim changes\n", changes);
    std::printf("consumer: last_seen -> %llu\n", (unsigned long long)floor);

    *lastSeen = floor; // the consumer's cursor: the next catch-up starts at floor + 1
    return true;
}
// [/snippet:consume-delta]

// [snippet:threaded-producer-consumer]
// Concurrent mode (--threads): producer and consumer run on the SAME instance
// from two threads -- every ovstage_api slot is thread-safe when called on a
// shared instance (see the Thread Safety section in ovstage_api.h). The
// consumer reads change membership only through a floor it fetched. A pending
// overlapping write makes the read fail with OP_FAILED. Once committed, a later
// change to the same selected (attribute, path) after the requested end makes
// the read fail with OUT_OF_RANGE whether or not that later change is sealed. A
// selected in-range unsealed change instead reports WRITE_FLOOR_VIOLATION. When
// no overlap rejection occurs, only the batching varies, which is why this
// mode's output is not part of the expected-output block. This demo treats any
// race failure as terminal: a stalled floor would hang the consumer, so either
// role flags the failure and both shut down.
static bool runThreaded(const Example& ex)
{
    std::atomic<bool> failed{ false };
    std::thread producer(
        [&ex, &failed]
        {
            for (int tick = 1; tick <= kTickCount && !failed; ++tick)
            {
                if (!runProducerTick(ex, tick))
                {
                    failed = true; // flag first: the consumer polls this
                    std::fprintf(stderr, "producer failed: stopping both roles\n");
                    return;
                }
                // The producer's "own rate": stall between ticks so catch-ups interleave.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });

    ovstage_ordinal_t lastSeen = 0;
    while (lastSeen < static_cast<ovstage_ordinal_t>(kTickCount) && !failed)
    {
        if (!consumerCatchUp(ex, &lastSeen))
        {
            failed = true; // flag first: the producer polls this between ticks
            std::fprintf(stderr, "consumer failed: stopping both roles\n");
            break;
        }
        if (lastSeen < static_cast<ovstage_ordinal_t>(kTickCount))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    producer.join();
    return !failed;
}
// [/snippet:threaded-producer-consumer]

int main(int argc, char** argv)
{
    const bool threaded = (argc > 1) && (std::strcmp(argv[1], "--threads") == 0);

    // ---- 1. setup: instance, temperature token, queries ----
    ovstage_instance_desc_t desc{};
    desc.name = "example.producer-consumer";
    Example ex;
    ovstage_api_status_t status = ovstage_create_instance(&desc, &ex.stage);
    check(nullptr, status, "create_instance");
    ex.dict = getPathDictionary(ex.stage);

    ovx_string_t attrName = literal_to_ovx_string("temperature");
    ovx_api_result_t ovxResult = path_dictionary_create_tokens_from_strings(ex.dict, &attrName, 1, &ex.temperature);
    checkOvx(ex.dict, ovxResult, "intern-token");

    ovx_string_t sensorPaths[kSensorCount];
    for (size_t i = 0; i < kSensorCount; ++i)
        sensorPaths[i] = ovx_string_t{ kSensorPaths[i], std::strlen(kSensorPaths[i]) };

    ovx_primpath_list_t allPathList = OVX_INVALID_PRIMPATH_LIST;
    ovxResult = path_dictionary_create_path_list_from_strings(ex.dict, sensorPaths, kSensorCount, &allPathList);
    checkOvx(ex.dict, ovxResult, "path-list");
    status = ovstage_query_from_path_list(ex.stage, allPathList, &ex.allQuery);
    check(ex.stage, status, "query_from_path_list");
    ovx_primpath_list_t sensorPathLists[kSensorCount] = {};
    for (size_t i = 0; i < kSensorCount; ++i)
    {
        ovxResult = path_dictionary_create_path_list_from_strings(ex.dict, &sensorPaths[i], 1, &sensorPathLists[i]);
        checkOvx(ex.dict, ovxResult, "path-list");
        status = ovstage_query_from_path_list(ex.stage, sensorPathLists[i], &ex.sensorQueries[i]);
        check(ex.stage, status, "query_from_path_list");
    }

    bool ok = true;
    if (threaded)
    {
        // ---- concurrent mode (--threads): both roles on one shared instance ----
        ok = runThreaded(ex);
    }
    else
    {
        ovstage_ordinal_t lastSeen = 0;

        // ---- 2. producer ticks 1..3 ----
        // A failed op is unexpected without a concurrent peer, so the
        // deterministic mode keeps the examples' fail-fast policy (the
        // try-helpers already reported the error).
        for (int tick = 1; tick <= 3; ++tick)
            if (!runProducerTick(ex, tick))
                return EXIT_FAILURE;

        // ---- 3. consumer catch-up: reads delta [1, 3] ----
        if (!consumerCatchUp(ex, &lastSeen))
            return EXIT_FAILURE;

        // ---- 4. producer ticks 4..6 (tick 5 deletes S2) ----
        for (int tick = 4; tick <= kTickCount; ++tick)
            if (!runProducerTick(ex, tick))
                return EXIT_FAILURE;

        // ---- 5. consumer catch-up: reads delta [4, 6] ----
        if (!consumerCatchUp(ex, &lastSeen))
            return EXIT_FAILURE;
    }

    // Release every handle, then destroy: ovstage_destroy_instance requires
    // all ops and handles released first.
    for (size_t i = 0; i < kSensorCount; ++i)
    {
        ovstage_enqueue_result_t enq = ovstage_release_query(ex.stage, ex.sensorQueries[i]);
        waitOp(ex.stage, enq, "release_query");
    }
    ovstage_enqueue_result_t enq = ovstage_release_query(ex.stage, ex.allQuery);
    waitOp(ex.stage, enq, "release_query");
    for (size_t i = 0; i < kSensorCount; ++i)
        path_dictionary_release_path_list_reference(ex.dict, sensorPathLists[i]);
    path_dictionary_release_path_list_reference(ex.dict, allPathList);
    ovstage_destroy_instance(ex.stage);
    return ok ? 0 : EXIT_FAILURE;
}
