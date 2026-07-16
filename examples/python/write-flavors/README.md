# ovstage write workflows (Python)

A tour of the higher-level ovstage write workflows: one headless Python program,
four numbered sections. It is the Python sibling of
`../../c/write-flavors/main.cpp` (sections 1–3 are identical flows; section 4 is
Python-only).

> The fine-grained write **contracts** — column shapes, semantics, UPSERT/INSERT
> admission, sparse `index_map`/`mask`, delete tombstones, and CPU map/unmap —
> are **asserted by the public tests** under `../../../tests/` (see that tree's
> `AGENTS.md`). This example stays a workflow tour; the tests are the contract.

## At a glance

Each numbered section is independent — write, seal, read back, print:

1. Batched writes — two columns in one operation.
2. Clone — stamp one subtree onto two targets, then re-query.
3. Pipelined submission — enqueue several ordinals ahead, drain with zero-timeout polls.
4. GPU ingest (Python only) — hand a warp CUDA array straight to a write.

## What you'll see

The output below is with warp and a CUDA device; without them the last line
becomes `gpu ingest skipped: warp with a CUDA device is not available`.

```
== 1. batched writes ==
heat: [7. 8.]
tint: [0.1 0.2 0.3] [0.4 0.5 0.6]
== 2. clone ==
mass /World/Env0/Rig = 5.0
mass /World/Env1/Rig = 5.0
== 3. pipelined submission ==
4 writes enqueued (ordinals 4..7) with zero waits; the CPU stays busy meanwhile
all 4 drained by zero-timeout polls and released; floor -> 7
latest sample after the pipeline: 400 401 402
== 4. GPU ingest (Python-only) ==
gpu-samples read back on cpu: [5. 6. 7.]
```

- Each section writes at its own ordinals and advances the write floor (sealing
  everything up to it) before reading back. The floor is global and monotonic,
  so the sections' ordinals increase across the run (1, 2–3, 4–7, 8).
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
  the write's Operation completes — `.wait()` covers both. Reads always return
  CPU tensors through this API (there is no public GPU read).
- A batched `write_attributes` is one operation (one op id, one structural
  precreate), not an atomic transaction — entries may apply incrementally.
- The example fails fast: any unexpected API failure raises `OvstageError` and
  aborts the run. A real application would handle errors instead.

