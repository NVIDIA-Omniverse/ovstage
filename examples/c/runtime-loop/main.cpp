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
// Headless ovstage runtime loop: load -> populate -> read -> update -> read,
// with no renderer attached. torus-plane.usda populates the ovstage runtime
// table, then the two update paths a client has once a scene is live:
//
//   1. Write omni:xform straight into the runtime table (24 animation frames).
//   2. Edit the USD source and propagate it with apply_usd_changes.
//
// The application owns the ordinal lifecycle throughout, sealing each tick
// with ovstage_advance_write_floor before reading.
//
// Run from this directory (loads ./torus-plane.usda), or pass a scene path as
// argv[1]. Expected output: see README.md. Snippet markers are referenced by
// the skills under ../../../skills/ -- keep them intact.

// [snippet:setup]
#include <ovstage/ovstage.h>
#include <ovstage/ovstage_population.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
// [/snippet:setup]

#include "../common/ovstage_example_utils.h"

static const int kFrames = 24;

// A one-prim layer referenced onto a new /World/EditCube. The inline USDA
// needs real newlines: a single-line "def ... { ... }" is a parse error.
static const ovx_string_t kEditCubeUsda = literal_to_ovx_string(
    "#usda 1.0\n"
    "(\n"
    "    defaultPrim = \"Ref\"\n"
    ")\n"
    "\n"
    "def Cube \"Ref\"\n"
    "{\n"
    "    double size = 1.0\n"
    "}\n");

// [snippet:read-populated]
// Read the reserved usd-prim-type column over a query and return the resolved
// type names as-is, in stage order -- proof that the prims populated.
// usd-prim-type is one token per prim (is_array=false); the column crosses as
// uint64 token ids resolved back to strings through the path dictionary.
static std::vector<std::string> readPrimTypes(ovstage_instance_t* stage, path_dictionary_instance_t* dict,
                                              ovstage_query_handle_t query, ovx_token_t primType,
                                              ovstage_ordinal_t endOrdinal)
{
    ovstage_ordinal_range_t range{};
    range.end_ordinal = endOrdinal;
    range.has_start_ordinal = false;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ovstage_enqueue_result_t enq = ovstage_read_attributes(stage, query, &primType, 1, range, &read);
    waitOp(stage, enq, "read_attributes");

    std::vector<std::string> typeNames;
    for (;;)
    {
        ovstage_read_group_t group{};
        const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
        if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
            break; // the normal end of the group stream
        check(stage, fetched, "fetch_read_next");
        if (group.data.tensor_count == 1 && group.data.tensors[0].data && group.data.tensors[0].ndim > 0)
        {
            const uint64_t* ids = static_cast<const uint64_t*>(group.data.tensors[0].data);
            for (int64_t i = 0; i < group.data.tensors[0].shape[0]; ++i)
            {
                ovx_token_t token = static_cast<ovx_token_t>(ids[i]);
                ovx_string_t name{};
                ovx_api_result_t ovxResult = path_dictionary_get_strings_from_tokens(dict, &token, 1, &name);
                checkOvx(dict, ovxResult, "resolve-type");
                typeNames.push_back(std::string(name.ptr ? name.ptr : "", name.length));
            }
        }
        ovstage_release_group(stage, &group);
    }
    enq = ovstage_release_read(stage, read);
    waitOp(stage, enq, "release_read");
    return typeNames;
}
// [/snippet:read-populated]

int main(int argc, char** argv)
{
    const std::string scenePath = (argc > 1) ? argv[1] : "torus-plane.usda";

    // ---- 1. setup: instance, tokens, queries over the scene paths ----
    ovstage_instance_desc_t desc{};
    desc.name = "example.runtime-loop";
    ovstage_instance_t* stage = nullptr;
    ovstage_api_status_t status = ovstage_create_instance(&desc, &stage);
    check(nullptr, status, "create_instance");
    path_dictionary_instance_t* dict = getPathDictionary(stage);

    ovx_string_t attrNames[] = { literal_to_ovx_string("usd-prim-type"), literal_to_ovx_string("omni:xform") };
    ovx_token_t tokens[2] = { OVX_INVALID_TOKEN, OVX_INVALID_TOKEN };
    ovx_api_result_t ovxResult = path_dictionary_create_tokens_from_strings(dict, attrNames, 2, tokens);
    checkOvx(dict, ovxResult, "intern-tokens");
    const ovx_token_t primType = tokens[0];
    const ovx_token_t xform = tokens[1]; // canonical 4x4 transform column

    // Prim paths authored in the scene: /World (Xform) with two meshes.
    const ovx_string_t scene[] = { literal_to_ovx_string("/World"), literal_to_ovx_string("/World/Plane"),
                                   literal_to_ovx_string("/World/Torus") };
    const ovx_string_t torus[] = { literal_to_ovx_string("/World/Torus") };
    ovx_primpath_list_t scenePaths = OVX_INVALID_PRIMPATH_LIST;
    ovx_primpath_list_t torusPaths = OVX_INVALID_PRIMPATH_LIST;
    ovxResult = path_dictionary_create_path_list_from_strings(dict, scene, 3, &scenePaths);
    checkOvx(dict, ovxResult, "create-path-list");
    ovxResult = path_dictionary_create_path_list_from_strings(dict, torus, 1, &torusPaths);
    checkOvx(dict, ovxResult, "create-path-list");
    ovstage_query_handle_t sceneQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_query_handle_t torusQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    status = ovstage_query_from_path_list(stage, scenePaths, &sceneQuery);
    check(stage, status, "query_from_path_list");
    status = ovstage_query_from_path_list(stage, torusPaths, &torusQuery);
    check(stage, status, "query_from_path_list");

    // ---- 2. populate: USD file -> runtime table at ordinal 1, seal, verify ----
    // [snippet:populate]
    // One population op loads the USD file into the runtime table at ordinal 1;
    // sealing that ordinal makes it readable. An explicit time code keeps later
    // USD-change synchronization on the same supported timeline sample;
    // RENDERING selects meshes/lights/cameras.
    ovstage_population_enqueue_result_t popEnq =
        ovstage_population_open_usd_from_file(stage, ovx_string_t{ scenePath.c_str(), scenePath.size() },
                                              /*ordinal*/ 1, /*time*/ 0.0, OVSTAGE_POPULATION_DOMAIN_RENDERING);
    waitPop(stage, popEnq, "open_usd");

    ovstage_write_floor_desc_t floor1{};
    floor1.ordinal = 1;
    floor1.scope = OVSTAGE_SCOPE_ALL;
    ovstage_enqueue_result_t enq = ovstage_advance_write_floor(stage, &floor1);
    waitOp(stage, enq, "advance_write_floor");
    // [/snippet:populate]
    const std::vector<std::string> populatedTypes = readPrimTypes(stage, dict, sceneQuery, primType, /*endOrdinal*/ 1);

    // Example plumbing: print the type names on one line, in stage order.
    std::printf("populated prim types:");
    for (const std::string& name : populatedTypes)
        std::printf(" %s", name.c_str());
    std::printf("\n");

    // ---- 3. update path 1: animate the Torus straight in the runtime table ----
    // [snippet:update-table]
    // Write the Torus transform over 24 frames (one sealed ordinal per frame),
    // no USD round-trip. omni:xform is a 4x4 double matrix (row-vector
    // convention: translation in elements [12..14]) -> semantic = MATRIX.
    // The current implementation stores it as ONE 16-lane element per prim, so the tensor is dtype
    // {kDLFloat, 64, 16} / shape={1} -- NOT a 4x4 of lanes=1.
    double matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const int64_t matShape[] = { 1 };
    const int64_t matStrides[] = { 1 };
    DLTensor matTensor{};
    matTensor.data = matrix;
    matTensor.device = { kDLCPU, 0 };
    matTensor.ndim = 1;
    matTensor.dtype = { kDLFloat, 64, 16 };
    matTensor.shape = const_cast<int64_t*>(matShape);
    matTensor.strides = const_cast<int64_t*>(matStrides);
    ovstage_write_data_t xformWrite = writeData(&matTensor, OVSTAGE_SEMANTIC_MATRIX);

    matrix[13] = 25.0; // the scene is Y-up: keep the Torus's authored y offset
    for (int frame = 0; frame < kFrames; ++frame)
    {
        const ovstage_ordinal_t ordinal = static_cast<ovstage_ordinal_t>(2 + frame);
        matrix[12] = 100.0 * frame / (kFrames - 1); // slide along +X: tx 0 -> 100
        enq = ovstage_write_attribute(stage, torusQuery, { xform, {} }, ordinal, xformWrite, OVSTAGE_PRIM_MODE_UPSERT);
        waitOp(stage, enq, "write_attribute");
        ovstage_write_floor_desc_t frameFloor{};
        frameFloor.ordinal = ordinal;
        frameFloor.scope = OVSTAGE_SCOPE_ALL;
        enq = ovstage_advance_write_floor(stage, &frameFloor);
        waitOp(stage, enq, "advance_write_floor");
    }

    // Read back the final frame's transform (our own written column).
    ovstage_ordinal_range_t range{};
    range.end_ordinal = static_cast<ovstage_ordinal_t>(2 + kFrames - 1);
    range.has_start_ordinal = false;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    enq = ovstage_read_attributes(stage, torusQuery, &xform, 1, range, &read);
    waitOp(stage, enq, "read_attributes");
    ovstage_read_group_t group{};
    status = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
    check(stage, status, "fetch_read_next");
    // Single-path query -> exactly one 16-lane matrix element.
    if (group.data.tensor_count != 1 || !group.data.tensors[0].data || group.data.tensors[0].ndim != 1 ||
        group.data.tensors[0].shape[0] != 1 || group.data.tensors[0].dtype.lanes != 16)
    {
        std::fprintf(stderr, "unexpected omni:xform read layout\n");
        return EXIT_FAILURE;
    }
    const double* m = static_cast<const double*>(group.data.tensors[0].data);
    std::printf("final Torus xform translation (row [3][0:3]): %.1f %.1f %.1f\n", m[12], m[13], m[14]);
    ovstage_release_group(stage, &group);
    enq = ovstage_release_read(stage, read);
    waitOp(stage, enq, "release_read");
    // [/snippet:update-table]

    // ---- 4. update path 2: edit the USD source and propagate it ----
    // [snippet:update-usd]
    // Reference a cube onto a new /World/EditCube in the USD source, then
    // propagate the edit into the runtime table with apply_usd_changes at a
    // fresh ordinal above the animation's floor. add-reference carries no
    // ordinal; apply_usd_changes does.
    const ovstage_ordinal_t usdOrdinal = static_cast<ovstage_ordinal_t>(2 + kFrames);
    const ovx_string_t editLayer = kEditCubeUsda;
    // The optional out-handle is only needed for a later remove_usd_reference;
    // this reference lives for the stage's lifetime, so pass NULL (as the
    // Python sibling does by discarding the returned handle).
    popEnq = ovstage_population_add_usd_reference_from_string(stage, editLayer,
                                                              literal_to_ovx_string("/World/EditCube"), nullptr);
    waitPop(stage, popEnq, "add_usd_reference");
    popEnq = ovstage_population_apply_usd_changes(stage, usdOrdinal);
    waitPop(stage, popEnq, "apply_usd_changes");

    ovstage_write_floor_desc_t usdFloor{};
    usdFloor.ordinal = usdOrdinal;
    usdFloor.scope = OVSTAGE_SCOPE_ALL;
    enq = ovstage_advance_write_floor(stage, &usdFloor);
    waitOp(stage, enq, "advance_write_floor");

    const ovx_string_t expanded[] = { literal_to_ovx_string("/World"), literal_to_ovx_string("/World/Plane"),
                                      literal_to_ovx_string("/World/Torus"),
                                      literal_to_ovx_string("/World/EditCube") };
    ovx_primpath_list_t expandedPaths = OVX_INVALID_PRIMPATH_LIST;
    ovxResult = path_dictionary_create_path_list_from_strings(dict, expanded, 4, &expandedPaths);
    checkOvx(dict, ovxResult, "create-path-list");
    ovstage_query_handle_t expandedQuery = OVSTAGE_INVALID_QUERY_HANDLE;
    status = ovstage_query_from_path_list(stage, expandedPaths, &expandedQuery);
    check(stage, status, "query_from_path_list");
    const std::vector<std::string> editedTypes = readPrimTypes(stage, dict, expandedQuery, primType, usdOrdinal);

    // Example plumbing: print the type names on one line; the Cube is the propagated edit.
    std::printf("after USD edit, prim types:");
    for (const std::string& name : editedTypes)
        std::printf(" %s", name.c_str());
    std::printf("\n");
    // [/snippet:update-usd]

    // Release every handle, then destroy: ovstage_destroy_instance requires
    // all ops and handles released first.
    enq = ovstage_release_query(stage, sceneQuery);
    waitOp(stage, enq, "release_query");
    enq = ovstage_release_query(stage, torusQuery);
    waitOp(stage, enq, "release_query");
    enq = ovstage_release_query(stage, expandedQuery);
    waitOp(stage, enq, "release_query");
    path_dictionary_release_path_list_reference(dict, scenePaths);
    path_dictionary_release_path_list_reference(dict, torusPaths);
    path_dictionary_release_path_list_reference(dict, expandedPaths);
    ovstage_destroy_instance(stage);
    return 0;
}
