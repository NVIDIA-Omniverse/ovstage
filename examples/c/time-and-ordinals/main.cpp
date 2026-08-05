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
// One timeline, two data sources, one mapping. A saxpy simulator steps three
// sphere prims with NON-UNIFORM dt (position += velocity * dt), and a
// time-sampled USD clip animates a conveyor. ovstage stores no time -- it
// only orders writes by opaque uint64 ordinals -- so the application owns an
// explicit ordinal <-> time table and uses it to land BOTH sources in the
// same ordinal slot per tick: the sim write and the clip sample for the same
// simulation time share one ordinal. Because the slots are shared, anything
// a consumer reads at a sealed ordinal describes one instant of time. A
// final tick rewinds the clip: USD sampling time is playback policy, free to
// diverge from the timeline. Verifying read-back values is the test suite's
// job, not this example's.
//
// Expected output: see README.md. Snippet markers are referenced by the
// skills under ../../../skills/ -- keep them intact.

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_population.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/ovstage_example_utils.h"

// A short time-sampled clip, inline: xformOp:translate.x animates linearly
// 0 -> 120 across timecodes 0..12 with timeCodesPerSecond = 16, so
// translate.x = 160 * t seconds. Prim bodies must be multi-line -- a
// single-line "def X { ... }" is a parse error.
static const char* kClipUsda =
    "#usda 1.0\n"
    "(\n"
    "    defaultPrim = \"World\"\n"
    "    metersPerUnit = 1.0\n"
    "    upAxis = \"Y\"\n"
    "    timeCodesPerSecond = 16\n"
    "    startTimeCode = 0\n"
    "    endTimeCode = 12\n"
    ")\n"
    "\n"
    "def Xform \"World\"\n"
    "{\n"
    "    def Cube \"Conveyor\"\n"
    "    {\n"
    "        double size = 1.0\n"
    "\n"
    "        double3 xformOp:translate.timeSamples = {\n"
    "            0: (0, 1, 0),\n"
    "            12: (120, 1, 0),\n"
    "        }\n"
    "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
    "    }\n"
    "}\n";

// Non-uniform step sizes (all exact binary fractions, so every printed time
// and position is exact). Ordinals are ordinal = tick + 1; because dt varies,
// time is NOT a formula of the ordinal -- the app must keep the table.
static const int kTicks = 6;
static const int kSphereCount = 3;
static const double kDtOfTick[kTicks] = { 0.125, 0.125, 0.0625, 0.0625, 0.25, 0.125 };
static const double kTimeCodesPerSecond = 16.0; // authored in the clip's layer metadata
static const double kRewindUsdTime = 0.125;     // section 5 points the clip clock back here
static const char* kSpherePaths[kSphereCount] = { "/World/Sphere_0", "/World/Sphere_1", "/World/Sphere_2" };
static const double kStartX[kSphereCount] = { 0.0, 10.0, 20.0 };
static const double kVelocityX[kSphereCount] = { 8.0, 16.0, 24.0 };
static const int64_t kFloat3Shape[] = { kSphereCount };
static const int64_t kFloat3Strides[] = { 1 };

int main()
{
    // ---- 1. setup: instance, tokens, the time table, clip + t=0 state at ordinal 1 ----
    ovstage_instance_desc_t desc{};
    desc.name = "example.time-and-ordinals";
    ovstage_instance_t* stage = nullptr;
    ovstage_api_status_t status = ovstage_create_instance(&desc, &stage);
    check(nullptr, status, "create_instance");
    path_dictionary_instance_t* dict = getPathDictionary(stage);

    ovx_string_t attrNames[] = { literal_to_ovx_string("sim:position"), literal_to_ovx_string("sim:velocity") };
    ovx_token_t tokens[2] = { OVX_INVALID_TOKEN, OVX_INVALID_TOKEN };
    ovx_api_result_t ovxResult = path_dictionary_create_tokens_from_strings(dict, attrNames, 2, tokens);
    checkOvx(dict, ovxResult, "intern-tokens");
    const ovx_token_t position = tokens[0];
    const ovx_token_t velocity = tokens[1];

    ovx_string_t paths[kSphereCount];
    for (int i = 0; i < kSphereCount; ++i)
        paths[i] = ovx_string_t{ kSpherePaths[i], std::strlen(kSpherePaths[i]) };
    ovx_primpath_list_t spherePaths = OVX_INVALID_PRIMPATH_LIST;
    ovxResult = path_dictionary_create_path_list_from_strings(dict, paths, kSphereCount, &spherePaths);
    checkOvx(dict, ovxResult, "create-path-list");
    ovstage_query_handle_t sphereQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    status = ovstage_query_from_path_list(stage, spherePaths, &sphereQuery);
    check(stage, status, "query_from_path_list");

    // [snippet:time-to-ordinal-table]
    // ovstage stores no time; its ordering key is an opaque uint64 ordinal.
    // The app owns the mapping, and because dt varies it is a TABLE, not a
    // formula: timeOfOrdinal[ordinal] is the simulation time whose state that
    // ordinal holds. Ordinal 1 holds the t = 0 state; tick N lands at N + 1.
    double timeOfOrdinal[kTicks + 3] = {};
    timeOfOrdinal[1] = 0.0;
    for (int tick = 1; tick <= kTicks; ++tick)
        timeOfOrdinal[tick + 1] = timeOfOrdinal[tick] + kDtOfTick[tick - 1];
    // [/snippet:time-to-ordinal-table]
    std::printf("time model: non-uniform dt; the app owns an ordinal <-> time table (ordinal = tick + 1)\n");
    std::printf("clip: conveyor translate.x animates 0 -> 120 over 12 timecodes @ 16 codes/s (translate.x = 160*t)\n");

    // Populate the clip at ordinal 1, evaluated at USD time 0.0 s, and write
    // the spheres' t=0 state at the same ordinal: both sources share the one
    // ordinal axis from the start. Sealing ordinal 1 makes it readable.
    const ovx_string_t clipUsda{ kClipUsda, std::strlen(kClipUsda) };
    ovstage_population_enqueue_result_t popEnq = ovstage_population_open_usd_from_string(
        stage, clipUsda, /*ordinal*/ 1, /*time*/ 0.0, OVSTAGE_POPULATION_DOMAIN_ALL);
    waitPop(stage, popEnq, "open_usd_from_string");

    // [snippet:float3-attribute-write]
    // This example uses the canonical float3 transport form: one tensor with
    // dtype lanes=3 / shape={prim count}. A [prim count][3] lanes=1 copy-in is
    // also accepted but normalizes to this raw form. The semantic stamps POINT
    // for positions and VECTOR for velocities. UPSERT creates the prims.
    float startPos[kSphereCount * 3] = {};
    float startVel[kSphereCount * 3] = {};
    for (int i = 0; i < kSphereCount; ++i)
    {
        startPos[i * 3] = static_cast<float>(kStartX[i]);
        startVel[i * 3] = static_cast<float>(kVelocityX[i]);
    }
    DLTensor posTensor = cpuFloatTensor(startPos, kFloat3Shape, kFloat3Strides, 3);
    DLTensor velTensor = cpuFloatTensor(startVel, kFloat3Shape, kFloat3Strides, 3);
    ovstage_write_data_t posWrite = writeData(&posTensor, OVSTAGE_SEMANTIC_POINT);
    ovstage_write_data_t velWrite = writeData(&velTensor, OVSTAGE_SEMANTIC_VECTOR);
    ovstage_enqueue_result_t enq = ovstage_write_attribute(stage, sphereQuery, { position, {} }, /*ordinal*/ 1,
                                                           posWrite, OVSTAGE_PRIM_MODE_UPSERT);
    waitOp(stage, enq, "write_attribute");
    enq = ovstage_write_attribute(stage, sphereQuery, { velocity, {} }, /*ordinal*/ 1, velWrite,
                                  OVSTAGE_PRIM_MODE_UPSERT);
    waitOp(stage, enq, "write_attribute");
    // [/snippet:float3-attribute-write]

    ovstage_write_floor_desc_t floor1{};
    floor1.ordinal = 1;
    floor1.scope = OVSTAGE_SCOPE_ALL;
    enq = ovstage_advance_write_floor(stage, &floor1);
    waitOp(stage, enq, "advance_write_floor");
    std::printf("setup: clip populated + %d spheres written at t = 0.0000 s (ordinal 1); write floor -> 1\n",
                kSphereCount);

    // ---- 2. tick loop: land both sources at the tick's ordinal and seal it ----
    // Per tick the app steps the saxpy sim (position += velocity * dt),
    // writes it at the tick's ordinal, and re-samples the clip at the same
    // simulation time (population and direct writes share the one ordinal
    // axis). Advancing the write floor seals the tick for consumers.
    double simX[kSphereCount] = { kStartX[0], kStartX[1], kStartX[2] };
    float pos[kSphereCount * 3] = {};
    for (int tick = 1; tick <= kTicks; ++tick)
    {
        const double dt = kDtOfTick[tick - 1];
        const double t = timeOfOrdinal[tick + 1];                                   // this tick's simulation time
        const ovstage_ordinal_t ordinal = static_cast<ovstage_ordinal_t>(tick + 1); // ...lands at this ordinal
        for (int i = 0; i < kSphereCount; ++i)
        {
            simX[i] += kVelocityX[i] * dt; // saxpy: x += v * dt
            pos[i * 3] = static_cast<float>(simX[i]);
        }
        DLTensor posT = cpuFloatTensor(pos, kFloat3Shape, kFloat3Strides, 3);
        ovstage_write_data_t write = writeData(&posT, OVSTAGE_SEMANTIC_POINT);
        enq = ovstage_write_attribute(stage, sphereQuery, { position, {} }, ordinal, write, OVSTAGE_PRIM_MODE_UPSERT);
        waitOp(stage, enq, "write_attribute");
        popEnq = ovstage_population_apply_usd_time(stage, ordinal, t);
        waitPop(stage, popEnq, "apply_usd_time");

        ovstage_write_floor_desc_t tickFloor{};
        tickFloor.ordinal = ordinal;
        tickFloor.scope = OVSTAGE_SCOPE_ALL;
        enq = ovstage_advance_write_floor(stage, &tickFloor);
        waitOp(stage, enq, "advance_write_floor");
        std::printf("tick %d (dt = %.4f s) -> t = %.4f s = timecode %2.0f -> ordinal %llu:"
                    " sim + clip written, sealed\n",
                    tick, dt, t, t * kTimeCodesPerSecond, (unsigned long long)ordinal);
    }

    // ---- 3. USD time is playback policy: rewind the clip ----
    // The clip's clock is not the timeline's. apply_usd_time can point it
    // anywhere -- rewind, loop, hold -- while simulation time and ordinals
    // only ever climb. One more tick: sim time advances to 0.875 s (ordinal
    // 8) but the clip is pointed BACK to usd t = 0.125 s.
    timeOfOrdinal[kTicks + 2] = timeOfOrdinal[kTicks + 1] + 0.125;
    const ovstage_ordinal_t rewindOrdinal = static_cast<ovstage_ordinal_t>(kTicks + 2);
    popEnq = ovstage_population_apply_usd_time(stage, rewindOrdinal, kRewindUsdTime);
    waitPop(stage, popEnq, "apply_usd_time");
    ovstage_write_floor_desc_t floorRewind{};
    floorRewind.ordinal = rewindOrdinal;
    floorRewind.scope = OVSTAGE_SCOPE_ALL;
    enq = ovstage_advance_write_floor(stage, &floorRewind);
    waitOp(stage, enq, "advance_write_floor");

    std::printf("rewind: sim t = %.4f s (ordinal %llu) but the clip is pointed at usd t = %.4f s"
                " -- playback policy, not the timeline\n",
                timeOfOrdinal[rewindOrdinal], (unsigned long long)rewindOrdinal, kRewindUsdTime);

    // Release every handle, then destroy: ovstage_destroy_instance requires
    // all ops and handles released first.
    enq = ovstage_release_query(stage, sphereQuery);
    waitOp(stage, enq, "release_query");
    path_dictionary_release_path_list_reference(dict, spherePaths);
    ovstage_destroy_instance(stage);
    return 0;
}
