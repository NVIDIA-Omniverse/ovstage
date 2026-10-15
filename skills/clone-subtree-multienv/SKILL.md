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
name: clone-subtree-multienv
description: >
  Clone the subtree under a source prim to one or more new target paths in a single
  ordinal-keyed call — including the multi-environment pattern (stamp out N copies of a
  prototype). Use when the user asks to clone, duplicate, or copy a prim/subtree, or to spawn
  per-environment instances (e.g. one scene/robot per RL environment).
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - clone
  - prims
tools:
  - Read
  - Grep
---

# Clone Subtree (Multi-Environment)

## When to Use

Use this skill when the user wants to clone, duplicate, or copy a prim subtree to new paths,
or to **stamp out N copies of a prototype** subtree in one call — the multi-environment / RL
pattern (one scene or robot per environment). Do **not** use it for USD references/instancing
or authoring new geometry from scratch; route those to the population/write skills.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- The source subtree path (must already exist on the stage) and one or more target paths
  (each must be a valid path and must **not** already exist).
- Target API surface: C, Python, or both.
- The clone ordinal must clear applicable floors and satisfy the configured
  admission policy for target cells and prim lifecycle. Under the strict policy,
  a later modification of a copied cell needs a newer ordinal.
- Whether the request is really a clone, vs. a USD reference/instance or fresh authoring.
- The shipped headers and the referenced example snippets are the authoritative contract.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced example snippets.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the async **submit/observe** model (`cpu-ahead-gpu-async`): clone is an enqueue —
  its `op_index` identifies accepted work; confirm completion with `wait_op` / `.wait()`.
- Confirm the source exists and every target path is free: the clone is **create-only and
  all-or-nothing** — a batch with any pre-existing target clones nothing.

## Instructions

1. Identify the source subtree path and every target path to create.
2. Choose an `ordinal` above the global and effective source-column floors;
   those checks apply even with no targets. Independently, clear target operation
   floors and satisfy the configured admission policy for target cells and prim
   lifecycle. Disjoint targets may share an ordinal.
3. Enqueue the clone with the target array — C `ovstage_clone`, Python `Stage.clone` (blocking)
   or `Stage.clone_async`.
4. Drive it to completion: await the op (C `ovstage_wait_op` + `ovstage_release_op`; Python
   `.wait()`) and check for per-op errors. After awaiting, read the completion report — C
   `ovstage_fetch_clone_result` before `ovstage_release_op`; Python `op.result()` on the
   `CloneOperation`, or the `CloneResult` returned by `Stage.clone`.
5. To make the clones readable, advance the write floor to/above the clone ordinal.
6. Do **not** use clone when the caller actually needs USD references/instancing or fresh
   authoring — route to the population/write skills instead.

## Output Format

- For explanations, cite the API names, the source snippet(s), and the key caveats
  (source-exists / targets-free / ordinal / relationship handling).
- For code changes, summarize the files touched, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before
  documenting new API usage.
- **Create-only, all-or-nothing.** Every target must be new; a batch mixing fresh and existing
  targets clones nothing (rejected before any prim is created).
  A prim that already exists below a new target is merged into. The merge is partial:
  the prim keeps attributes the source prim lacks and, if the source prim has no prim
  type, its own prim type; its applied schemas and ancestor types become the union of
  its own and the source prim's; and for a name the source prim holds both as a value
  and as a connection, the connection is copied only if the prim already has one.
  Delete such a prim first to get an exact copy.
- **Internal paths are rebased; external paths are shared.** Relationship targets, scalar
  and array path values, and USD attribute connections that point inside the source subtree are
  retargeted to each clone's corresponding prim or property. Paths outside the subtree stay
  unchanged, preserving bindings to shared material and resource scopes.
- **Change tracking.** Every cloned column is ordinal-change-tracked: attribute
  values, relationship targets, an attribute connection that has no value
  column, prim type / applied schema / ancestor type metadata, and each typed
  identity of an attribute name that has different types on different source
  prims. Scene hierarchy changes, such as parent child lists, are not. A name held
  both as a value and as a connection is tracked under its value type only.
- **Latest-snapshot build** — clones become visible at/below the write floor once you advance it;
  don't design around reading historical ordinals.
- **⚠️ Draft — API in flux.** Treat exact symbols/ordering as provisional against the headers.

## Overview

`ovstage_clone` copies the subtree under a source path to one or more new target paths — the
data-plane peer of ovrtx's `ovrtx_clone_usd` (the `_usd` postfix is dropped). Passing several
target paths in **one** call is the multi-environment pattern: stamp out N copies of a prototype
subtree (e.g. one scene/robot per RL environment) in a single enqueue.

Like `write_attribute`, clone is an **ordinal-keyed write**: it carries an `ordinal`, is sealed
by the write floor, and records the copied cells at that ordinal. Customizing those cells
afterward needs a newer ordinal under the `STRICTLY_INCREASING` policy,
even before sealing. See [Ordinal Admission Policies](../../docs/scene/writing_attributes.rst)
for `NONDECREASING`, which allows equal unsealed ordinals, and `FLOOR_ONLY`, which
skips retained-history admission. Both retain the clone's operation floors;
source-floor checks remain independent of target admission, so fresh targets do
not bypass them. The two history policies also check applicable prim-lifecycle
history and its `usd-path` floor.
The source must exist; each target must be new (create-only). Internal path-bearing
values are rebased per clone, while bindings to shared scopes outside the subtree
stay unchanged.

## C

Clone `/World/A` to two new environment targets in one call, then drive the enqueue to completion
(the source was written and sealed earlier in the example):

> **Source:** `examples/c/minimal/main.cpp` snippet `clone-subtree-multienv`

Awaiting and per-op error checking use the same enqueue/wait helper as every other data-plane op:

> **Source:** `examples/c/minimal/main.cpp` snippet `enqueue-wait-error`

The public C test asserts the create-only contract end to end — write a source, seal
it, clone the subtree to new targets, seal, then verify each clone is queryable with
the copied attribute value (not just that the enqueue was accepted):

> **Source:** `tests/c/test_clone.cpp` snippet `clone-and-verify-c`

Once `ovstage_wait_op` has returned, `ovstage_fetch_clone_result` copies the clone's completion
report (`ovstage_clone_result_t`: `prims_cloned`, `target_count`, `source_prim_count`,
`ordinal`). The report is per-op state that lives until `ovstage_release_op`, so fetch it before
releasing; a failed clone returns its own code (for example `OVSTAGE_ERROR_NOT_FOUND`,
`OVSTAGE_ERROR_PRIM_NOT_FOUND`, or `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`) from the fetch, where
`ovstage_wait_op` only reports `OVSTAGE_ERROR_OP_FAILED` plus the message:

> **Source:** `tests/c/test_clone.cpp` snippet `clone-result-c`

## Python

`Stage.clone` blocks and returns a `CloneResult` (raising `OvstageError` on failure);
`Stage.clone_async` returns a `CloneOperation` — an `Operation` whose `wait()` returns it — so
`report = op.wait().result()` chains. Clone to N targets in one call, then advance the write
floor so the clones are readable:

> **Source:** `examples/python/minimal/main.py` snippet `clone-subtree-multienv`

The public Python test asserts the same round-trip against the produced wheel — clone,
seal, then read back the copied value on each target:

> **Source:** `tests/python/test_clone.py` snippet `clone-and-verify`

The same test reads the completion report — `prims_cloned`, `target_count`,
`source_prim_count`, `ordinal` — from the `CloneOperation`:

> **Source:** `tests/python/test_clone.py` snippet `clone-result`

## Key Types / Functions

| Python | C |
|--------|---|
| `Stage.clone(source, targets, ordinal)` (blocking) → `CloneResult` | `ovstage_clone(instance, source, targets, count, ordinal)` |
| `Stage.clone_async(source, targets, ordinal)` → `CloneOperation` | (always async in C; await with `ovstage_wait_op`) |
| `CloneOperation.result(timeout)` → `CloneResult` | `ovstage_fetch_clone_result(instance, op_id, timeout, &result)` |
| `CloneResult` | `ovstage_clone_result_t` |

## Troubleshooting

- **`OVSTAGE_ERROR_NOT_FOUND` (missing source)** — the source path must already exist on the
  stage; the error names it. Populate or write it first.
- **`OVSTAGE_ERROR_PRIM_NOT_FOUND` (target exists)** — a target path already exists. Targets
  are create-only, and a batch with any existing target clones nothing. The error names the
  existing targets (e.g. `target prim already exists at '/World/envs/env_3'`), so the whole
  batch can be corrected in one round-trip. Use fresh paths. A prim that already exists *below*
  a new target (for example a child left by deleting only an earlier clone's root) is merged
  into instead (partially; see Limitations).
- **`OVSTAGE_ERROR_OP_FAILED` naming an existing prim** — a prim below a new target already
  holds a cloned attribute with another type, carries a prim type other than that of
  its typed source prim, or lacks a cloned attribute connection that has no value
  column, so the clone cannot merge into it. Delete that prim first.
- **`OVSTAGE_ERROR_OP_FAILED` naming a target root** — the target root is a property
  path, such as `/World/Tgt.attr`, and the source has child prims, which a property
  path cannot hold. Use a prim path.
- **`OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`** — the clone ordinal conflicts with an
  applicable source or target operation floor, or the configured admission policy
  for a target cell or prim lifecycle. Use a newer ordinal, then advance the floor
  after completion.
- **Enqueue succeeded but the clones aren't there** — enqueue success means *accepted*, not
  *executed*. Await the op (C `ovstage_wait_op`; Python `.wait()`), and advance the write floor
  to/above the clone ordinal to read the clones.
- **`OVSTAGE_ERROR_INVALID_ARGUMENT` at enqueue with "duplicate target path" or
  "is not a valid path"** — the same target path appears more than once in one call;
  the enqueue is rejected before anything runs. An empty or unparseable target path
  is rejected the same way.
  Nested targets (one under another target of the same batch) are *not* rejected and inflate
  `prims_cloned`, unless a nested target is also a prim cloned under another target; that clone
  fails at execution with `OVSTAGE_ERROR_OP_FAILED`.
  Pass each target once.
- **A clone still points at the original** — verify the target is actually inside the cloned
  source subtree. Only paths with the source root as a path prefix are rebased; references to
  shared scopes outside that subtree intentionally stay unchanged.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `application-flow` — where clone fits in the create → write → seal → read lifecycle.
- `cpu-ahead-gpu-async` — the async submit/observe model and ordinal/write-floor semantics clone shares with writes.
- `error-handling` — status checks and per-op error reporting (the codes above).
- `path-dictionary` / `string-handling` — building the source/target path strings (`ovx_string_t`).
- Keep related skills, docs, and snippets synchronized when changing the workflow.
