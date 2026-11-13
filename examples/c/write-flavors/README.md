# ovstage write-flavors (C/C++)

Tour several ways to update runtime data: batch attributes, clone a subtree,
submit asynchronous writes, and control which rows receive values. The example
creates its own prims, so it needs no USD input file. Later sections read the
results back and show how a change interval differs from a latest-value read.

The [Python example](../../python/write-flavors/README.md) covers the same CPU workflows.

## At a glance

An ordinal orders writes. The example waits for operations and advances the write
floor before reading their results. Pipelining separates submission from waiting.
For the client-managed tensors used here, keep each input buffer, `DLTensor`
descriptor, and its shape and stride storage alive and unchanged until its write
completes.

Pseudocode (workflow only):

```text
batch heat and tint into one write; read both back
clone a prototype into two environments; query the new paths
submit four writes with distinct buffers; poll and drain them
write scalars, vectors, and per-prim arrays
apply a source index map, a mask, and a limited count
finish writes; compare latest values with changes at ordinal 14
```

### Fixed vectors versus per-prim arrays

The same three prims, A, B, and C, can each receive one vector or an array of
vectors. The distinction is the number of values per prim, not whether the input
happens to be stored in an array. These excerpts use the setup in the runnable
example; they are not standalone programs.

**One vector per prim** ([`write-fixed-shape-c`](main.cpp)). A single tensor has
shape `(3, 3)`: three prim rows, each with three scalar components. The surrounding
write sets `is_array = false`.

```cpp
float values[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
int64_t shape[] = { 3, 3 };
DLTensor tensor{};
tensor.data = values;
tensor.device = { kDLCPU, 0 };
tensor.ndim = 2;
tensor.dtype = { kDLFloat, 32, 1 };
tensor.shape = shape;
```

**An array of vectors per prim** ([`write-array-width-c`](main.cpp)). Three tensors
describe arrays of lengths 2, 1, and 3. Here `lanes = 3` packs each float3 as one
element, so a tensor's one-dimensional shape counts vectors, not scalar components.
The surrounding write passes all three tensors with `is_array = true`.

```cpp
float a[] = { 0, 0, 0, 1, 0, 0 };
float b[] = { 0, 1, 0 };
float c[] = { 0, 0, 1, 1, 0, 1, 2, 0, 1 };
int64_t lengths[] = { 2, 1, 3 };
DLTensor tensors[] = {
    { a, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, &lengths[0], nullptr, 0 },
    { b, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, &lengths[1], nullptr, 0 },
    { c, { kDLCPU, 0 }, 1, { kDLFloat, 32, 3 }, &lengths[2], nullptr, 0 },
};
```

Both write sections wait while their source buffers are still alive.

## What you'll see

These selected results connect the workflows to their readback:

| Workflow | Result and meaning |
|---|---|
| Batched attributes | Heat values `7, 8` and two tint vectors are submitted in one operation. |
| Clone and re-query | Both cloned rigs have `mass = 5`; the new query addresses the clones. |
| Pipelined writes | The final sample is `400, 401, 402`, after all four writes finish. |
| Index map, mask, count | Heat ends at A = 300, B = 400, C = 200. The sorted display is `200, 300, 400`. |
| Change interval at 14 | Only A and B changed heat there, yielding `300, 400`; C last changed at 12. |

Duplicate clone destinations and a missing source are intentional error cases.
The asynchronous clone also checks its completion report. These checks demonstrate
how to inspect failures and completion, not just how to submit work.

## How to read the code

Start in `main()` in [main.cpp](main.cpp), then follow the helpers it calls.
The names below are searchable `[snippet:name]` markers in that file.

| Source markers | What to learn |
|---|---|
| `batched-write-attributes` | Write a scalar column and a three-component tint column together; read both back to connect the batch to its results. |
| `clone-and-requery`, `clone-duplicate-targets-c`, `clone-async-report-c`, `clone-specific-failure-c` | Clone into new paths, re-query them, then inspect duplicate-target rejection, an asynchronous report, and a missing-source error. |
| `pipelined-submission`, `poll-wait-release` | Submit four writes before waiting, keeping distinct backing buffers. Poll with zero timeout and release completed operations. |
| `write-parameters-c`, `write-fixed-shape-c`, `write-array-width-c` | Contrast one scalar per prim, one fixed vector per prim, and variable-length arrays of float3 elements. |
| `write-index-map-c`, `write-mask-c`, `write-count-c` | Reuse source rows, update only B, then update just A and B. Each step changes a different part of the destination/source mapping. |
| `write-semantic-c`, `write-watermark-c` | Store interned material tokens with TOKEN_ID semantics; advance the heat-specific floor and then the global floor. |
| `read-copy-groups-c`, `read-fixed-vectors-c` | Copy values that must outlive a read group and honor data index maps when reading vector rows. |
| `read-change-range-c`, `read-retained-records-c` | Read changes at ordinal 14, then check the oldest preserved change-record boundary before relying on interval membership. |

## Build and run

See the [shared setup](../../README.md) for package prerequisites.
Run these commands from this example's directory.

Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build
./write-flavors
```

Windows (multi-configuration generator):

```powershell
cmake -S . -B build
cmake --build build --config Release
$env:PATH = "<ovstage-package>\bin;$env:PATH"
Set-Location build\Release
.\write-flavors.exe
```

Keep the package's `bin/` directory intact so the library can find its plugins.
Linux builds set an rpath to that directory. The project uses the shared
[cmake](../cmake/) and [common](../common/) directories; retain them if copying it.

The main program is CPU-only. [cuda_ingest.cpp](cuda_ingest.cpp) is a separate
optional executable: configure with `-DOVSTAGE_EXAMPLE_CUDA=ON` and run
`cuda-ingest` from the build directory. It requires the CUDA toolkit to build;
exit code 77 means no CUDA device was available, while zero means readback matched.


## Notes

- A batch groups completion; it is not an atomic transaction.
- A tensor's shape, element width, and per-prim array status describe different
  things. Read the scalar/vector/array sections together before changing layouts.
- Query order is not read-group order. The read helpers use group prim indices and per-column
  data indices to associate values with paths; sorting is only for presentation.
- Exact retained change records do not guarantee historical values remain
  readable after later writes replace them. The retention check is not a promise
  of permanent snapshots.
- This is a workflow tour, not a performance benchmark. Four submitted writes do
  not establish a throughput improvement or simultaneous execution.

## Further reading

- [Writing attributes](../../../docs/scene/writing_attributes.rst)
- [Cloning subtrees](../../../docs/scene/cloning.rst)
- [Reading attributes and change intervals](../../../docs/scene/reading_attributes.rst)
- [Producer-consumer example](../producer-consumer/README.md)
