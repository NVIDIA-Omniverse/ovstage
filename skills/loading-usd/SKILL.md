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
name: loading-usd
description: >
  Populate an ovstage instance from USD via the population API: open a USD file or
  inline USDA into the stage, choose data domains, drive the async populate op, and
  read populated prims back by usd-path. Use when the user asks to load/ingest USD
  into ovstage, populate from a .usd/.usda file or string, add/remove USD references,
  or propagate live USD edits.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - population
  - usd
tools:
  - Read
  - Grep
---

# Loading USD (population)

## When to Use

Use this skill when the program ingests **existing USD** into an ovstage instead of
writing attribute columns directly. That includes: loading a `.usd`/`.usda`/`.usdc`
file or an inline USDA string into the stage, choosing which data **domains** to
populate (rendering vs. physics), adding/removing USD references, propagating live
USD edits per tick, or confirming what landed by querying populated prims.

For the direct write → seal → read data-plane lifecycle (no USD), use
`application-flow` and `path-dictionary` instead.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- USD source: a file/URL path, or an inline USDA string.
- Data domains to populate: RENDERING, PHYSICS, or ALL (bitmask). `0`
  (`DOMAIN_NONE`) authors stage units only.
- The caller-owned `ordinal` for the tick (population never opens/commits its own).
- Whether this is a one-shot open or a running loop with live edits
  (`apply_usd_changes`) / time playback (`apply_usd_time`).
- The shipped `ovstage_population.h` header and the referenced test snippets are the
  authoritative contract.

## Prerequisites

- A created ovstage instance (`ovstage_create_instance` / `ovstage.Stage`). Per-stage
  population state is created lazily on first populate call and released with the
  instance.
- Understand the async **submit/observe** model (`cpu-ahead-gpu-async`): population
  enqueues return an op id; nothing is materialized until you wait on it.
- **Ordinal ownership:** the caller (frame coordinator) owns the ordinal and the
  write floor; population is invoked against a caller-provided ordinal.

## Instructions

1. **Open the USD into the stage.** Call `ovstage_population_open_usd_from_string`
   (inline USDA) or `_open_usd_from_file` (path/URL), passing the tick `ordinal`, a
   `time` in seconds, and a `domains` bitmask. This both loads the USD and populates
   the ovstage in one op, replacing any previously loaded content.
2. **Await the op.** Population is asynchronous — wait via
   `ovstage_population_wait_op` (C) or the blocking `population.open_usd_from_string`
   / `Operation.wait()` (Python).
3. **Read back / consume.** Populated prims are queryable immediately (queries
   resolve against the latest committed state). Confirm with a `usd-path` filter
   query, or hand the stage to a consumer (e.g. `ovrtx_attach_ovstage`).
4. **Live edits (optional).** For a running loop: `add_usd_reference_*` /
   `remove_usd_reference` / `reset_usd` edit the USD source only — follow with
   `apply_usd_changes(ordinal)` to propagate structural edits into the ovstage;
   use `apply_usd_time(ordinal, time)` once per tick for time-sampled playback.
5. **Diagnose failures** via `ovstage_population_get_last_error` /
   `_get_last_op_error` (C) or the `OvstageError` raised by the blocking Python
   wrappers.

## Output Format

- For explanations, return the populate → wait → query/consume sequence with the
  chosen domains and where live edits fit.
- For code changes, summarize the population calls touched, snippets affected, and
  the validation run.

## Overview

ovstage itself has **no USD dependency**; the population API is the bridge that reads
USD (files or inline USDA) and mirrors it into an ovstage instance so consumers like
ovrtx can render it. The initial `open_usd_*` is one-shot (it replaces prior USD
content); subsequent live edits are picked up by `apply_usd_changes` (structural) and
`apply_usd_time` (time-sampled). Regardless of `domains`, stage units
(metersPerUnit, kilogramsPerUnit, upAxis) are authored onto the reserved
`/__ovstage_population_stage_info__` prim.

### Domains

`ovstage_population_domain_t` is a bitmask — OR values together:

- `RENDERING` — meshes, lights, materials, and cameras.
- `PHYSICS` — colliders, rigid bodies, joints, articulations, physics schema attrs.
- `ALL` — both. `NONE` (0) authors stage units only.

## Python

The verified test populates from an inline USDA string and confirms the prim landed
via a `usd-path` filter query:

> **Source:** `tests/python/test_population.py` snippet `populate-and-query`

Live edits are asserted too — add a USD reference then propagate it, reset the USD
source, and confirm a missing file fails the populate op:

> **Source:** `tests/python/test_population.py` snippets `usd-reference`, `reset-usd`, `open-missing-file`

`population.open_usd_from_string(stage, usda, ordinal=, time_code=, domains=)` blocks;
`open_usd_from_string_async` returns an `Operation` for the CPU-ahead pattern
(`cpu-ahead-gpu-async`). `open_usd` / `open_usd_from_file` load from a path/URL. Live
edits: `add_usd_reference[_from_string]`, `remove_usd`, `reset_usd`,
`apply_usd_changes`, `update_from_usd_time` (each has an `*_async` variant).

## C

The C sibling drives the async op explicitly and reads back with a `usd-path` filter
query:

> **Source:** `tests/c/test_population.cpp` snippet `populate-and-query-c`

Live edits (add/apply/remove a reference, reset) and the missing-file failure are
asserted in the C sibling too:

> **Source:** `tests/c/test_population.cpp` snippets `usd-reference-c`, `reset-usd-c`, `open-missing-file-c`

`ovstage_population_open_usd_from_string` / `_open_usd_from_file` return an enqueue
result (`status` + `op_index`); await with `ovstage_population_wait_op`. Reference
edits (`ovstage_population_add_usd_reference_from_*`, `_remove_usd_reference`,
`_reset_usd`) touch USD source only — follow with `ovstage_population_apply_usd_changes`.

## Key Functions

| Purpose | C | Python |
|---------|---|--------|
| Open inline USDA | `ovstage_population_open_usd_from_string` | `population.open_usd_from_string` |
| Open file/URL | `ovstage_population_open_usd_from_file` | `population.open_usd` / `open_usd_from_file` |
| Await op | `ovstage_population_wait_op` | `Operation.wait()` (blocking wrappers auto-wait) |
| Add reference | `ovstage_population_add_usd_reference_from_{file,string}` | `population.add_usd_reference[_from_string]` |
| Remove reference | `ovstage_population_remove_usd_reference` | `population.remove_usd` |
| Reset USD source | `ovstage_population_reset_usd` | `population.reset_usd` |
| Propagate structural edits | `ovstage_population_apply_usd_changes` | `population.apply_usd_changes` |
| Time-sampled tick | `ovstage_population_apply_usd_time` | `population.update_from_usd_time` |

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets are the source of truth; this skill composes them and
  describes the surrounding population API rather than introducing new code.
- **One-shot open.** `open_usd_*` replaces previously loaded USD content; additive
  changes go through `add_usd_reference_*` + `apply_usd_changes`.
- **Surviving-anchor removal (current-build implementation gap).** Removing a reference from an existing prim or a
  root sublayer may leave stale descendants; removal works when the resync root disappears.
- **Ordinal is caller-owned.** Population never opens/commits an ordinal; pass the
  tick ordinal in and advance the write floor yourself.
- **Latest-snapshot payloads.** Do not design a flow around historical payloads;
  bounded older change membership is available only at or above the reported
  retention frontier.
- **⚠️ Draft — API in flux.** Treat exact symbols/ordering as provisional against the
  shipped headers.

## Troubleshooting

- **Populate enqueue rejected** — a `NULL` stage or (for `add_usd_reference` /
  `apply_usd_changes` / `reset_usd`) no population state yet: call an `open_usd_*`
  first. The rejection is in the enqueue `status`.
- **Op accepted but nothing materialized** — enqueue success ≠ completion. Wait on
  the op (`ovstage_population_wait_op` / `Operation.wait()`); check
  `ovstage_population_get_last_op_error` for the failing op.
- **Query returns nothing after populate** — verify the `usd-path` value matches the
  prim exactly (`FILTER_OP_IN`) or use a prefix; confirm the prim's domain was
  populated (a physics-only prim won't appear under `RENDERING`).
- **USDA fails to parse** — surfaced by the wait, with detail in
  `ovstage_population_get_last_error`; validate the USDA independently.
- **Reference/added subtree not visible** — `add_usd_reference_*` edits USD source
  only; call `apply_usd_changes(ordinal)` afterwards to propagate it into the ovstage.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `application-flow` — the direct write/read lifecycle (no USD).
- `path-dictionary` — tokens, path lists, and queries used to read populated prims back.
- `cpu-ahead-gpu-async` — the async submit/observe model shared by the `*_async` population calls.
- `error-handling` — status checks and per-op error reporting.
- The shipped `include/ovstage/ovstage_population.h` header is the authoritative contract.
