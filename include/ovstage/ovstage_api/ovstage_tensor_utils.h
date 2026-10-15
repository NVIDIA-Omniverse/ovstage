/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#ifndef OVSTAGE_TENSOR_UTILS_H
#define OVSTAGE_TENSOR_UTILS_H

#include "ovstage_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Explicitly convert a typed read or map tensor to the lane-based array-write layout.
 * Only call for a tensor obtained from an OVStage read or map, whose trailing axes
 * describe components. This does not infer types for arbitrary scalar arrays.
 * No payload is copied. Preserve the attribute's semantic and array kind.
 * Copy payload AND descriptor metadata before releasing the read group if the
 * write targets its pinned storage. Caller-owned layout[2] and payload must
 * remain valid until the write completes. On error, output is unchanged.
 */
static inline ovstage_api_status_t ovstage_tensor_to_write(
    const DLTensor* source, DLTensor* output, int64_t layout[2])
{
    if (!source || !output || !layout || !source->shape ||
        source->dtype.lanes != 1 || source->ndim < 1 || source->ndim > 3 ||
        source->shape[0] < 0)
        return OVSTAGE_ERROR_INVALID_ARGUMENT;

    int64_t components = 1;
    for (int32_t axis = source->ndim; axis-- > 1;)
    {
        if (source->shape[axis] <= 0 || source->shape[axis] > 255 / components ||
            (source->strides && source->strides[axis] != components))
            return OVSTAGE_ERROR_INVALID_ARGUMENT;
        components *= source->shape[axis];
    }
    const int64_t rowStride = source->strides ? source->strides[0] : components;
    if (rowStride < 0 || rowStride % components != 0)
        return OVSTAGE_ERROR_INVALID_ARGUMENT;

    DLTensor converted = *source;
    layout[0] = source->shape[0];
    layout[1] = rowStride / components;
    converted.ndim = 1;
    converted.dtype.lanes = (uint16_t)components;
    converted.shape = &layout[0];
    converted.strides = &layout[1];
    *output = converted;
    return OVSTAGE_OK;
}

#ifdef __cplusplus
}
#endif
#endif
