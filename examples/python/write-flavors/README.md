# ovstage write workflows (Python)

A tour of the higher-level ovstage write workflows: one headless Python program,
five numbered sections. It is the Python sibling of
`../../c/write-flavors/main.cpp` (sections 1–3 cover the same flows, and both
languages include write-option examples; section 4 uses Python's Warp integration).

> The fine-grained write **contracts** — column shapes, semantics, UPSERT/INSERT
> admission, sparse `index_map`/`mask`, delete tombstones, and CPU map/unmap —
> are **asserted by the public tests** under `../../../tests/` (see that tree's
> `AGENTS.md`). This example stays a workflow tour; the tests are the contract.
> In particular, `../../../tests/python/test_minimal.py` verifies that a convenience matrix input
> `(N, 4, 4)` reads back as a scalar-lane `(N, 4, 4)` matrix and exports through
> DLPack with that shape. `../../../tests/python/test_map_attribute.py` verifies
> that maps use the same scalar-lane shapes.

## At a glance

Each numbered section is independent — write, seal, read back, print:

1. Batched writes — two columns in one operation.
2. Clone — stamp one subtree onto two targets, then re-query.
3. Pipelined submission — enqueue several ordinals ahead, drain with zero-timeout polls.
4. GPU ingest (Python only) — hand a warp CUDA array straight to a write.
5. Write options — use explicit shapes, sparse writes, semantics, and read-to-write forwarding.

## What you'll see

The output below is with warp and a CUDA device; without them the GPU readback
line becomes `gpu ingest skipped: warp with a CUDA device is not available`.
The three `Velocity:` lines may appear in a different order because read groups
need not arrive in query order.

```
== 1. batched writes ==
heat: [7. 8.]
tint: [0.1 0.2 0.3] [0.4 0.5 0.6]
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
== 4. GPU ingest (Python-only) ==
gpu-samples read back on cpu: [5. 6. 7.]
Copied heat values: [200.0, 300.0, 400.0]
Velocity: [1.0, 0.0, 0.0]
Velocity: [0.0, 1.0, 0.0]
Velocity: [0.0, 0.0, 1.0]
Heat values changed at 14: [300.0, 400.0]
```

- Each section writes at its own ordinals and advances the write floor (sealing
  everything up to it) before reading back. The floor is global and monotonic,
  so the sections' ordinals increase across the run (1, 2–4, 4–7, 8, 9–15).
- Section 3 shows the pipelined programming model, not a speedup — current
  releases may execute enqueued operations serially. The bindings pin each
  caller-owned tensor on its `Operation`, so holding the pending operations keeps
  the buffers valid until each op completes.
- Section 4 hands a warp CUDA array straight to `write_attribute` (DLPack, no
  host copy on the write leg); the read-back lands on the CPU.

## Build and run

The example is a [uv](https://docs.astral.sh/uv/) project pinning the released
`ovstage` wheel (see `pyproject.toml`); the wheel bundles a shared
library at `<package>/bin`, which the bindings load automatically:

```bash
uv run main.py
```

> **Pre-release:** if `uv` cannot resolve the pinned `ovstage` wheel, no package
> index available to you carries it yet — check the repository releases page for
> current availability.

Section 4 also needs [warp](https://pypi.org/project/warp-lang/) and a
CUDA-capable GPU (`uv add warp-lang`, or `pip install warp-lang`); without them
it prints a skip line.


## Snippets

The `[snippet:name]` markers in `main.py` fence regions referenced by the ovstage
skills under `../../../skills/`; keep them intact when editing.

- `setup` — the imports the sections rely on
- `batched-write-attributes` — two columns in one `write_attributes` op
- `clone-and-requery` — clone to N targets, then a fresh path-list query for readback
- `pipelined-submission` — enqueue several ordinals ahead without blocking
- `poll-wait-release` — `wait_op(timeout=0)` poll loop, TIMEOUT handling, `release_op`
- `gpu-warp-ingest` — writing a warp CUDA array device buffer, CPU read-back print

## Notes

- Section 4 needs warp and a CUDA-capable GPU. Without a CUDA device, warp may
  log a `Warp CUDA error 100` notice to stderr; stdout stays deterministic.
- Keep the source GPU array alive, and the producing device synchronized, until
  the write's Operation completes — `.wait()` covers both in this example.
  These unplaced reads return CPU tensors. The experimental device-placement
  extension also supports CUDA reads; see the tensor-exchange guide.
- A batched `write_attributes` is one operation (one op id, one structural
  precreate), not an atomic transaction — entries may apply incrementally.
- The example fails fast: any unexpected API failure raises `OvstageError` and
  aborts the run. A real application would handle errors instead.


## CUDA tensor-write excerpt

With the stage, query, attribute token, and Warp already set up, synchronize the
producer and retain the CUDA array until the write finishes:

```python
# Finish producing GPU data, then retain it until the write completes.
device_values = wp.array(np.array([5.0, 6.0, 7.0], np.float32), dtype=wp.float32, device="cuda:0")
wp.synchronize()
write = stage.write_attribute(
    query, gpu_samples, ordinal=8, tensors=device_values, is_array=False
)
# Keep device_values alive and unchanged while the write is in progress.
write.wait()
del device_values  # Safe to release our reference after completion.
```

## Writing individual attributes

These snippets run in order on a query containing `/World/Writes/A`,
`/World/Writes/B`, and `/World/Writes/C`.
Use the imports shown in `main.py`.

### Parameters

```python
# One scalar per prim; wait while values is still alive.
values = np.array([10, 20, 30], dtype=np.float32)
stage.write_attribute(query, "heat", ordinal=9, tensors=values, is_array=False).wait()
```

### Fixed shape

```python
# Three rows, each containing one three-component vector.
velocities = np.array([[1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=np.float32)
stage.write_attribute(query, "velocity", ordinal=10,
                      tensors=velocities, is_array=False).wait()
```

### Array width

```python
# Two float3 elements for A, one for B, and three for C.
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

### Index map

```python
# Reuse source row 0 for A and B, and source row 1 for C.
values = np.array([100, 200], dtype=np.float32)
stage.write_attribute(query, "heat", ordinal=12, tensors=values,
                      is_array=False, index_map=[0, 0, 1], count=3).wait()
# A = 100, B = 100, C = 200.
```

### Mask

```python
# Only bit 1 is set, so only B receives a value. A and C keep theirs.
values = np.array([0, 250, 0], dtype=np.float32)
stage.write_attribute(query, "heat", ordinal=13, tensors=values,
                      is_array=False, mask=[0b010], count=3).wait()
# A = 100, B = 250, C = 200.
```

### Count

```python
# Address just the first two prims in query order: A and B.
values = np.array([300, 400], dtype=np.float32)
stage.write_attribute(query, "heat", ordinal=14, tensors=values,
                      is_array=False, count=2).wait()
# A = 300, B = 400, C = 200.
```

### Semantic

```python
# TOKEN_ID requires interned token identifiers stored as uint64 values.
materials = np.array([paths.intern_token(name) for name in ["steel", "rubber", "wood"]],
                     dtype=np.uint64)
stage.write_attribute(query, "material", ordinal=15, tensors=materials,
                      is_array=False, semantic=AttributeSemantic.TOKEN_ID).wait()
```

### Watermark

```python
# Make heat writes through ordinal 14 available for reads.
heat = paths.intern_token("heat")
stage.advance_write_floor(ordinal=14, scope=ovstage.Scope.INCLUDE, attributes=[heat]).wait()
# Finish all attribute writes through ordinal 15, including material.
stage.advance_write_floor(ordinal=15).wait()
```

## Reading the written attributes

These snippets follow the write-options examples using the same three-prim
query. The watermark is 15. `heat` is 300 on A, 400 on B, and 200 on C;
only A and B changed at ordinal 14.

### Copy groups

```python
# Copy selected values before releasing each group; copies outlive the read.
heat = paths.intern_token("heat")
saved = []
with stage.read_attributes(query, [heat], OrdinalRange.latest(15)) as read:
    read.wait()
    for group in read.groups():
        with group:
            rows = group.array(0)
            for i in range(group.prim_count):
                row = group.data_row_index(i) if group.has_data_index_map else i
                saved.append(float(rows[row]))
# Sorting is for display; read groups need not arrive in query order.
print("Copied heat values:", sorted(saved))
# Copied heat values: [200.0, 300.0, 400.0]
```

### Fixed vectors

```python
velocity = paths.intern_token("velocity")
with stage.read_attributes(query, [velocity], OrdinalRange.latest(15)) as read:
    read.wait()
    for group in read.groups():
        with group:
            # Raw and NumPy shapes are (N, 3), with scalar lanes.
            vectors = np.from_dlpack(group.dlpack(0))
            for i in range(group.prim_count):
                row = group.data_row_index(i) if group.has_data_index_map else i
                print("Velocity:", vectors[row].tolist())
                # One of [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0].
```

### Change range

```python
# Only A and B changed heat at ordinal 14; C last changed at ordinal 12.
heat = paths.intern_token("heat")
changed = []
with stage.read_attributes(query, [heat], OrdinalRange.between(14, 14)) as read:
    read.wait()
    for group in read.groups():
        with group:
            if group.is_delete:
                continue  # Deletions carry no value tensor.
            values = group.array(0)
            for i in range(group.prim_count):
                row = group.data_row_index(i) if group.has_data_index_map else i
                changed.append(float(values[row]))
print("Heat values changed at 14:", sorted(changed))
# Heat values changed at 14: [300.0, 400.0]
```

### Retained records

```python
# Query the boundary before relying on exact membership of an old interval.
with stage.get_oldest_preserved_ordinal() as ordinal_query:
    ordinal_query.wait()
    oldest = ordinal_query.fetch()
requested_start = 14
if requested_start < oldest:
    raise RuntimeError("Change records for this interval are no longer exact")
# A subsequent range read can still fail if a newer write replaced its values.
```

## Clone rules and completion

These snippets use the single-prim `/World/Proto/Rig` created earlier.
Its mass was written at ordinal 2 and the watermark has reached 3.

### Duplicate targets

```python
# A target path may appear only once in a clone request.
try:
    stage.clone_async("/World/Proto/Rig", ["/World/Duplicate/Rig", "/World/Duplicate/Rig"],
                      ordinal=4).wait()
except OvstageError as error:
    if error.code != ovstage.ErrorCode.INVALID_ARGUMENT:
        raise
    print("Duplicate targets rejected before cloning.")
    # Duplicate targets rejected before cloning.
else:
    raise RuntimeError("Expected duplicate targets to be rejected")
```

### Async report

```python
# Submission returns an operation; wait when the copies are needed.
operation = stage.clone_async("/World/Proto/Rig", ["/World/Async/Rig"], ordinal=4)
# Independent application work can run here while the clone is pending.
report = operation.wait().result()
print("Async clone copied", report.prims_cloned, "prim to", report.target_count, "target")
# Async clone copied 1 prim to 1 target
# Advance the watermark before reading copied values.
```

### Specific failure

```python
operation = stage.clone_async("/World/Missing", ["/World/MissingCopy"], ordinal=4)
if not operation.ok:
    operation.wait()  # Raise a submission error before trying to fetch a report.
try:
    # Fetch waits for completion and preserves the clone-specific failure code.
    # Do not call operation.wait() first: it releases the native report.
    stage.fetch_clone_result(operation)
except OvstageError as error:
    if error.code != ovstage.ErrorCode.NOT_FOUND:
        raise
    print("Clone source was not found.")
    # Clone source was not found.
else:
    raise RuntimeError("Expected a missing-source clone failure")
finally:
    stage.release_op(operation.op_id)  # Fetching the report does not release it.
```
