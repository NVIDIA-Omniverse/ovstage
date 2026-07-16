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
  data-producing calls are enqueues; nothing is done until you `wait_op` / `fetch_*`.
- Remember this build is **latest-snapshot only for payloads**. Exact change membership is
  retained across a bounded set of completed write-floor epochs; query
  `ovstage_get_oldest_preserved_ordinal` before consuming an older range.

## Instructions

1. **Create the instance** (`ovstage_create_instance`) and obtain its instance-owned path
   dictionary (`ovstage_get_path_dictionary`).
2. **Establish identity:** intern the attribute name → `ovx_token_t`, build a prim-path list,
   and open a query over it (`ovstage_query_from_path_list`).
3. **Write** attribute values at an `ordinal` (`ovstage_write_attribute`); observe the
   enqueue with `ovstage_wait_op`.
4. **Seal:** advance the write floor to that ordinal (`ovstage_advance_write_floor`) so data
   at/below it is immutable and readable.
5. **Read:** `ovstage_read_attributes` over an ordinal range → `ovstage_fetch_read_next` →
   consume `group.data` tensors.
6. **Release in reverse:** the read group, the read handle, the path-list reference, then
   destroy the instance.
7. **Layer in the focused skills** as concerns arise: tensor data (`dlpack-tensor-exchange`),
   submit/observe & concurrency (`cpu-ahead-gpu-async`), error reporting (`error-handling`),
   string/token identity (`string-handling`, `path-dictionary`).

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
  changed/deleted membership, but each non-delete group carries the latest committed payload
  for the selected key. Below that frontier, membership may be coalesced or discarded. Do not
  treat an ordinal range as a historical-payload event log.
- **USD population not covered here.** Ingesting existing USD uses the population API
  (`ovstage_population.h`); this skill documents the direct write/read lifecycle.
- **⚠️ Draft — API in flux.** Treat exact symbols/ordering as provisional against the headers.

## Overview

ovstage is a **data stage, not a renderer**. A program creates an instance, identifies
prims/attributes through the (instance-owned) path dictionary, writes attribute columns at
**ordinals**, **seals** them by advancing the write floor, and reads them back — all on the
asynchronous submit/observe model. (ovrtx's renderer lifecycle — Camera / RenderProduct /
`step()` / render output — does not apply.)

Two cross-cutting rules shape every stage:

- **Async enqueue / observe.** Enqueue is synchronous and cheap and returns an `op_index`; the
  CPU can run ahead. Same-ordinal ops execute in submission order; different-ordinal ops are
  independent. Block or poll with `ovstage_wait_op`. (→ `cpu-ahead-gpu-async`)
- **Ordinals + write floor.** Writes carry an `ordinal`. Advancing the **write floor** seals
  everything at/below it; reads target sealed data at/below the floor, while queries resolve
  against the latest committed state.

## Lifecycle

```
1. Create instance                 → project-setup-c
2. Get path dictionary             → path-dictionary
3. Identify prims + attributes     → path-dictionary, string-handling
   (intern token, build path list, open query)
4. Write attribute(s) at an ordinal → dlpack-tensor-exchange (data) + cpu-ahead-gpu-async (submit/observe)
5. Advance the write floor (seal)   → cpu-ahead-gpu-async
6. Read / fetch                     → dlpack-tensor-exchange
7. Release + destroy instance       → error-handling (status checks throughout)
```

Optional: ingest existing USD via the population API (`ovstage_population.h`) instead of, or
before, direct writes.

## C — end-to-end

The minimal example walks the whole lifecycle. After `ovstage_create_instance` (see
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
| Create / destroy instance | `ovstage_create_instance` / `ovstage_destroy_instance` | `project-setup-c` |
| Path dictionary (identity) | `ovstage_get_path_dictionary`, `path_dictionary_create_tokens_from_strings`, `path_dictionary_create_path_list_from_strings` | `path-dictionary`, `string-handling` |
| Query target prims | `ovstage_query_from_path_list` | `path-dictionary` |
| Write (ordinal-keyed) | `ovstage_write_attribute` (+ `ovstage_write_data_t`) | `dlpack-tensor-exchange`, `cpu-ahead-gpu-async` |
| Seal | `ovstage_advance_write_floor` | `cpu-ahead-gpu-async` |
| Read | `ovstage_read_attributes` → `ovstage_fetch_read_next` → `ovstage_release_group` | `dlpack-tensor-exchange` |
| Observe / errors | `ovstage_wait_op`, `ovstage_get_error_string`, `ovstage_get_last_op_error` | `cpu-ahead-gpu-async`, `error-handling` |

## Troubleshooting

- **Wrote, but the read sees nothing** — two causes: the ordinal isn't sealed yet (call
  `ovstage_advance_write_floor` to/above it before reading at/below it), and/or enqueue
  success means *accepted*, not *executed* (wait/fetch first). See `cpu-ahead-gpu-async`.
- **Write rejected (`OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`)** — you wrote at an ordinal at or
  below the current write floor. Write above the floor, then advance the floor afterward.
- **Expecting historical payloads from an ordinal range** — unsupported in this
  latest-snapshot build. Ranges at or above the reported retention frontier provide exact
  changed-prim membership, but their groups carry the latest committed payload or tombstone.
- **Leaks / teardown crashes** — release in reverse: the read group, the read handle, the
  path-list reference, *then* destroy the instance (the path dictionary is instance-owned —
  never free it yourself).
- **Can't stringify an error before the instance exists** — `ovstage_get_error_string` is
  vtable-dispatched and takes the instance; print the numeric code in the create window. Per-op
  errors come from `ovstage_wait_op` + `ovstage_get_last_op_error` (see `error-handling`).

## Python

The Python bindings expose the same lifecycle — `Stage` + `PathDictionary` context
managers, async ops returning handles with `.wait()`, write → advance write floor → read. See
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

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `project-setup-c` — instance creation and build/link setup.
- `path-dictionary` / `string-handling` — prim/attribute identity (tokens, path lists, queries).
- `cpu-ahead-gpu-async` — the async submit/observe model and ordinal/write-floor semantics.
- `dlpack-tensor-exchange` — attribute data as DLPack tensors (write/read/map).
- `error-handling` — status checks and per-op error reporting throughout the lifecycle.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
