# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

# [snippet:gpu-transform-write]
from ovstage import AttributeSemantic, DLDataType, DLDataTypeCode, HierarchyComputationModel, make_dltensor


def write_gpu_transforms(stage, query, matrices, reset_flags, prim_count, ordinal, ready_event):
    """Write CUDA-resident matrices and reset flags after the producer event."""
    # Choose an available GPU model before changing the stage.
    model = HierarchyComputationModel.GPU_GLOBAL
    if model not in [item.model_id for item in stage.get_hierarchy_computation_models()]:
        raise RuntimeError("GPU global hierarchy computation is unavailable")

    # matrices is a contiguous CUDA DLPack producer with float64 values and
    # shape (prim_count, 4, 4) or (prim_count, 16). No buffer copy is needed.
    matrix_tensor = make_dltensor(matrices,
        dtype=DLDataType(code=DLDataTypeCode.kDLFloat, bits=64, lanes=16),
        shape=[prim_count])
    # reset_flags is a CUDA boolean array: True means explicit world transforms.
    reset_tensor = make_dltensor(reset_flags,
        dtype=DLDataType(code=DLDataTypeCode.kDLBool, bits=8, lanes=1),
        shape=[prim_count])

    # ready_event is the integer handle of a CUDA event recorded after both
    # buffers were produced. Waits synchronize work; they do not read values back.
    stage.write_attribute(query, "omni:xform", ordinal=ordinal,
        tensors=matrix_tensor, is_array=False, semantic=AttributeSemantic.MATRIX,
        cuda_event=ready_event).wait()
    stage.write_attribute(query, "omni:resetXformStack", ordinal=ordinal,
        tensors=reset_tensor, is_array=False, cuda_event=ready_event).wait()
    # Keep both buffers and the event alive and unchanged until these writes finish.
    # Compute from the completed update on the GPU; no payload readback.
    stage.compute_hierarchy(input_ordinal=ordinal, output_ordinal=ordinal, model=model)
    stage.advance_write_floor(ordinal=ordinal).wait()
# [/snippet:gpu-transform-write]
