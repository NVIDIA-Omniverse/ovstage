# ovstage write workflows (C/C++)

A tour of the higher-level ovstage write workflows via the core C API: one
standalone program, three numbered sections. It is the C sibling of
`../../python/write-flavors/main.py` (whose section 4, GPU ingest, is Python-only).

> The fine-grained write **contracts** — column shapes, semantics, UPSERT/INSERT
> admission, sparse `index_map`/`mask`, delete tombstones, and CPU map/unmap —
> are **asserted by the public tests** under `../../../tests/` (see that tree's
> `AGENTS.md`). This example stays a workflow tour; the tests are the contract.
> In particular, `../../../tests/c/test_minimal.cpp` verifies that a fixed-size
> matrix with MATRIX semantic written as `[N, 4, 4]` with `lanes = 1`
> reads and maps back as `[N, 4, 4]` with scalar lanes.

## At a glance

Each numbered section is independent — write, seal, read back, print:

1. Batched writes — two columns in one operation.
2. Clone — stamp one subtree onto two targets, then re-query.
3. Pipelined submission — enqueue several ordinals ahead, drain with zero-timeout polls.

## What you'll see

```text
== 1. batched writes ==
heat: 7.0 8.0
tint: (0.1 0.2 0.3) (0.4 0.5 0.6)
== 2. clone ==
mass /World/Env0/Rig = 5.0
mass /World/Env1/Rig = 5.0
Duplicate targets rejected before cloning.
Async clone copied 1 prim to 1 target
Clone source was not found.
== 3. pipelined submission ==
4 writes enqueued (ordinals 4..7) with zero waits; the CPU stays busy meanwhile
all 4 drained by zero-timeout polls and released; floor -> 7
latest sample after the pipeline: 400 401 402
Copied heat values: 200.0 300.0 400.0
Heat values changed at 14: 300.0 400.0
```

- Each section writes at its own ordinals, then advances the write floor (seals
  everything at or below that ordinal) before reading back. The floor is global
  and monotonic, so the sections' ordinals increase across the run (1, 2–4, 4–7, then 9–15).
- Section 3 shows the pipelined programming model, not a speedup — current
  releases may execute enqueued operations serially. Client-managed tensors must
  stay valid until their op completes, hence one buffer/tensor per in-flight write.

## Build and run

The example builds standalone with CMake: `find_package(ovstage)` locates an
installed package, otherwise the build fetches the released package zip (see
`../cmake/ovstage.cmake`). The shared check/wait helpers live in
`../common/ovstage_example_utils.h` — like `../cmake/`, copy that directory along
if you relocate this example.

```bash
# Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/write-flavors
```

```powershell
# Windows
cmake -B build
cmake --build build --config Release
# ovstage discovers its bundled plugins relative to where ovstage.dll loads
# from, so keep the package bin/ intact and put it on PATH (do not copy the
# DLL next to the exe):
$env:PATH = "<ovstage-package>\bin;$env:PATH"
.\build\Release\write-flavors.exe
```

On Linux the build sets an rpath onto the package `bin/`, so the binary runs from
anywhere with no environment setup (no assets needed). To build every C example
at once, configure from the parent directory (`../CMakeLists.txt` aggregates them).


## Snippets

The `[snippet:name]` markers in `main.cpp` fence regions referenced by the ovstage
skills under `../../../skills/`; keep them intact when editing.

- `batched-write-attributes` — two columns in one `ovstage_write_attributes` op
- `clone-and-requery` — clone to N targets, then a fresh path-list query for readback
- `pipelined-submission` — enqueue several ordinals ahead without blocking
- `poll-wait-release` — `wait_op(timeout=0)` poll loop, TIMEOUT handling, `release_op`

## Notes

- This C example is CPU-only. The C API also accepts GPU-resident write payloads:
  `ovstage_write_data_t::cuda_sync` carries the producer-side `{stream, wait_event}`
  pair (see the header docs). The optional `cuda-ingest` target below and the
  Python sibling demonstrate CUDA input.
- Reads may split one query across several groups, each with its own prim/data
  index maps; `readLatestRows` in `main.cpp` is the robust way to consume them.
- A batched `ovstage_write_attributes` is one operation (one op id, one structural
  precreate), not an atomic transaction — entries may apply incrementally.
- The examples fail fast: any unexpected API failure prints and exits (helpers in
  `../common/ovstage_example_utils.h`). A real application would propagate errors instead.


## Optional CUDA tensor write

With the CUDA Toolkit installed, configure with `-DOVSTAGE_EXAMPLE_CUDA=ON` and
build the `cuda-ingest` target. Run `cuda-ingest` to write three CUDA values and
check CPU read-back. Exit 77 means no CUDA device is available; exit 0 means the
values matched. The main `write-flavors` target remains CPU-only.

The CUDA source synchronizes production before writing and retains its allocation
until the operation completes:

```cpp
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
```

## Writing individual attributes

These snippets run in order on a query containing `/World/Writes/A`,
`/World/Writes/B`, and `/World/Writes/C`.
`waitOp()` waits, checks errors, and releases operation tracking.

### Parameters

```cpp
float values[] = { 10, 20, 30 };
int64_t shape[] = { 3 };
DLTensor tensor{};
tensor.data = values;
tensor.device = { kDLCPU, 0 };
tensor.ndim = 1;
tensor.dtype = { kDLFloat, 32, 1 };
tensor.shape = shape;
// Keep the source buffer and descriptors alive until completion.
ovstage_write_data_t write{};
write.tensors = &tensor;
write.tensor_count = 1;
write.is_array = false;

waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("heat") },
    9, write, OVSTAGE_PRIM_MODE_UPSERT), "write heat");
```

### Fixed shape

```cpp
// Three rows, each containing one three-component vector.
float values[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
int64_t shape[] = { 3, 3 };
DLTensor tensor{};
tensor.data = values;
tensor.device = { kDLCPU, 0 };
tensor.ndim = 2;
tensor.dtype = { kDLFloat, 32, 1 };
tensor.shape = shape;
// Keep the source buffer and descriptors alive until completion.
ovstage_write_data_t write{};
write.tensors = &tensor;
write.tensor_count = 1;
write.is_array = false;

waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("velocity") },
    10, write, OVSTAGE_PRIM_MODE_UPSERT), "write velocity");
```

### Array width

```cpp
// Two float3 elements for A, one for B, and three for C.
float a[] = { 0, 0, 0, 1, 0, 0 };
float b[] = { 0, 1, 0 };
float c[] = { 0, 0, 1, 1, 0, 1, 2, 0, 1 };
int64_t lengths[] = { 2, 1, 3 };
DLTensor tensors[] = {
    { a, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, &lengths[0], nullptr, 0 },
    { b, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, &lengths[1], nullptr, 0 },
    { c, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, &lengths[2], nullptr, 0 },
};
ovstage_write_data_t write{};
write.tensors = tensors;
write.tensor_count = 3;
write.is_array = true; // Each tensor describes one prim's array.
waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("points") },
    11, write, OVSTAGE_PRIM_MODE_UPSERT), "write points");
```

### Index map

```cpp
// A and B use row 0; C uses row 1.
float values[] = { 100, 200 };
int64_t shape[] = { 2 };
DLTensor tensor{};
tensor.data = values;
tensor.device = { kDLCPU, 0 };
tensor.ndim = 1;
tensor.dtype = { kDLFloat, 32, 1 };
tensor.shape = shape;
// Keep the source buffer and descriptors alive until completion.
ovstage_write_data_t write{};
write.tensors = &tensor;
write.tensor_count = 1;
write.is_array = false;
const uint32_t indexMap[] = { 0, 0, 1 };
write.index_map = indexMap;
write.count = 3;
waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("heat") },
    12, write, OVSTAGE_PRIM_MODE_UPSERT), "write heat");
// A = 100, B = 100, C = 200.
```

### Mask

```cpp
// Only B receives a value, but the source still has a row for each prim.
float values[] = { 0, 250, 0 };
int64_t shape[] = { 3 };
DLTensor tensor{};
tensor.data = values;
tensor.device = { kDLCPU, 0 };
tensor.ndim = 1;
tensor.dtype = { kDLFloat, 32, 1 };
tensor.shape = shape;
// Keep the source buffer and descriptors alive until completion.
ovstage_write_data_t write{};
write.tensors = &tensor;
write.tensor_count = 1;
write.is_array = false;
const uint64_t mask[] = { 0b010 };
write.mask = mask;
write.count = 3;
waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("heat") },
    13, write, OVSTAGE_PRIM_MODE_UPSERT), "write heat");
// A = 100, B = 250, C = 200.
```

### Count

```cpp
// Address only A and B, the first two prims in query order.
float values[] = { 300, 400 };
int64_t shape[] = { 2 };
DLTensor tensor{};
tensor.data = values;
tensor.device = { kDLCPU, 0 };
tensor.ndim = 1;
tensor.dtype = { kDLFloat, 32, 1 };
tensor.shape = shape;
// Keep the source buffer and descriptors alive until completion.
ovstage_write_data_t write{};
write.tensors = &tensor;
write.tensor_count = 1;
write.is_array = false;
write.count = 2;
waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("heat") },
    14, write, OVSTAGE_PRIM_MODE_UPSERT), "write heat");
// A = 300, B = 400, C = 200.
```

### Semantic

```cpp
// TOKEN_ID requires interned token identifiers, not character strings.
const ovx_string_t names[] = { literal_to_ovx_string("steel"),
    literal_to_ovx_string("rubber"), literal_to_ovx_string("wood") };
ovx_token_t values[3]{};
checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, names, 3, values), "intern materials");
int64_t shape[] = { 3 };
DLTensor tensor{};
tensor.data = values;
tensor.device = { kDLCPU, 0 };
tensor.ndim = 1;
tensor.dtype = { kDLUInt, 64, 1 };
tensor.shape = shape;
// Keep the source buffer and descriptors alive until completion.
ovstage_write_data_t write{};
write.tensors = &tensor;
write.tensor_count = 1;
write.is_array = false;
write.semantic = OVSTAGE_SEMANTIC_TOKEN_ID;
waitOp(stage, ovstage_write_attribute(stage, query, { 0, literal_to_ovx_string("material") },
    15, write, OVSTAGE_PRIM_MODE_UPSERT), "write material");
```

### Watermark

```cpp
// Make heat writes through ordinal 14 available for reads.
const ovx_string_t name = literal_to_ovx_string("heat");
ovx_token_t heat = OVX_INVALID_TOKEN;
checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &heat), "intern heat");
ovstage_write_floor_desc_t watermark{};
watermark.ordinal = 14;
watermark.scope = OVSTAGE_SCOPE_INCLUDE;
watermark.attributes = &heat;
watermark.attribute_count = 1;
waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance heat watermark");
// Finish all attribute writes through ordinal 15, including material.
watermark = {};
watermark.ordinal = 15;
watermark.scope = OVSTAGE_SCOPE_ALL;
waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance all watermarks");
```

## Reading the written attributes

These snippets follow the write-options examples using the same three-prim
query. The watermark is 15. `heat` is 300 on A, 400 on B, and 200 on C;
only A and B changed at ordinal 14.

### Copy groups

```cpp
const ovx_string_t name = literal_to_ovx_string("heat");
ovx_token_t attribute = OVX_INVALID_TOKEN;
checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "intern attribute");
ovstage_ordinal_range_t range{};
range.end_ordinal = 15;
ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read attributes");
std::vector<float> saved;
for (;;)
{
    ovstage_read_group_t group{};
    const auto status = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
    if (status == OVSTAGE_ERROR_END_OF_ITERATION)
        break;
    check(stage, status, "fetch group");
    const DLTensor& tensor = group.data.tensors[0];
    const auto* values = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
    for (uint32_t i = 0; i < group.prims.count; ++i)
    {
        const uint32_t row = group.data.index_map ? group.data.index_map[i] : i;
        saved.push_back(values[row]); // Copy the value before releasing its group.
    }
    check(stage, ovstage_release_group(stage, &group), "release group");
}
waitOp(stage, ovstage_release_read(stage, read), "release read");
// Copied values remain valid after both groups and the read are released.
std::sort(saved.begin(), saved.end());
std::printf("Copied heat values:");
for (float value : saved)
    std::printf(" %.1f", value);
std::printf("\n");
// Copied heat values: 200.0 300.0 400.0
```

### Fixed vectors

```cpp
const ovx_string_t name = literal_to_ovx_string("velocity");
ovx_token_t attribute = OVX_INVALID_TOKEN;
checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "intern attribute");
ovstage_ordinal_range_t range{};
range.end_ordinal = 15;
ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read attributes");

for (;;)
{
    ovstage_read_group_t group{};
    const auto status = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
    if (status == OVSTAGE_ERROR_END_OF_ITERATION)
        break;
    check(stage, status, "fetch group");
    // Raw tensor shape is (N, 3), with scalar float32 lanes.
    const DLTensor& tensor = group.data.tensors[0];
    const auto* values = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
    for (uint32_t i = 0; i < group.prims.count; ++i)
    {
        const uint32_t row = group.data.index_map ? group.data.index_map[i] : i;
        const float* vector = values + row * 3;
        // Use vector[0..2] here, while the group still owns the buffer.
        (void)vector;
    }
    check(stage, ovstage_release_group(stage, &group), "release group");
}
waitOp(stage, ovstage_release_read(stage, read), "release read");
```

### Change range

```cpp
const ovx_string_t name = literal_to_ovx_string("heat");
ovx_token_t attribute = OVX_INVALID_TOKEN;
checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "intern attribute");
ovstage_ordinal_range_t range{};
// A and B changed at 14; C last changed at 12.
range.has_start_ordinal = true;
range.start_ordinal = 14;
range.end_ordinal = 14;
ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read attributes");
std::vector<float> saved;
for (;;)
{
    ovstage_read_group_t group{};
    const auto status = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
    if (status == OVSTAGE_ERROR_END_OF_ITERATION)
        break;
    check(stage, status, "fetch group");
    if (group.is_delete)
    {
        // Deletions carry no value tensor; still release their group.
        check(stage, ovstage_release_group(stage, &group), "release deletion group");
        continue;
    }
    const DLTensor& tensor = group.data.tensors[0];
    const auto* values = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
    for (uint32_t i = 0; i < group.prims.count; ++i)
    {
        const uint32_t row = group.data.index_map ? group.data.index_map[i] : i;
        saved.push_back(values[row]); // Copy the value before releasing its group.
    }
    check(stage, ovstage_release_group(stage, &group), "release group");
}
waitOp(stage, ovstage_release_read(stage, read), "release read");
std::sort(saved.begin(), saved.end());
std::printf("Heat values changed at 14:");
for (float value : saved)
    std::printf(" %.1f", value);
std::printf("\n");
// Heat values changed at 14: 300.0 400.0
```

### Retained records

```cpp
// Query the boundary before relying on exact membership of an old interval.
ovstage_ordinal_query_handle_t ordinalQuery{};
waitOp(stage, ovstage_get_oldest_preserved_ordinal(stage, &ordinalQuery), "query oldest ordinal");
ovstage_ordinal_t oldest = 0;
check(stage, ovstage_fetch_ordinal(stage, ordinalQuery, OVSTAGE_TIMEOUT_INFINITE, &oldest), "fetch ordinal");
waitOp(stage, ovstage_release_ordinal_query(stage, ordinalQuery), "release ordinal query");
const ovstage_ordinal_t requestedStart = 14;
if (requestedStart < oldest)
{
    std::fprintf(stderr, "Change records for this interval are no longer exact\n");
    std::exit(EXIT_FAILURE);
}
// A subsequent range read can still fail if a newer write replaced its values.
```

## Clone rules and completion

These snippets use the single-prim `/World/Proto/Rig` created earlier.
Its mass was written at ordinal 2 and the watermark has reached 3.

### Duplicate targets

```cpp
// A target path may appear only once in a clone request.
const ovx_string_t targets[] = { literal_to_ovx_string("/World/Duplicate/Rig"),
    literal_to_ovx_string("/World/Duplicate/Rig") };
const auto operation = ovstage_clone(stage, literal_to_ovx_string("/World/Proto/Rig"), targets, 2, 4);
if (operation.status != OVSTAGE_ERROR_INVALID_ARGUMENT)
{
    std::fprintf(stderr, "Expected duplicate targets to be rejected\n");
    std::exit(EXIT_FAILURE);
}
// Submission failed, so there is no pending operation to wait for or release.
std::printf("Duplicate targets rejected before cloning.\n");
// Duplicate targets rejected before cloning.
```

### Async report

```cpp
const ovx_string_t target = literal_to_ovx_string("/World/Async/Rig");
const auto operation = ovstage_clone(stage, literal_to_ovx_string("/World/Proto/Rig"), &target, 1, 4);
check(stage, operation.status, "submit clone");
// Independent application work can run here while the clone is pending.
ovstage_op_wait_result_t wait{};
check(stage, ovstage_wait_op(stage, operation.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait), "wait clone");
checkWait(stage, wait, "clone completion");
ovstage_clone_result_t report{};
check(stage, ovstage_fetch_clone_result(stage, operation.op_index, OVSTAGE_TIMEOUT_INFINITE, &report),
      "fetch clone report");
std::printf("Async clone copied %llu prim to %llu target\n",
    static_cast<unsigned long long>(report.prims_cloned), static_cast<unsigned long long>(report.target_count));
// Async clone copied 1 prim to 1 target
check(stage, ovstage_release_op(stage, operation.op_index), "release clone operation");
// Advance the watermark before reading copied values.
```

### Specific failure

```cpp
const ovx_string_t target = literal_to_ovx_string("/World/MissingCopy");
const auto operation = ovstage_clone(stage, literal_to_ovx_string("/World/Missing"), &target, 1, 4);
check(stage, operation.status, "submit clone");
// Fetch waits for completion and returns the clone-specific failure code.
ovstage_clone_result_t report{};
const auto status = ovstage_fetch_clone_result(stage, operation.op_index, OVSTAGE_TIMEOUT_INFINITE, &report);
check(stage, ovstage_release_op(stage, operation.op_index), "release failed clone operation");
if (status != OVSTAGE_ERROR_NOT_FOUND)
{
    std::fprintf(stderr, "Expected a missing-source clone failure\n");
    std::exit(EXIT_FAILURE);
}
std::printf("Clone source was not found.\n");
// Clone source was not found.
```

Error helpers in these C examples terminate the process immediately. This also
applies to the CUDA ingest example: failure paths rely on process teardown to
reclaim resources. Applications that recover from errors must finish pending
GPU and ovstage work before freeing buffers and releasing stage handles.
