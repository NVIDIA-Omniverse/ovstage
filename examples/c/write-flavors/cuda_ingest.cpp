// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#include "../common/ovstage_example_utils.h"
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <cuda_runtime_api.h>
#include <cstdio>
#include <cstdlib>

static void checkCuda(cudaError_t status)
{
    if (status != cudaSuccess)
    {
        std::fprintf(stderr, "CUDA failed: %s\n", cudaGetErrorString(status));
        std::exit(EXIT_FAILURE);
    }
}

int main()
{
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0)
        return 77; // Explicit skip: this example requires a working CUDA device.
    checkCuda(cudaSetDevice(0));

    ovstage_instance_desc_t desc{};
    desc.name = "example.cuda-ingest";
    ovstage_instance_t* stage = nullptr;
    check(nullptr, ovstage_create_instance(&desc, &stage), "create_instance");
    auto* dict = ovstage_get_path_dictionary(stage);
    const ovx_string_t names[] = {literal_to_ovx_string("gpu-samples")};
    ovx_token_t attribute{};
    checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, names, 1, &attribute), "attribute token");
    const ovx_string_t paths[] = {literal_to_ovx_string("/World/Gpu/A"),
                                  literal_to_ovx_string("/World/Gpu/B"),
                                  literal_to_ovx_string("/World/Gpu/C")};
    ovx_primpath_list_t pathList{};
    checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, paths, 3, &pathList), "path list");
    ovstage_query_handle_t query{};
    check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");

    // [snippet:dlpack-cuda-ingest-c]
    float values[] = {5.0f, 6.0f, 7.0f};
    float* deviceValues = nullptr;
    checkCuda(cudaMalloc(reinterpret_cast<void**>(&deviceValues), sizeof(values)));
    checkCuda(cudaMemcpyAsync(deviceValues, values, sizeof(values), cudaMemcpyHostToDevice));
    // Finish producing the GPU data before submitting it to ovstage.
    checkCuda(cudaDeviceSynchronize());

    int64_t shape[] = {3};
    int64_t strides[] = {1};
    DLTensor tensor{};
    tensor.data = deviceValues;
    tensor.device = {kDLCUDA, 0};
    tensor.ndim = 1;
    tensor.dtype = {kDLFloat, 32, 1};
    tensor.shape = shape;
    tensor.strides = strides;
    ovstage_write_data_t write{};
    write.tensors = &tensor;
    write.tensor_count = 1;
    write.is_array = false;
    write.cuda_sync = {}; // Producer already synchronized: no additional wait.
    waitOp(stage, ovstage_write_attribute(stage, query, {attribute, {}}, 8,
                                         write, OVSTAGE_PRIM_MODE_UPSERT), "CUDA write");
    // Keep the GPU allocation alive until the write completes.
    checkCuda(cudaFree(deviceValues));
    // [/snippet:dlpack-cuda-ingest-c]

    ovstage_write_floor_desc_t watermark{};
    watermark.ordinal = 8;
    watermark.scope = OVSTAGE_SCOPE_ALL;
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "watermark");
    float result[3]{};
    readColumn(stage, query, attribute, 8, result, 3, 1);
    const bool matches = result[0] == 5.0f && result[1] == 6.0f && result[2] == 7.0f;
    waitOp(stage, ovstage_release_query(stage, query), "release query");
    checkOvx(dict, path_dictionary_release_path_list_reference(dict, pathList), "release path list");
    check(stage, ovstage_destroy_instance(stage), "destroy instance");
    return matches ? 0 : 1;
}
