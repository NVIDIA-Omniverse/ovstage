# ovstage write workflows (C/C++)

A tour of the higher-level ovstage write workflows via the core C API: one
standalone program, three numbered sections. It is the C sibling of
`../../python/write-flavors/main.py` (whose section 4, GPU ingest, is Python-only).

> The fine-grained write **contracts** — column shapes, semantics, UPSERT/INSERT
> admission, sparse `index_map`/`mask`, delete tombstones, and CPU map/unmap —
> are **asserted by the public tests** under `../../../tests/` (see that tree's
> `AGENTS.md`). This example stays a workflow tour; the tests are the contract.
> In particular, `../../../tests/c/test_minimal.cpp` verifies that a fixed-size
> matrix written as `[N, 4, 4]` with `lanes = 1` reads back raw as `[N]` with
> `lanes = 16`.

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
== 3. pipelined submission ==
4 writes enqueued (ordinals 4..7) with zero waits; the CPU stays busy meanwhile
all 4 drained by zero-timeout polls and released; floor -> 7
latest sample after the pipeline: 400 401 402
```

- Each section writes at its own ordinals, then advances the write floor (seals
  everything at or below that ordinal) before reading back. The floor is global
  and monotonic, so the sections' ordinals increase across the run (1, 2–3, 4–7).
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
  pair (see the header docs); the Python sibling shows that path with a warp CUDA array.
- Reads may split one query across several groups, each with its own prim/data
  index maps; `readLatestRows` in `main.cpp` is the robust way to consume them.
- A batched `ovstage_write_attributes` is one operation (one op id, one structural
  precreate), not an atomic transaction — entries may apply incrementally.
- The examples fail fast: any unexpected API failure prints and exits (helpers in
  `../common/ovstage_example_utils.h`). A real application would propagate errors instead.

