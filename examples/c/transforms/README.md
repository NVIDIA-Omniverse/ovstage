# Transforms

Populate `car.usda` by selecting every Xform prim, update the wheel transform,
then override its reset state and read its world matrix. Requires a native
build with USD population and CPU incremental hierarchy computation.

Configure with CMake, build the `transforms` target, and run it with the path to `car.usda`.

Expected output:

```text
Wheel world x: 4.0
```

The snippets run in order. `query` selects `/World/Car/Wheel`; its parent
has a translation of 10 along X. The USD source keeps transform inheritance enabled.

`waitOp()` waits, checks errors, and releases operation tracking.
`check()` and `checkOvx()` check stage and dictionary status codes.

## Layout

```cpp
// One row-major matrix for the wheel; translation is in elements 12..14.
double matrix[] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 0, 0, 1 };
int64_t shape[] = { 1 };
DLTensor matrixTensor{ matrix, { kDLCPU, 0 }, 1, { kDLFloat, 64, 16 }, shape, nullptr, 0 };
bool reset = false;
DLTensor resetTensor{ &reset, { kDLCPU, 0 }, 1, { kDLBool, 8, 1 }, shape, nullptr, 0 };
```

## Write

```cpp
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
```

## Compute

```cpp
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
```

## Override

```cpp
// Override the runtime reset without editing the source USD's xformOpOrder.
reset = true; // The earlier write has completed, so its input can be changed.
waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("omni:resetXformStack") },
    3, resetWrite, OVSTAGE_PRIM_MODE_UPSERT), "override reset");
watermark.ordinal = 3;
waitOp(stage, ovstage_compute_hierarchy(stage, model, 3, 3), "compute hierarchy");
waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
// The unchanged matrix now puts the wheel at world x=4, ignoring the car.
```

## Read

```cpp
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
```

## GPU transform writes

`gpu_write.cpp` provides a helper for existing CUDA buffers. Supply one
float64 matrix and one boolean reset flag per queried prim, and a CUDA event
recorded after producing both buffers. Keep the inputs alive until the helper
returns. The helper selects `GPU_GLOBAL` and computes the hierarchy after writing.
Use a populated hierarchy with existing derived-output rows. It does not read
payloads back to the CPU.

```cpp
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
```

The optional `transforms-gpu-write` object target compiles `gpu_write.cpp` as a
reference helper. To use it in an application, copy the helper into your source
file, or link the object target and declare `writeGpuTransforms` with the
signature shown above. The example executable does not call this helper.
