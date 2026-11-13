# ovstage write-flavors (Python)

Tour several ways to update runtime data: batch attributes, clone a subtree,
submit asynchronous writes, and control which rows receive values. The example
creates its own prims, so it needs no USD input file. Later sections read the
results back and show how a change interval differs from a latest-value read.

The [C/C++ example](../../c/write-flavors/README.md) covers the same CPU workflows.

## At a glance

An ordinal orders writes. The example waits for operations and advances the write
floor before reading their results. Pipelining separates submission from waiting;
it does not remove the requirement to keep each input buffer alive and unchanged
until its write completes.

Pseudocode (workflow only):

```text
batch heat and tint into one write; read both back
clone a prototype into two environments; query the new paths
submit four writes with distinct buffers; poll and drain them
if Warp and CUDA are available, write a device buffer and read it on the CPU
write scalars, vectors, and per-prim arrays
apply a source index map, a mask, and a limited count
finish writes; compare latest values with changes at ordinal 14
```

### Fixed vectors versus per-prim arrays

The same three prims, A, B, and C, can each receive one vector or an array of
vectors. The distinction is the number of values per prim, not whether the input
happens to be stored in an array. These excerpts use the setup in the runnable
example; they are not standalone programs.

**One vector per prim** ([`write-fixed-shape`](main.py)). The NumPy
shape is `(3, 3)`: three prim rows, each with three components. `is_array=False`
makes each row one fixed-size value. The existing `query` selects A, B, and C.

```python
velocities = np.array([[1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=np.float32)
stage.write_attribute(query, "velocity", ordinal=10,
                      tensors=velocities, is_array=False).wait()
```

**An array of vectors per prim** ([`write-array-width`](main.py)). Each NumPy
array below belongs to one prim. `len(row)` counts vector rows; the three-lane
element type describes each row as one float3 element. `is_array=True` selects
per-prim array storage.

```python
points = [
    np.array([[0, 0, 0], [1, 0, 0]], dtype=np.float32),
    np.array([[0, 1, 0]], dtype=np.float32),
    np.array([[0, 0, 1], [1, 0, 1], [2, 0, 1]], dtype=np.float32),
]
tensors = [make_dltensor(row,
    dtype=DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3),
    shape=[len(row)]) for row in points]
stage.write_attribute(query, "points", ordinal=11,
                      tensors=tensors, is_array=True).wait()
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

Start in `main()` in [main.py](main.py), then follow the helpers it calls.
The names below are searchable `[snippet:name]` markers in that file.

| Source markers | What to learn |
|---|---|
| `batched-write-attributes` | Write a scalar column and a three-component tint column together; read both back to connect the batch to its results. |
| `clone-and-requery`, `clone-duplicate-targets`, `clone-async-report`, `clone-specific-failure` | Clone into new paths, re-query them, then inspect duplicate-target rejection, an asynchronous report, and a missing-source error. |
| `pipelined-submission`, `poll-wait-release` | Submit four writes before waiting, keeping distinct backing buffers. Poll with zero timeout and release completed operations. |
| `gpu-warp-ingest`, `dlpack-cuda-ingest` | Optionally hand a Warp CUDA buffer to a write, wait before dropping it, and read its values back on the CPU. |
| `write-parameters`, `write-fixed-shape`, `write-array-width` | Contrast one scalar per prim, one fixed vector per prim, and variable-length arrays of float3 elements. |
| `write-index-map`, `write-mask`, `write-count` | Reuse source rows, update only B, then update just A and B. Each step changes a different part of the destination/source mapping. |
| `write-semantic`, `write-watermark` | Store interned material tokens with TOKEN_ID semantics; advance the heat-specific floor and then the global floor. |
| `read-copy-groups`, `read-fixed-vectors` | Copy values that must outlive a read group and honor data index maps when reading vector rows. |
| `read-change-range`, `read-retained-records` | Read changes at ordinal 14, then check the oldest preserved change-record boundary before relying on interval membership. |

## Build and run

See the [shared setup](../../README.md) for package prerequisites.
Run these commands from this example's directory.

```bash
uv run main.py
```

The Warp section runs when Warp and a CUDA device are available; otherwise it
prints a skip message and the CPU sections continue. To supply Warp without
editing the project, run `uv run --with warp-lang main.py`.


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
