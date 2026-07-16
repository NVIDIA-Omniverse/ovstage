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
  (each must **not** already exist).
- Target API surface: C, Python, or both.
- The ordinal for the clone — must be **above the current write floor** (and above the seal of
  every attribute the clone reproduces).
- Whether the request is really a clone, vs. a USD reference/instance or fresh authoring.
- The shipped headers and the referenced example snippets are the authoritative contract.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced example snippets.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the async **submit/observe** model (`cpu-ahead-gpu-async`): clone is an enqueue —
  it returns immediately with an `op_index`; nothing is created until you `wait_op` / `.wait()`.
- Confirm the source exists and every target path is free: the clone is **create-only and
  all-or-nothing** — a batch with any pre-existing target clones nothing.

## Instructions

1. Identify the source subtree path and every target path to create.
2. Choose an `ordinal` strictly above the current write floor (clone is an ordinal-keyed write,
   like `write_attribute`).
3. Enqueue the clone with the target array — C `ovstage_clone`, Python `Stage.clone` (blocking)
   or `Stage.clone_async`.
4. Drive it to completion: await the op (C `ovstage_wait_op` + `ovstage_release_op`; Python
   `.wait()`) and check for per-op errors.
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
- **Relationships are copied verbatim, not retargeted.** Relationship attributes (e.g.
  `material:binding`, `skel:skeleton`) are copied as-is — bindings to targets *outside* the
  cloned subtree resolve correctly (the common case, e.g. a shared materials scope), but targets
  *inside* the subtree are **not** retargeted to the clone's own copies (matches `ovrtx_clone_usd`).
- **Only value attributes are change-tracked.** Relationship changes, and connectivity changes
  such as the source/target parents' child lists, are not ordinal-change-tracked.
- **Latest-snapshot build** — clones become visible at/below the write floor once you advance it;
  don't design around reading historical ordinals.
- **⚠️ Draft — API in flux.** Treat exact symbols/ordering as provisional against the headers.

## Overview

`ovstage_clone` copies the subtree under a source path to one or more new target paths — the
data-plane peer of ovrtx's `ovrtx_clone_usd` (the `_usd` postfix is dropped). Passing several
target paths in **one** call is the multi-environment pattern: stamp out N copies of a prototype
subtree (e.g. one scene/robot per RL environment) in a single enqueue.

Like `write_attribute`, clone is an **ordinal-keyed write**: it carries an `ordinal`, is sealed
by the write floor, and can never mutate sealed ordinals. The source must exist; each target must
be new (create-only). Relationship targets are cloned (copied verbatim), so a clone's bindings to
a shared scope outside the subtree still resolve.

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

## Python

`Stage.clone` blocks (and raises `OvstageError` on failure); `Stage.clone_async` returns an
`Operation` you `.wait()` later. Clone to N targets in one call, then advance the write floor so
the clones are readable:

> **Source:** `examples/python/minimal/main.py` snippet `clone-subtree-multienv`

The public Python test asserts the same round-trip against the produced wheel — clone,
seal, then read back the copied value on each target:

> **Source:** `tests/python/test_clone.py` snippet `clone-and-verify`

## Key Types / Functions

| Python | C |
|--------|---|
| `Stage.clone(source, targets, ordinal)` (blocking) | `ovstage_clone(instance, source, targets, count, ordinal)` |
| `Stage.clone_async(source, targets, ordinal)` → `Operation` | (always async in C; await with `ovstage_wait_op`) |

## Troubleshooting

- **`OVSTAGE_ERROR_NOT_FOUND` (missing source)** — the source path must already exist on the
  stage. Populate or write it first.
- **`OVSTAGE_ERROR_PRIM_NOT_FOUND` (target exists)** — a target path already exists. Targets are
  create-only, and a batch with any existing target clones nothing. Use fresh paths.
- **`OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`** — the clone ordinal is at/below the seal of an
  attribute it reproduces. Clone at an ordinal above the write floor, then advance the floor
  afterward.
- **Enqueue succeeded but the clones aren't there** — enqueue success means *accepted*, not
  *executed*. Await the op (C `ovstage_wait_op`; Python `.wait()`), and advance the write floor
  to/above the clone ordinal to read the clones.
- **A clone's relationship points at the original, not the clone** — relationships are copied
  verbatim, not retargeted; targets inside the cloned subtree still point at the source's copies
  (matches `ovrtx_clone_usd`).

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `application-flow` — where clone fits in the create → write → seal → read lifecycle.
- `cpu-ahead-gpu-async` — the async submit/observe model and ordinal/write-floor semantics clone shares with writes.
- `error-handling` — status checks and per-op error reporting (the codes above).
- `path-dictionary` / `string-handling` — building the source/target path strings (`ovx_string_t`).
- Keep related skills, docs, and snippets synchronized when changing the workflow.
