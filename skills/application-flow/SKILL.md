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
name: application-flow
description: >
  End-to-end ovstage application lifecycle: create an instance, identify prims/attributes
  via the path dictionary, write attribute columns at an ordinal, advance the write floor to
  seal them, query/read, and release. Use when user asks how to structure an ovstage program,
  what the main steps are, or how the pieces fit together.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - lifecycle
  - workflow
tools:
  - Read
  - Grep
---

# Application Flow

## When to Use

Use this skill when the user asks how to structure an ovstage program end-to-end, what the
main steps are, how the pieces (instance, path dictionary, query, write/ordinal/write-floor,
read, release) fit together, or which focused skill owns each step.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- Lifecycle stage in question: instance creation, prim/attribute identity, write, seal
  (advance write floor), query/read, or release.
- Whether the program ingests existing **USD** (the population API, `ovstage_population.h`)
  or **writes attributes directly** (the path shown here).
- Whether it's a one-shot write→read or a running producer/consumer loop (CPU running ahead
  of execution — see `cpu-ahead-gpu-async`).
- The shipped headers and the referenced example snippets are the authoritative contract.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced example/snippets.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the async **submit/observe** model (`cpu-ahead-gpu-async`): state-mutating and
  data-producing calls are enqueues; confirm completion with `wait_op` / `fetch_*`
  and check the result before relying on it.
- Remember this build is **latest-snapshot only for payloads**. Exact change membership is
  retained across a bounded set of completed write-floor epochs; query
  `ovstage_get_oldest_preserved_ordinal` before consuming an older range.

## Instructions

1. **Configure process state when needed:** for a static loader with a non-default
   binary root, call `ovstage_initialize` with
   `ovstage_config_entry_binary_package_root_path()` before any other ovstage call.
   Use the package `bin/` directory and check initialization succeeds (see
   `project-setup-c`, Instance configuration). Then **create the instance**
   (`ovstage_create_instance`) and obtain its instance-owned path dictionary (`ovstage_get_path_dictionary`).
2. **Establish identity:** intern the attribute name → `ovx_token_t`, build a prim-path list,
   and open a query over it (`ovstage_query_from_path_list`).
3. **Write** attribute values at an `ordinal` (`ovstage_write_attribute`); observe the
   enqueue with `ovstage_wait_op`.
4. **Seal:** advance the write floor to that ordinal (`ovstage_advance_write_floor`) so data
   at/below it is immutable and readable.
5. **Read:** `ovstage_read_attributes` over an ordinal range → `ovstage_fetch_read_next` →
   consume `group.data` tensors.
6. **Release in reverse:** the read group, the read handle, the path-list reference, then
   destroy the instance. Balance any successful explicit `ovstage_initialize` with
   `ovstage_shutdown` after instance teardown.
7. **Layer in the focused skills** as concerns arise: tensor data (`dlpack-tensor-exchange`),
   submit/observe & concurrency (`cpu-ahead-gpu-async`), error reporting (`error-handling`),
   string/token identity (`string-handling`, `path-dictionary`).
   Once a committed snapshot must be persisted as USD, route to `exporting-to-usd`;
   it owns selection, destination lifetime, save ownership, and export-specific waits.

## Output Format

- For explanations, return an ordered lifecycle with the relevant follow-on skills.
- For code changes, summarize the lifecycle files touched, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; this skill composes the minimal
  example's snippets in lifecycle order rather than introducing new code.
- **Ordinal surface, latest-only payloads:** ordinals, write floors, ordinal ranges, and
  `ovstage_get_oldest_preserved_ordinal` describe ordering, sealing, and retained
  change/deletion membership — that surface is part of the API contract. **In the current
  build**, payload reads are latest-snapshot only and do not provide historical payload
  versions. At or above the reported inclusive retention frontier, a range selects exact
  changed/deleted membership; a snapshot selects all queried paths. Reads require selected
  changes through the requested end to be sealed. They return `OVSTAGE_ERROR_OUT_OF_RANGE`
  if history records a later change for a selected `(attribute, path)`, because older values
  are not retained. End `UINT64_MAX` requests current values; a selected unsealed change still returns `WRITE_FLOOR_VIOLATION`.
  Below the frontier, membership may be coalesced or discarded, so a successful
  snapshot read does not guarantee the requested earlier state.
  Do not treat an ordinal range as a historical-payload event log.
- **USD population not covered here.** Ingesting existing USD uses the population API
  (`ovstage_population.h`); this skill documents the direct write/read lifecycle.
- **⚠️ Draft — API in flux.** Treat exact symbols/ordering as provisional against the headers.

## Overview

ovstage is a **data stage, not a renderer**. A program creates an instance, identifies
prims/attributes through the (instance-owned) path dictionary, writes attribute columns at
**ordinals**, **seals** them by advancing the write floor, and reads them back — all on the
asynchronous submit/observe model. (ovrtx's renderer lifecycle — Camera / RenderProduct /
`step()` / render output — does not apply.)

These cross-cutting rules shape every stage:

- **Async enqueue / observe.** Enqueue is synchronous and returns an `op_index`
  before completion is confirmed; the CPU can run ahead. Same-ordinal ops execute
  in submission order; different-ordinal ops are independent. Block or poll with
  `ovstage_wait_op`. (→ `cpu-ahead-gpu-async`)
- **Ordinals + write floor.** Writes carry an `ordinal`. Advancing the **write floor** seals
  everything at/below it; reads target sealed data at/below the floor, while queries resolve
  against the latest committed state.
- **Strict admission: one committed modification per cell and ordinal.** With
  `STRICTLY_INCREASING`, write, map publication, clone and delete must clear
  applicable floors and retained changes to each affected cell and prim lifecycle.
  Disjoint cells may share an ordinal. For optional policies, see
  [Ordinal Admission Policies](../../docs/scene/writing_attributes.rst). Always
  observe completion; WRITE/MAP enqueue can succeed before value admission fails.

## Lifecycle

```
0. Configure / initialize if needed → project-setup-c (static loader: before any ovstage call)
1. Create instance                 → project-setup-c
2. Get path dictionary             → path-dictionary
3. Identify prims + attributes     → path-dictionary, string-handling
   (intern token, build path list, open query)
4. Write attribute(s) at an ordinal → dlpack-tensor-exchange (data) + cpu-ahead-gpu-async (submit/observe)
5. Advance the write floor (seal)   → cpu-ahead-gpu-async
6. Read / fetch                     → dlpack-tensor-exchange
7. Release + destroy instance       → error-handling (status checks throughout)
8. Shutdown explicit initialization → project-setup-c
```

Optional: ingest existing USD via the population API (`ovstage_population.h`) instead of, or
before, direct writes.

## C — end-to-end

For a static loader with a non-default binary root, begin with this tested
initialization/teardown recipe; use the extracted package's `bin/` directory:

> **Source:** `tests/c/test_static_config.cpp` snippet `initialize-static-loader-c`

Insert the write/read lifecycle while the instance is alive, then release its
resources, destroy it, and balance the explicit initialization with
`ovstage_shutdown`. Shared-loader consumers using defaults can create the
instance directly.

The minimal example walks the data lifecycle. After `ovstage_create_instance` (see
`project-setup-c`), compose these snippets in order — get the path dictionary + intern a
token, build a path list + open a query, form the attribute argument, then write → seal →
read:

> **Source:** `examples/c/minimal/main.cpp` snippet `intern-and-resolve`
>
> Followed by: `examples/c/minimal/main.cpp` snippet `path-list-query`
>
> Followed by: `examples/c/minimal/main.cpp` snippet `string-or-token-arg`
>
> Followed by: `examples/c/minimal/main.cpp` snippet `minimal-write-read`

Every state-mutating / data-producing call is an async enqueue; drive each to completion and
check synchronous calls along the way:

> **Source:** `examples/c/minimal/main.cpp` snippet `enqueue-wait-error`
>
> And for synchronous calls: `examples/c/minimal/main.cpp` snippet `check-sync-error`

The public C test asserts this whole round-trip (write → seal → read → verify the
values) against the produced package, not just prints it:

> **Source:** `tests/c/test_minimal.cpp` snippet `write-read-roundtrip-c`

The essential ordering is: `ovstage_create_instance` → `ovstage_get_path_dictionary` →
intern token / `path_dictionary_create_path_list_from_strings` → `ovstage_query_from_path_list`
→ `ovstage_write_attribute` *(ordinal)* → `ovstage_advance_write_floor` *(seal)* →
`ovstage_read_attributes` → `ovstage_fetch_read_next` → release (`ovstage_release_group` →
`ovstage_release_read` → `path_dictionary_release_path_list_reference`) →
`ovstage_destroy_instance`.

## Key Stages / Functions

| Stage | Function(s) | Detail skill |
|-------|-------------|--------------|
| Configure / initialize / shutdown | `ovstage_initialize`, `ovstage_config_entry_binary_package_root_path`, `ovstage_shutdown` | `project-setup-c` |
| Create / destroy instance | `ovstage_create_instance` / `ovstage_destroy_instance` | `project-setup-c` |
| Path dictionary (identity) | `ovstage_get_path_dictionary`, `path_dictionary_create_tokens_from_strings`, `path_dictionary_create_path_list_from_strings` | `path-dictionary`, `string-handling` |
| Query target prims | `ovstage_query_from_path_list` | `path-dictionary` |
| Write (ordinal-keyed) | `ovstage_write_attribute` (+ `ovstage_write_data_t`) | `dlpack-tensor-exchange`, `cpu-ahead-gpu-async` |
| Seal | `ovstage_advance_write_floor` | `cpu-ahead-gpu-async` |
| Read | `ovstage_read_attributes` → `ovstage_fetch_read_next` → `ovstage_release_group` | `dlpack-tensor-exchange` |
| Observe / errors | `ovstage_wait_op`, `ovstage_get_error_string`, `ovstage_get_last_op_error` | `cpu-ahead-gpu-async`, `error-handling` |

## Troubleshooting

- **Read fails with `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` after a write** — the selected state is
  not sealed; call `ovstage_advance_write_floor` to/above its ordinal. Also remember that enqueue
  success means *accepted*, not *executed* (wait/fetch first). See `cpu-ahead-gpu-async`.
- **Read fails with `OVSTAGE_ERROR_OUT_OF_RANGE`** — a selected `(attribute, path)` has a
  recorded change after the requested end, and older values are not retained. If newer values
  are acceptable, move the end forward or use `UINT64_MAX` (Python: `(1 << 64) - 1`) for current
  values; a selected unsealed change still returns `WRITE_FLOOR_VIOLATION`. Advancing the floor alone does not restore older values.
- **Write rejected (`OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`)** — the ordinal conflicts with an
  applicable floor or the configured admission policy for an affected cell or prim
  lifecycle. The strict policy requires a newer ordinal for a second modification
  of the same cell. Disjoint cells may share an ordinal.
- **Expecting historical payloads from an ordinal range** — unsupported in this
  latest-snapshot build. Ranges at or above the reported retention frontier provide exact
  changed-prim membership; representable ranges carry current payload/delete state, while a
  retained same-key successor after the range end produces `OUT_OF_RANGE`.
- **Leaks / teardown crashes** — release in reverse: the read group, the read handle, the
  path-list reference, *then* destroy the instance (the path dictionary is instance-owned —
  never free it yourself).
- **Can't stringify an error before the instance exists** — `ovstage_get_error_string` is
  vtable-dispatched and takes the instance; print the numeric code in the create window. Per-op
  errors come from `ovstage_wait_op` + `ovstage_get_last_op_error` (see `error-handling`).

## Python

The Python bindings expose the same lifecycle — `Stage` + `PathDictionary` context
managers, async ops returning handles whose `.wait()` returns the handle (and raises on
failure), write → advance write floor → read. A mutation's `Operation.ok` checks
only enqueue acceptance; retain the operation and call `wait()` to observe its
execution. Use query/read/map context managers for resource lifetimes and explicit
wait/fetch/commit calls for results; cleanup and finalizers do not replace error
checks. See
`project-setup-python` for the package surface. The verified example walks it end to end:

> **Source:** `examples/python/minimal/main.py` snippet `setup`
>
> Followed by: `examples/python/minimal/main.py` snippet `intern-and-resolve`
>
> Followed by: `examples/python/minimal/main.py` snippet `path-list-query`
>
> Followed by: `examples/python/minimal/main.py` snippet `minimal-write-read`

The public Python test asserts the same round-trip against the produced package:

> **Source:** `tests/python/test_minimal.py` snippet `write-read-roundtrip`

## Per-Tick Change Feeds

A consumer that polls once per tick keeps one whole-stage query (`HAS(usd-path)`) alive across
ticks and reads explicit ranges `[tick, tick]` against it; a range that selects no change is a
successful zero-group read, not an error. A filter query keeps the membership it resolved when it
ran, so open a fresh query when newly created prims must enter the feed. Seal with `SCOPE_INCLUDE`
only when the tick wrote nothing structural (no new prims, prim types, applied schemas, or
deletes): an INCLUDE seal leaves every unlisted column's floor where its last covering seal put it
(here, the previous `SCOPE_ALL` seal). A selected structural change at or before the requested
end and above its floor fails with `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` until a covering seal
lands. An earlier snapshot that selects the changed path fails with `OVSTAGE_ERROR_OUT_OF_RANGE`
regardless of sealing, because the earlier value is unavailable. Derived transforms such as
`omni:fabric:worldMatrix` are read-only output: poll them with a latest read after the seal and
follow `docs/scene/transforms.rst` for the freshness contract rather than treating the range
feed as their change log.

The public tests assert these behaviors against the produced package:

> **Source:** `tests/c/test_change_feed.cpp` snippets `empty-change-range-c`, `include-seal-change-range-c`, `frozen-vs-fresh-query-c`

> **Source:** `tests/python/test_change_feed.py` snippets `empty-change-range`, `include-seal-change-range`, `frozen-vs-fresh-query`

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `project-setup-c` — instance creation and build/link setup.
- `path-dictionary` / `string-handling` — prim/attribute identity (tokens, path lists, queries).
- `cpu-ahead-gpu-async` — the async submit/observe model and ordinal/write-floor semantics.
- `dlpack-tensor-exchange` — attribute data as DLPack tensors (write/read/map).
- `error-handling` — status checks and per-op error reporting throughout the lifecycle.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
