---
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.
name: runtime-loop
description: >
  Headless load -> populate -> read -> update -> read loop with no renderer attached: open a USD
  scene into the ovstage runtime table, read prims back to confirm, then update the live stage two
  ways — write straight into the ovstage table (e.g. animate a transform), or edit the USD source
  and propagate it through. Use when the user wants to drive an ovstage scene headlessly, populate
  USD into the runtime stage, or see runtime-table vs USD-source edits reflected in reads.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - population
  - usd
  - runtime
tools:
  - Read
  - Grep
---

# Runtime Loop (Populate, Read, Update)

## When to Use

Use this skill when the user wants to work an ovstage scene **headlessly** (no renderer): load a
USD scene and populate it into the runtime table, read prims back, and update the live stage. It
covers the **two update paths** a client has once a scene is live:

1. **Directly in the ovstage runtime table** — `write_attribute` at a new ordinal (e.g. animate a
   prim's `omni:xform` transform over frames). The fast GPU-side edit; no USD round-trip.
2. **Through the USD source** — `add_usd_reference` / `apply_usd_changes` (or `update_from_usd_time`
   for time samples), so USD-side edits propagate into the runtime table.

Do **not** use it for rendering a scene (that stays in ovrtx), or for the mechanics of a single
call — route those to the focused skills (`application-flow`, `dlpack-tensor-exchange`, etc.).

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- The scene: a USD file path (`open_usd`) or inline USDA string (`open_usd_from_string`).
- The prims to query and the attribute(s) to read/write (as interned tokens or strings).
- The caller-owned **ordinals**: one per populate/write/apply step, advanced monotonically.
- Which update path(s) the request needs: direct runtime-table write, USD-source edit, or both.
- Target API surface: C, Python, or both.
- The shipped headers and the referenced example snippets are the authoritative contract.

## Prerequisites

- Population entry points: `ovstage_population_*` in C, `ovstage.population` in Python.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the async **submit/observe** model (`cpu-ahead-gpu-async`): populate and write are
  enqueues that return an `op_index`; nothing is visible until you await and **advance the write
  floor**. Population has its own wait (`ovstage_population_wait_op`), parallel to the data-plane
  `ovstage_wait_op`.
- The **application owns the ordinal lifecycle** — population never opens or seals an ordinal; you
  pass the current one to each call and advance the write floor per tick.

## Instructions

1. **Populate.** `open_usd` (file) or `open_usd_from_string` (inline) at ordinal 1 with a
   `PopulationDomain`, then `advance_write_floor` to seal it so reads can see it.
2. **Read to confirm.** Read the reserved `usd-prim-type` metadata (auto-maintained for every
   populated prim) over a path-list query — the guaranteed proof the populate landed.
3. **Update path 1 — runtime table.** `write_attribute` at a new ordinal (e.g. a prim's
   `omni:xform` transform, `is_array=false`, `semantic=MATRIX`), advancing the write floor each
   step; read the column back to confirm.
4. **Update path 2 — USD source.** `add_usd_reference` (or `_from_string`) edits only USD;
   `apply_usd_changes` at a fresh ordinal propagates it into the runtime table. Advance the floor,
   then re-read to see the change.
5. **Own the ordinals.** Keep ordinals monotonic across all steps; a write/apply at or below the
   floor is a write-floor violation.

## Output Format

- For explanations, cite the API names, the source snippet(s), and the key caveats (population
  support, ordinal ownership, which update path, transform recipe).
- For code changes, summarize the files touched, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before
  documenting new API usage.
- **Transform layout: `omni:xform` is one 16-lane element per prim.** The canonical 4×4 double
  tensor uses `dtype.lanes = 16`, `shape = [1]` (`semantic = MATRIX`, row-vector convention,
  translation in row `[3][0..2]`). A compact `shape = [1, 4, 4]`, `lanes = 1` copy-in is also
  accepted, but the trailing dimensions are folded and not preserved: raw reads and maps return
  the canonical `shape = [1]`, `lanes = 16` layout.
- **Reading populated transforms.** This loop reads back the transform **it wrote itself** (works).
  The *populated* transform (`omni:fabric:localMatrix` / `omni:fabric:worldMatrix`) is not surfaced via
  `read_attributes` (computed downstream in ovrtx) — don't rely on reading a scene-authored transform.
- **Inline USDA for `add_usd_reference` must be multi-line.** A single-line layer-metadata + prim
  body does not parse through the anonymous-layer import path.
- **`apply_usd_time` / structural edits.** For the RENDERING domain, `apply_usd_changes` /
  `apply_usd_time` also reflect structural USD edits; other domains apply structural edits only via
  `apply_usd_changes`.
- **Latest-snapshot build** — data becomes visible at/below the write floor once you advance it;
  don't design around reading historical ordinals.

## Overview

ovstage is a headless, GPU-native runtime stage for USD data. The **population** bridge
(`ovstage_population.h`) reads USD (a file or inline USDA) and mirrors it into an ovstage instance;
the **data plane** (`ovstage.h`) then reads and writes columns against the populated prims. This
skill ties them into one loop: populate → read → update (table and/or USD) → read, with the
application owning the ordinal lifecycle and sealing each tick with `advance_write_floor`.

## C

Populate a USD file and seal the ordinal (population has its own enqueue/wait; the `waitPop`
helper mirrors the data-plane `waitOp`):

> **Source:** `examples/c/runtime-loop/main.cpp` snippet `populate`

Confirm the populate by reading the reserved `usd-prim-type` metadata and resolving the tokens:

> **Source:** `examples/c/runtime-loop/main.cpp` snippet `read-populated`

Update path 1 — write the transform straight into the runtime table over frames:

> **Source:** `examples/c/runtime-loop/main.cpp` snippet `update-table`

Update path 2 — edit the USD source and propagate it through:

> **Source:** `examples/c/runtime-loop/main.cpp` snippet `update-usd`

## Python

The Python population surface is `ovstage.population.*` (blocking `foo(...)` plus async
`foo_async(...)`). Imports:

> **Source:** `examples/python/runtime-loop/main.py` snippet `setup`

Populate and seal:

> **Source:** `examples/python/runtime-loop/main.py` snippet `populate`

Read the reserved `usd-prim-type` metadata to confirm the populate:

> **Source:** `examples/python/runtime-loop/main.py` snippet `read-populated`

Update path 1 — animate the transform straight into the runtime table:

> **Source:** `examples/python/runtime-loop/main.py` snippet `update-table`

Update path 2 — edit the USD source and propagate it through:

> **Source:** `examples/python/runtime-loop/main.py` snippet `update-usd`

## Key Types / Functions

| Python | C |
|--------|---|
| `population.open_usd(stage, path, ordinal, ...)` / `open_usd_from_string` | `ovstage_population_open_usd_from_file` / `_from_string` |
| `population.apply_usd_changes(stage, ordinal)` | `ovstage_population_apply_usd_changes` |
| `population.add_usd_reference_from_string(stage, usda, target)` | `ovstage_population_add_usd_reference_from_string` |
| `Stage.write_attribute(query, attr, ordinal, tensors, is_array=, semantic=)` | `ovstage_write_attribute` (+ `ovstage_write_data_t`) |
| `Stage.read_attributes(query, attrs, OrdinalRange)` → `Read` | `ovstage_read_attributes` (+ `ovstage_fetch_read_next`) |
| `Stage.advance_write_floor(ordinal)` | `ovstage_advance_write_floor` |

## Troubleshooting

- **Read fails with `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` after populate** — advance the write
  floor to the populate ordinal; reads only see sealed data at/below the floor.
- **`OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`** — a write/`apply_usd_changes` used an ordinal at/below
  the floor. Keep ordinals monotonic (populate 1 → writes 2..N → USD edit N+1).
- **USD edit didn't show up in reads** — `add_usd_reference` only edits USD; you must call
  `apply_usd_changes` at a fresh ordinal and advance the floor before re-reading.
- **Transform read looks wrong / empty** — see Limitations: reading a *populated* transform may not
  be surfaced; confirm the `omni:xform` recipe and that you're reading a column you wrote.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `application-flow` — the create → write → seal → read lifecycle this loop instantiates.
- `cpu-ahead-gpu-async` — the async submit/observe model and ordinal/write-floor semantics.
- `dlpack-tensor-exchange` — tensor interchange for the read/write payloads (matrices, columns).
- `error-handling` — status checks and per-op error reporting for enqueues and waits.
- `path-dictionary` / `string-handling` — interning prim paths and attribute names (`ovx_string_t`).
- Keep related skills, docs, and snippets synchronized when changing the workflow.
