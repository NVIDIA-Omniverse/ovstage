// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
// property and proprietary rights in and to this material, related
// documentation and any modifications thereto. Any use, reproduction,
// disclosure or distribution of this material and related documentation
// without an express license agreement from NVIDIA CORPORATION or
// its affiliates is strictly prohibited.

// Shared helpers for the ovstage examples. Fail-fast policy: any unexpected
// API failure prints to stderr and exits, so example code reads straight
// through. A real application would propagate errors instead (the `minimal`
// example shows the underlying checking pattern inline).
//
// If you copy one example directory out of this tree, take `common/` with it
// (the CMake fetch logic in `cmake/` is a sibling dependency the same way).

#pragma once

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_population.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <dlpack/dlpack.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Check a synchronous ovstage call; exit on failure.
inline void check(ovstage_instance_t* stage, ovstage_api_status_t status, const char* what)
{
    if (status == OVSTAGE_OK)
        return;
    std::fprintf(stderr, "ovstage %s failed (code %u): %s\n", what, status,
                 stage ? ovstage_get_error_string(stage, status) : "(no instance)");
    std::exit(EXIT_FAILURE);
}

// Check a path-dictionary call; the error string is dictionary-owned and must
// be released before exiting.
inline void checkOvx(path_dictionary_instance_t* dict, ovx_api_result_t result, const char* what)
{
    if (result.status == OVX_API_SUCCESS)
        return;
    std::fprintf(stderr, "path dictionary %s failed (code %u)\n", what, (unsigned)result.status);
    if (dict && result.error.ptr)
        path_dictionary_release_error(dict, result.error);
    std::exit(EXIT_FAILURE);
}

// Report per-op errors surfaced by a wait; exit if there were any.
inline void checkWait(ovstage_instance_t* stage, const ovstage_op_wait_result_t& wait, const char* what)
{
    for (size_t i = 0; i < wait.error_op_id_count; ++i)
    {
        const ovx_string_t e = ovstage_get_last_op_error(stage, wait.error_op_ids[i]);
        std::fprintf(stderr, "ovstage %s op %llu failed: %.*s\n", what,
                     (unsigned long long)wait.error_op_ids[i], (int)e.length, e.ptr ? e.ptr : "");
    }
    if (wait.error_op_id_count != 0)
        std::exit(EXIT_FAILURE);
}

// Drive a data-plane async enqueue to completion, then retire the op
// (ovstage_destroy_instance requires every op released first).
inline void waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq, const char* what)
{
    check(stage, enq.status, what);
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t code = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    checkWait(stage, wait, what);
    ovstage_release_op(stage, enq.op_index);
    check(stage, code, what);
}

// Population-bridge sibling of waitOp: population has its own enqueue/wait
// pair and its ops need no release.
inline void waitPop(ovstage_instance_t* stage, ovstage_population_enqueue_result_t enq, const char* what)
{
    if (enq.status != OVSTAGE_OK)
    {
        std::fprintf(stderr, "ovstage population %s enqueue rejected (code %u)\n", what, enq.status);
        std::exit(EXIT_FAILURE);
    }
    ovstage_population_op_wait_result_t wait{};
    const ovstage_api_status_t code =
        ovstage_population_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    for (size_t i = 0; i < wait.error_op_id_count; ++i)
    {
        const ovx_string_t e = ovstage_population_get_last_op_error(wait.error_op_ids[i]);
        std::fprintf(stderr, "ovstage population %s op %llu failed: %.*s\n", what,
                     (unsigned long long)wait.error_op_ids[i], (int)e.length, e.ptr ? e.ptr : "");
    }
    if (code != OVSTAGE_OK)
        std::fprintf(stderr, "ovstage population %s wait failed (code %u)\n", what, code);
    if (code != OVSTAGE_OK || wait.error_op_id_count != 0)
        std::exit(EXIT_FAILURE);
}

// The instance-owned path dictionary. NULL is a reserved valid return per
// the header ("check for NULL before dereferencing the result").
inline path_dictionary_instance_t* getPathDictionary(ovstage_instance_t* stage)
{
    path_dictionary_instance_t* dict = ovstage_get_path_dictionary(stage);
    if (!dict)
    {
        std::fprintf(stderr, "ovstage instance has no path dictionary\n");
        std::exit(EXIT_FAILURE);
    }
    return dict;
}

// Describe float32 data in CPU memory: one 1-D tensor of `shape[0]` elements,
// `lanes` lanes each. `shape`/`strides` must outlive the tensor (DLTensor
// stores pointers to them).
inline DLTensor cpuFloatTensor(float* data, const int64_t* shape, const int64_t* strides, uint8_t lanes)
{
    DLTensor tensor{};
    tensor.data = data;
    tensor.device = { kDLCPU, 0 };
    tensor.ndim = 1;
    tensor.dtype = { kDLFloat, 32, lanes };
    tensor.shape = const_cast<int64_t*>(shape);
    tensor.strides = const_cast<int64_t*>(strides);
    return tensor;
}

// Wrap one tensor as write data (see the minimal example for the full layout).
inline ovstage_write_data_t writeData(DLTensor* tensor, ovstage_attribute_semantic_t semantic = OVSTAGE_SEMANTIC_NONE,
                                      bool isArray = false)
{
    ovstage_write_data_t write{};
    write.tensors = tensor;
    write.tensor_count = 1;
    write.is_array = isArray;
    write.semantic = semantic;
    return write;
}

// Read one fixed-size float32 column as a latest read at `endOrdinal` and
// copy it into `dst`. The query must resolve to exactly one group of `count`
// elements with `lanes` lanes each.
inline void readColumn(ovstage_instance_t* stage, ovstage_query_handle_t query, ovx_token_t attribute,
                       ovstage_ordinal_t endOrdinal, float* dst, int64_t count, uint8_t lanes)
{
    ovstage_ordinal_range_t range{};
    range.end_ordinal = endOrdinal;
    range.has_start_ordinal = false;

    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read_attributes");

    ovstage_read_group_t group{};
    check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch_read_next");
    const DLTensor* tensor = group.data.tensor_count == 1 ? &group.data.tensors[0] : nullptr;
    if (!tensor || !tensor->data || tensor->ndim != 1 || tensor->shape[0] != count || tensor->dtype.lanes != lanes)
    {
        std::fprintf(stderr, "unexpected column layout (want one 1-D tensor of %lld elements, lanes=%u)\n",
                     (long long)count, lanes);
        std::exit(EXIT_FAILURE);
    }
    std::memcpy(dst, tensor->data, (size_t)count * lanes * sizeof(float));
    ovstage_release_group(stage, &group);
    waitOp(stage, ovstage_release_read(stage, read), "release_read");
}
