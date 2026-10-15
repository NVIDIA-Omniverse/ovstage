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
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <cuda_runtime_api.h>
#include <cstdio>
#include <cstdlib>
#include "../common/ovstage_example_utils.h"

// [snippet:gpu-transform-write-c]
// Write caller-owned CUDA buffers after the producer event, with no CPU readback.
void writeGpuTransforms(ovstage_instance_t* stage, ovstage_query_handle_t query,
                       double* deviceMatrices, bool* deviceResetFlags,
                       int64_t primCount, int deviceId, ovstage_ordinal_t ordinal,
                       cudaEvent_t readyEvent)
{
    // Choose an available GPU model before changing the stage.
    const ovstage_hierarchy_computation_model_desc_t* models = nullptr;
    size_t modelCount = 0;
    check(stage, ovstage_get_hierarchy_computation_models(stage, &models, &modelCount), "get models");
    const auto model = OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_GLOBAL;
    bool supported = false;
    for (size_t i = 0; i < modelCount; ++i)
        supported = supported || models[i].model_id == model;
    if (!supported)
    {
        std::fprintf(stderr, "GPU global hierarchy computation is unavailable\n");
        std::exit(EXIT_FAILURE);
    }

    // Dense query order: one row-major 4x4 matrix and one bool per prim.
    int64_t shape[] = { primCount };
    DLTensor matrices{ deviceMatrices, { kDLCUDA, deviceId }, 1,
        { kDLFloat, 64, 16 }, shape, nullptr, 0 };
    DLTensor resetFlags{ deviceResetFlags, { kDLCUDA, deviceId }, 1,
        { kDLBool, 8, 1 }, shape, nullptr, 0 };

    // Record readyEvent on the producing stream after filling both buffers.
    ovstage_write_data_t write{};
    write.tensors = &matrices;
    write.tensor_count = 1;
    write.is_array = false;
    write.semantic = OVSTAGE_SEMANTIC_MATRIX;
    write.cuda_sync.wait_event = reinterpret_cast<uintptr_t>(readyEvent);
    waitOp(stage, ovstage_write_attribute(stage, query,
        { 0, literal_to_ovx_string("omni:xform") }, ordinal, write,
        OVSTAGE_PRIM_MODE_UPSERT), "write GPU transforms");

    write.tensors = &resetFlags;
    write.semantic = OVSTAGE_SEMANTIC_NONE;
    waitOp(stage, ovstage_write_attribute(stage, query,
        { 0, literal_to_ovx_string("omni:resetXformStack") }, ordinal, write,
        OVSTAGE_PRIM_MODE_UPSERT), "write GPU reset flags");
    // Both writes are complete: the caller can now reuse the buffers and event.
    ovstage_write_floor_desc_t watermark{};
    watermark.ordinal = ordinal;
    watermark.scope = OVSTAGE_SCOPE_ALL;
    // Wait for GPU hierarchy computation before a consumer uses the result.
    waitOp(stage, ovstage_compute_hierarchy(stage, model, ordinal, ordinal), "compute GPU hierarchy");
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
}
// [/snippet:gpu-transform-write-c]
