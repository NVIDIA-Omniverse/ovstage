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
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
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

// Concurrent mode (--threads) only. Its overlap phase runs at the sealed
// ordinal kOverlapTick: the consumer keeps that tick's temperature read group
// borrowed while the producer updates S0's "humidity" -- an unrelated
// attribute, created before the threads start -- at the next ordinal.
// kWatchdog is a diagnostic timeout for the coordination waits (waiting for
// the peer); it is not a process-wide execution deadline.
static const int kOverlapTick = 3;
static const size_t kHumiditySensor = 0;
static const std::chrono::seconds kWatchdog(30);

// State shared by both roles: the instance, the interned attribute tokens, the
// consumer's query over all four sensors, and one single-prim query per sensor
// so each producer tick can target just its subset.
struct Example
{
    ovstage_instance_t* stage = nullptr;
    path_dictionary_instance_t* dict = nullptr;
    ovx_token_t temperature = OVX_INVALID_TOKEN;
    ovx_token_t humidity = OVX_INVALID_TOKEN; // written only by the concurrent mode's overlap phase
    ovstage_query_handle_t allQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_query_handle_t sensorQueries[kSensorCount] = {
        OVSTAGE_INVALID_QUERY_HANDLE, OVSTAGE_INVALID_QUERY_HANDLE,
        OVSTAGE_INVALID_QUERY_HANDLE, OVSTAGE_INVALID_QUERY_HANDLE
    };
};

// Non-exiting sibling of waitOp: drive the op to completion and report any
// failure, but RETURN it instead of exiting. In --threads mode a failure in
// EITHER role must coordinate a shutdown with the peer thread: std::exit from
// one thread would tear the process down under the other's blocking ovstage
// wait. Both roles' ovstage calls therefore route through this and tryCheck
// below.
static bool tryWaitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq, const char* what)
{
    if (enq.status != OVSTAGE_OK)
    {
        // A synchronous rejection leaves its detail in the thread-local
        // last-error slot, valid until the next API call on this thread.
        const ovx_string_t detail = ovstage_get_last_error();
        if (detail.ptr && detail.length)
            std::fprintf(stderr, "ovstage %s failed (code %u): %.*s\n", what, enq.status, (int)detail.length,
                         detail.ptr);
        else
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
                     (unsigned long long)wait.error_op_ids[i].raw_id, (int)e.length, e.ptr ? e.ptr : "");
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
// GLOBAL write floor to it. The floor tells the consumer which ordinals are
// sealed against further writes. Values may still change at newer ordinals.
// The application owns the ordinal counter; the store never mints ordinals.
// Returns false on a rejected write (reported by tryWaitOp) so the caller decides
// the policy.
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
// tells the consumer which ordinals are sealed for change-range queries.
// Payload storage is latest-only, so a later change to the same selected
// (attribute, path) can make the fixed-range read
// return OUT_OF_RANGE.
// Returns false on a failed op (already reported) so the caller decides the
// policy; the handle is released either way.
static bool fetchGlobalFloor(ovstage_instance_t* stage, ovstage_ordinal_t* outFloor)
{
    ovx_token_or_string_t globalAttr{}; // token 0 + empty string selects the global floor
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
/// Return the display name (S0..S3) for the local-th prim in a valid read group, using ex.dict to resolve its path.
/// Apply prims.index_map when present, otherwise prims.offset + local, and look up the path in the group's own list.
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
        fail("could not decompose a prim path into tokens");
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

// Copy one fetched group into application-owned storage. The value row for
// the local-th prim honors data.index_map when present. Copying is what lets
// the caller release the group promptly: a borrowed group blocks every
// overlapping write for as long as it is held.
static bool copyGroup(const Example& ex, const ovstage_read_group_t& group, DeltaGroup* out)
{
    if (group.column_count != 1 || !group.columns)
    {
        std::fprintf(stderr, "single-attribute read returned an unexpected column layout\n");
        return false;
    }
    const ovstage_read_column_t& column = group.columns[0];
    out->ordinal = column.ordinal;
    out->isDelete = column.is_delete;
    out->names.clear();
    out->values.clear();
    for (uint32_t local = 0; local < group.prims.count; ++local)
        out->names.push_back(sensorName(ex, group.prims, local));
    if (column.is_delete)
        return true;
    if (column.data.tensor_count != 1 || !column.data.tensors[0].data)
    {
        std::fprintf(stderr, "unexpected read-group layout (want one tensor row per covered prim)\n");
        return false;
    }
    const float* cells = static_cast<const float*>(column.data.tensors[0].data);
    for (uint32_t local = 0; local < group.prims.count; ++local)
    {
        const uint32_t row = column.data.index_map ? column.data.index_map[local] : local;
        out->values.push_back(cells[row]);
    }
    return true;
}

// Range-read [startOrdinal, endOrdinal] over all four sensors and return the
// fetched groups, copied, in outGroups. The EXPLICIT-BEGIN range makes it a
// delta ("what changed since startOrdinal - 1"); an open begin would be a
// snapshot read. Each fetched group covers the prims that changed in the
// range for one attribute. If a selected (attribute, path) changed again after
// endOrdinal, the read returns OUT_OF_RANGE.
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
        size_t fetchedGroupCount = 0;
        ovstage_read_group_t group{};
        const ovstage_api_status_t fetch =
            ovstage_fetch_read_next(ex.stage, read, OVSTAGE_TIMEOUT_INFINITE, &group, 1, &fetchedGroupCount);
        if (fetch == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        ok = tryCheck(ex.stage, fetch, "fetch_read_next");
        if (!ok)
            break;

        DeltaGroup rows;
        ok = copyGroup(ex, group, &rows); // on failure, shut down like any failed op: the group is still released
        if (ok)
            outGroups->push_back(rows);
        const ovstage_api_status_t status = ovstage_release_groups(ex.stage, &group, 1);
        ok = tryCheck(ex.stage, status, "release_groups") && ok;
        if (!ok)
            break;
    }
    enq = ovstage_release_read(ex.stage, read);
    ok = tryWaitOp(ex.stage, enq, "release_read") && ok;
    return ok;
}

// Example plumbing: report the range a catch-up covered, each copied group
// (value or tombstone), and the batch size in prim changes.
static void reportDelta(const std::vector<DeltaGroup>& groups, ovstage_ordinal_t floor, ovstage_ordinal_t lastSeen)
{
    std::printf("consumer: floor %llu, last_seen %llu -> reading delta [%llu, %llu]\n", (unsigned long long)floor,
                (unsigned long long)lastSeen, (unsigned long long)(lastSeen + 1), (unsigned long long)floor);
    uint32_t changes = 0;
    for (const DeltaGroup& group : groups)
    {
        if (group.isDelete)
        {
            std::printf("  tombstone group (ordinal %lld):", (long long)group.ordinal);
            for (size_t i = 0; i < group.names.size(); ++i)
                std::printf(i ? ", %s" : " %s", group.names[i].c_str());
            std::printf(" deleted\n");
        }
        else
        {
            std::printf("  value group (ordinal %lld):", (long long)group.ordinal);
            for (size_t i = 0; i < group.names.size(); ++i)
                std::printf(i ? ", %s = %.1f" : " %s = %.1f", group.names[i].c_str(), group.values[i]);
            std::printf("\n");
        }
        changes += static_cast<uint32_t>(group.names.size());
    }
    std::printf("  delta batch: %u prim changes\n", changes);
    std::printf("consumer: last_seen -> %llu\n", (unsigned long long)floor);
}

// One consumer catch-up (the deterministic mode runs this): fetch the global
// floor; if it moved past last_seen, consume the delta [last_seen + 1, floor],
// report it and advance the cursor. False means an op failed and was already
// reported.
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
    reportDelta(groups, floor, *lastSeen);

    *lastSeen = floor; // the consumer's cursor: the next catch-up starts at floor + 1
    return true;
}
// [/snippet:consume-delta]

// [snippet:threaded-producer-consumer]
// Shared state of the concurrent mode (--threads); see runThreaded. `guard`
// protects the temperature access scope -- an application choice for this
// example's four-sensor access set, not a stage-wide requirement -- and
// `progress` publishes the newest sealed tick. `failed` is set by whichever role fails; fail() wakes every
// waiter so both roles shut down. The one-time overlap phase has its own lock
// (the consumer holds `guard` while it waits for the unrelated write) plus the
// observed order of its steps.
struct Coordination
{
    std::mutex guard;
    std::condition_variable progress;
    ovstage_ordinal_t published = 0; // newest sealed tick
    bool done = false;               // the producer finished
    std::atomic<bool> failed{ false };

    std::mutex phaseMutex;
    std::condition_variable phaseCv;
    bool readHeld = false;        // the consumer borrowed the overlap-phase read
    bool humidityWritten = false; // the producer's unrelated write completed
    std::vector<std::string> events;

    void fail()
    {
        // Flag first and wake the phase waiters without the guard: the consumer
        // holds the guard while it waits for the unrelated write, so taking it
        // first would delay the shutdown until its watchdog expired. The brief
        // guard acquisition before notifying `progress` serializes with the
        // consumer's predicate check, so that wake-up cannot be lost.
        failed = true;
        {
            std::lock_guard<std::mutex> lock(phaseMutex);
            readHeld = humidityWritten = true; // wake a waiter of either phase step; it re-checks `failed`
        }
        phaseCv.notify_all();
        {
            std::lock_guard<std::mutex> lock(guard);
        }
        progress.notify_all();
    }

    void finish()
    {
        {
            std::lock_guard<std::mutex> lock(guard);
            done = true;
        }
        progress.notify_all();
    }
};

static bool writeUnrelatedWhileReadHeld(const Example& ex, Coordination& co, int tick);
static bool consumeDeltaHoldingRead(const Example& ex, Coordination& co, ovstage_ordinal_t startOrdinal,
                                    ovstage_ordinal_t endOrdinal, std::vector<DeltaGroup>* outGroups);
static bool checkThreadedResults(Coordination& co, const std::map<std::string, float>& latest,
                                 const std::set<std::string>& deleted);

// Concurrent mode (--threads): producer and consumer run on the SAME instance
// from two threads -- every ovstage_api slot is thread-safe when called on a
// shared instance (see the Thread Safety section in ovstage_api.h). Both roles
// touch the "temperature" column, so the example coordinates the accesses that
// conflict instead of discovering them through rejected submissions: a write
// that overlaps a borrowed read group is rejected, and a payload that a newer
// write replaced cannot be read back at an older ordinal.
//  * The producer holds the guard across a tick's temperature writes (or the
//    delete) and the floor advance that seals them, then publishes the tick.
//  * The consumer waits for a tick newer than its cursor and holds the guard
//    from polling the floor through reading the delta, copying every value
//    and tombstone, and releasing every group and the read handle; it reports
//    the copies outside the guard. A lagging consumer simply reads a larger
//    interval. The normal loop needs no per-tick acknowledgment: the producer
//    may publish several ticks before a catch-up and keeps writing while the
//    consumer processes its copies; conflicting accesses wait for the guard.
//  * The overlap phase shows what does NOT conflict: while the consumer holds
//    the temperature read group of kOverlapTick, the producer writes S0's
//    pre-created humidity outside the guard, and that write completes. It is
//    an ordinary value update to a pre-existing, disjoint attribute; adding an
//    attribute or deleting a prim would be a broader structural change.
// A failure on either side wakes the other and the run exits nonzero. kWatchdog
// is a diagnostic timeout for the coordination waits, not a process-wide
// execution deadline. Batching varies run to run, which is why this mode's
// output is not part of the expected-output block.
static bool runThreaded(const Example& ex)
{
    Coordination co;
    {
        // Pre-create the overlap phase's unrelated attribute before the threads
        // start: adding a column is a structural change that conflicts with
        // every borrowed read of the prim, while updating an existing value does
        // not.
        float initial = 0.0f;
        DLTensor tensor = cpuFloatTensor(&initial, kScalarShape, kScalarStrides, 1);
        ovstage_write_data_t write = writeData(&tensor);
        ovstage_enqueue_result_t enq = ovstage_write_attribute(ex.stage, ex.sensorQueries[kHumiditySensor],
                                                               { ex.humidity, {} }, 1, write, OVSTAGE_PRIM_MODE_UPSERT);
        if (!tryWaitOp(ex.stage, enq, "write_attribute"))
            return false;
    }

    std::thread producer(
        [&ex, &co]
        {
            for (int tick = 1; tick <= kTickCount && !co.failed; ++tick)
            {
                if (tick == kOverlapTick + 1 && !writeUnrelatedWhileReadHeld(ex, co, tick))
                {
                    co.fail();
                    std::fprintf(stderr, "producer failed: stopping both roles\n");
                    co.finish();
                    return;
                }
                bool ok;
                {
                    // The tick's writes and the floor advance that seals them are
                    // one protected interval.
                    std::lock_guard<std::mutex> lock(co.guard);
                    ok = !co.failed && runProducerTick(ex, tick);
                    if (ok)
                        co.published = static_cast<ovstage_ordinal_t>(tick);
                }
                co.progress.notify_all();
                if (!ok)
                {
                    co.fail();
                    std::fprintf(stderr, "producer failed: stopping both roles\n");
                    co.finish();
                    return;
                }
                // The producer's "own rate": stall between ticks; the consumer may lag several ticks.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            co.finish();
        });

    std::map<std::string, float> latest; // sensor name -> the latest value the consumer copied
    std::set<std::string> deleted;       // sensor names the consumer saw as tombstones
    ovstage_ordinal_t lastSeen = 0;
    for (;;)
    {
        std::vector<DeltaGroup> groups;
        ovstage_ordinal_t floor = 0;
        const ovstage_ordinal_t before = lastSeen;
        bool ok = true;
        bool finished = false;
        {
            std::unique_lock<std::mutex> lock(co.guard);
            if (!co.progress.wait_for(lock, kWatchdog,
                                      [&] { return co.published > lastSeen || co.done || co.failed; }))
            {
                std::fprintf(stderr, "consumer: no producer progress for %lld s\n", (long long)kWatchdog.count());
                ok = false;
            }
            else if (co.failed || co.published <= lastSeen)
            {
                finished = true; // the producer failed (already reported), or finished and the final interval is drained
            }
            else
            {
                // Under the guard: the floor cannot move between the poll and the
                // read, and no write can touch the borrowed groups.
                ok = fetchGlobalFloor(ex.stage, &floor) &&
                     (floor == static_cast<ovstage_ordinal_t>(kOverlapTick) ?
                          consumeDeltaHoldingRead(ex, co, lastSeen + 1, floor, &groups) :
                          consumeDelta(ex, lastSeen + 1, floor, &groups));
                if (ok)
                    lastSeen = floor;
            }
        }
        if (!ok)
        {
            co.fail(); // flag first: the producer polls this between ticks
            std::fprintf(stderr, "consumer failed: stopping both roles\n");
            break;
        }
        if (finished)
            break;
        reportDelta(groups, floor, before); // outside the guard: the producer may write meanwhile
        for (const DeltaGroup& group : groups)
        {
            for (size_t i = 0; i < group.names.size(); ++i)
            {
                if (group.isDelete)
                {
                    deleted.insert(group.names[i]);
                    latest.erase(group.names[i]);
                }
                else
                {
                    latest[group.names[i]] = group.values[i];
                }
            }
        }
    }
    producer.join();
    if (co.failed)
        return false;
    return checkThreadedResults(co, latest, deleted);
}
// [/snippet:threaded-producer-consumer]

// Producer side of the overlap phase. Wait until the consumer has borrowed the
// temperature read group of kOverlapTick, then update S0's humidity at `tick`
// (kOverlapTick + 1) OUTSIDE the guard and wait for the write to complete.
// humidity is a different column that already exists on S0, so the write does
// not overlap the borrowed read: the runtime admits and completes it while that
// read is held.
static bool writeUnrelatedWhileReadHeld(const Example& ex, Coordination& co, int tick)
{
    {
        std::unique_lock<std::mutex> lock(co.phaseMutex);
        if (!co.phaseCv.wait_for(lock, kWatchdog, [&] { return co.readHeld; }))
        {
            std::fprintf(stderr, "producer: the consumer did not borrow the overlap-phase read within %lld s\n",
                         (long long)kWatchdog.count());
            return false;
        }
    }
    if (co.failed)
        return false;
    float value = 55.0f;
    DLTensor tensor = cpuFloatTensor(&value, kScalarShape, kScalarStrides, 1);
    ovstage_write_data_t write = writeData(&tensor);
    ovstage_enqueue_result_t enq =
        ovstage_write_attribute(ex.stage, ex.sensorQueries[kHumiditySensor], { ex.humidity, {} },
                                static_cast<ovstage_ordinal_t>(tick), write, OVSTAGE_PRIM_MODE_UPSERT);
    if (!tryWaitOp(ex.stage, enq, "write_attribute"))
        return false;
    {
        std::lock_guard<std::mutex> lock(co.phaseMutex);
        co.events.push_back("humidity_written");
        co.humidityWritten = true;
    }
    co.phaseCv.notify_all();
    return true;
}

// Consumer side of the overlap phase: consumeDelta, except that the fetched
// groups stay borrowed while the producer completes the unrelated humidity
// write. The borrowed temperature payload must be unchanged by it; only then
// are the groups released.
static bool consumeDeltaHoldingRead(const Example& ex, Coordination& co, ovstage_ordinal_t startOrdinal,
                                    ovstage_ordinal_t endOrdinal, std::vector<DeltaGroup>* outGroups)
{
    ovstage_ordinal_range_t delta{};
    delta.start_ordinal = startOrdinal;
    delta.end_ordinal = endOrdinal;
    delta.has_start_ordinal = true;

    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_read_attributes(ex.stage, ex.allQuery, &ex.temperature, 1, delta, &read);
    if (!tryWaitOp(ex.stage, enq, "read_attributes"))
        return false;

    std::vector<ovstage_read_group_t> held;
    std::vector<DeltaGroup> before;
    bool ok = true;
    for (;;)
    {
        size_t fetchedBatchCount = 0;
        ovstage_read_group_t group{};
        const ovstage_api_status_t fetch = ovstage_fetch_read_next(ex.stage, read, OVSTAGE_TIMEOUT_INFINITE,
            &group, 1, &fetchedBatchCount);
        if (fetch == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        ok = tryCheck(ex.stage, fetch, "fetch_read_next");
        if (!ok)
            break;
        held.push_back(group);
        DeltaGroup rows;
        ok = copyGroup(ex, group, &rows);
        if (!ok)
            break;
        before.push_back(rows);
    }
    if (ok)
    {
        {
            std::lock_guard<std::mutex> lock(co.phaseMutex);
            co.events.push_back("read_held");
            co.readHeld = true;
        }
        co.phaseCv.notify_all();
        std::unique_lock<std::mutex> lock(co.phaseMutex);
        if (!co.phaseCv.wait_for(lock, kWatchdog, [&] { return co.humidityWritten; }))
        {
            std::fprintf(stderr, "consumer: the unrelated humidity write did not complete within %lld s\n",
                         (long long)kWatchdog.count());
            ok = false;
        }
        lock.unlock();
        if (ok && co.failed)
            ok = false; // the producer failed during the phase (already reported)
    }
    for (size_t i = 0; ok && i < held.size(); ++i)
    {
        DeltaGroup after;
        ok = copyGroup(ex, held[i], &after);
        if (ok && (after.names != before[i].names || after.values != before[i].values))
        {
            std::fprintf(stderr, "consumer: borrowed temperature payload changed under an unrelated write\n");
            ok = false;
        }
    }
    for (ovstage_read_group_t& group : held) // release every borrowed group, success or not
        ok = tryCheck(ex.stage, ovstage_release_groups(ex.stage, &group, 1), "release_group") && ok;
    if (ok)
    {
        std::lock_guard<std::mutex> lock(co.phaseMutex);
        co.events.push_back("read_released");
        *outGroups = before;
    }
    enq = ovstage_release_read(ex.stage, read);
    ok = tryWaitOp(ex.stage, enq, "release_read") && ok;
    return ok;
}

// Assert what the concurrent run must have observed: the overlap phase's order
// (read held, unrelated write completed, read released) and the final state
// (S2 deleted; S0, S1 and S3 at their tick-4 and tick-6 values).
static bool checkThreadedResults(Coordination& co, const std::map<std::string, float>& latest,
                                 const std::set<std::string>& deleted)
{
    const std::vector<std::string> expectedEvents = { "read_held", "humidity_written", "read_released" };
    const std::map<std::string, float> expectedLatest = { { "S0", 40.0f }, { "S1", 61.0f }, { "S3", 63.0f } };
    const std::set<std::string> expectedDeleted = { "S2" };
    std::vector<std::string> events;
    {
        std::lock_guard<std::mutex> lock(co.phaseMutex);
        events = co.events;
    }
    bool ok = true;
    if (events != expectedEvents)
    {
        std::fprintf(stderr, "overlap phase out of order:");
        for (const std::string& event : events)
            std::fprintf(stderr, " %s", event.c_str());
        std::fprintf(stderr, "\n");
        ok = false;
    }
    if (latest != expectedLatest || deleted != expectedDeleted)
    {
        std::fprintf(stderr, "unexpected final state:");
        for (const auto& entry : latest)
            std::fprintf(stderr, " %s = %.1f", entry.first.c_str(), entry.second);
        for (const std::string& name : deleted)
            std::fprintf(stderr, " %s deleted", name.c_str());
        std::fprintf(stderr, "\n");
        ok = false;
    }
    if (ok)
        std::printf("overlap phase: humidity on S%zu written at ordinal %d while the temperature read of ordinal %d "
                    "was held; final state verified\n",
                    kHumiditySensor, kOverlapTick + 1, kOverlapTick);
    return ok;
}

int main(int argc, char** argv)
{
    const bool threaded = (argc > 1) && (std::strcmp(argv[1], "--threads") == 0);

    // ---- 1. setup: instance, attribute tokens, queries ----
    ovstage_instance_desc_t desc{};
    desc.name = "example.producer-consumer";
    Example ex;
    ovstage_api_status_t status = ovstage_create_instance(&desc, &ex.stage);
    check(nullptr, status, "create_instance");
    ex.dict = getPathDictionary(ex.stage);

    ovx_string_t attrName = literal_to_ovx_string("temperature");
    ovx_api_result_t ovxResult = path_dictionary_create_tokens_from_strings(ex.dict, &attrName, 1, &ex.temperature);
    checkOvx(ex.dict, ovxResult, "intern-token");
    ovx_string_t humidityName = literal_to_ovx_string("humidity");
    ovxResult = path_dictionary_create_tokens_from_strings(ex.dict, &humidityName, 1, &ex.humidity);
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
