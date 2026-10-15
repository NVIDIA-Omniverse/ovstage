// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
// property and proprietary rights in and to this material, related
// documentation and any modifications thereto. Any use, reproduction,
// disclosure or distribution of this material and related documentation
// without an express license agreement from NVIDIA CORPORATION or
// its affiliates is strictly prohibited.

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_population.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../common/ovstage_example_utils.h"

// Populate a hierarchy, change inheritance, and read the computed matrix.
int main(int argc, char** argv)
{
    const std::string file = argc > 1 ? argv[1] : "car.usda";
    ovstage_instance_desc_t desc{};
    desc.name = "example.transforms";
    ovstage_instance_t* stage = nullptr;
    check(nullptr, ovstage_create_instance(&desc, &stage), "create instance");
    auto* dict = getPathDictionary(stage);
    // Include every Xform so population seeds the hierarchy and output rows.
    const ovx_string_t type = literal_to_ovx_string("Xform");
    ovstage_population_selector_t selector{};
    selector.prim_predicate = ovstage_population_prim_predicate_has_type(&type, 1);
    ovstage_population_desc_t populationDesc{};
    populationDesc.selectors = &selector;
    populationDesc.selector_count = 1;
    waitPop(stage, ovstage_population_open_usd_from_file_with_desc(stage,
        { file.c_str(), file.size() }, 1, 0.0, &populationDesc, 1), "populate");
    ovstage_write_floor_desc_t initial{};
    initial.ordinal = 1;
    initial.scope = OVSTAGE_SCOPE_ALL;
    waitOp(stage, ovstage_advance_write_floor(stage, &initial), "advance watermark");
    const ovx_string_t path = literal_to_ovx_string("/World/Car/Wheel");
    ovx_primpath_list_t list = OVX_INVALID_PRIMPATH_LIST;
    checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, &path, 1, &list), "create paths");
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    check(stage, ovstage_query_from_path_list(stage, list, &query), "create query");
    // [snippet:transform-layout-c]
    // One row-major matrix for the wheel; translation is in elements 12..14.
    double matrix[] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 0, 0, 1 };
    int64_t shape[] = { 1 };
    DLTensor matrixTensor{ matrix, { kDLCPU, 0 }, 1, { kDLFloat, 64, 16 }, shape, nullptr, 0 };
    bool reset = false;
    DLTensor resetTensor{ &reset, { kDLCPU, 0 }, 1, { kDLBool, 8, 1 }, shape, nullptr, 0 };
    // [/snippet:transform-layout-c]

    // [snippet:transform-write-c]
    // Both attributes describe the same update, at ordinal 2.
    ovstage_write_data_t matrixWrite{};
    matrixWrite.tensors = &matrixTensor;
    matrixWrite.tensor_count = 1;
    matrixWrite.is_array = false;
    matrixWrite.semantic = OVSTAGE_SEMANTIC_MATRIX;
    waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("omni:xform") },
        2, matrixWrite, OVSTAGE_PRIM_MODE_UPSERT), "write transform");
    ovstage_write_data_t resetWrite{};
    resetWrite.tensors = &resetTensor;
    resetWrite.tensor_count = 1;
    resetWrite.is_array = false;
    waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("omni:resetXformStack") },
        2, resetWrite, OVSTAGE_PRIM_MODE_UPSERT), "write reset");
    // false keeps the wheel relative to the car; true would make it world-relative.
    // [/snippet:transform-write-c]

    // [snippet:transform-compute-c]
    // CPU incremental computation supports this USD-populated hierarchy.
    const ovstage_hierarchy_computation_model_desc_t* models = nullptr;
    size_t count = 0;
    check(stage, ovstage_get_hierarchy_computation_models(stage, &models, &count), "get models");
    const auto model = OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL;
    bool supported = false;
    for (size_t i = 0; i < count; ++i)
        supported = supported || models[i].model_id == model;
    if (!supported)
    {
        std::fprintf(stderr, "CPU incremental hierarchy computation is unavailable\n");
        std::exit(EXIT_FAILURE);
    }
    ovstage_write_floor_desc_t watermark{};
    watermark.ordinal = 2;
    watermark.scope = OVSTAGE_SCOPE_ALL;
    waitOp(stage, ovstage_compute_hierarchy(stage, model, 2, 2), "compute hierarchy");
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
    // Car x=10 plus wheel local x=4 gives wheel world x=14.
    // [/snippet:transform-compute-c]

    // [snippet:transform-override-c]
    // Override the runtime reset without editing the source USD's xformOpOrder.
    reset = true; // The earlier write has completed, so its input can be changed.
    waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("omni:resetXformStack") },
        3, resetWrite, OVSTAGE_PRIM_MODE_UPSERT), "override reset");
    watermark.ordinal = 3;
    waitOp(stage, ovstage_compute_hierarchy(stage, model, 3, 3), "compute hierarchy");
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
    // The unchanged matrix now puts the wheel at world x=4, ignoring the car.
    // [/snippet:transform-override-c]

    // [snippet:transform-read-c]
    // Read the derived output only after computation; never write it directly.
    const ovx_string_t name = literal_to_ovx_string("omni:fabric:worldMatrix");
    ovx_token_t world = OVX_INVALID_TOKEN;
    checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &world), "intern world matrix");
    ovstage_ordinal_range_t range{};
    range.end_ordinal = 3;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    waitOp(stage, ovstage_read_attributes(stage, query, &world, 1, range, &read), "read world matrix");
    bool found = false;
    for (;;)
    {
        ovstage_read_group_t group{};
        const auto status = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
        if (status == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        check(stage, status, "fetch group");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const double*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        for (uint32_t i = 0; i < group.prims.count; ++i)
        {
            const uint32_t row = group.data.index_map ? group.data.index_map[i] : i;
            std::printf("Wheel world x: %.1f\n", values[row * 16 + 12]);
            // Wheel world x: 4.0
            found = true;
        }
        check(stage, ovstage_release_group(stage, &group), "release group");
    }
    waitOp(stage, ovstage_release_read(stage, read), "release read");
    if (!found)
    {
        std::printf("No derived world-matrix row is available.\n");
        // No derived world-matrix row is available.
    }
    // [/snippet:transform-read-c]

    waitOp(stage, ovstage_release_query(stage, query), "release query");
    path_dictionary_release_path_list_reference(dict, list);
    ovstage_destroy_instance(stage);
    return 0;
}
