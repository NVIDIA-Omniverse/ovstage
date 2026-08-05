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
name: error-handling
description: >
  Error checking patterns for the ovstage C API. Use when user asks about error
  handling, checking return codes, debugging ovstage failures, or troubleshooting a
  failed enqueue, wait, or fetch.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - errors
  - debugging
tools:
  - Read
  - Grep
---

# Error Handling

## When to Use

Use this skill when the user asks about error handling, checking return codes, debugging
ovstage failures, or troubleshooting an operation that failed at enqueue, wait, or fetch.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- Failure point: a synchronous call returning `ovstage_api_status_t`, an async enqueue
  returning `ovstage_enqueue_result_t`, an `ovstage_wait_op`, or a `fetch_*` call.
- Error signal available: the returned `ovstage_api_status_t` code, an enqueue `status`, an
  `op_id`, or the `ovstage_op_wait_result_t` from a wait.
- Operation being debugged and whether the caller needs recovery, logging, or
  propagation guidance.
- Repository source snippets referenced below. Treat these snippets as the API source of truth.

## Prerequisites

- Use an ovstage checkout that contains the referenced example/snippets.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Distinguish enqueue success from execution success: an enqueue returning `OVSTAGE_OK`
  means the op was *accepted*, not that it *completed*. Completion/errors surface at
  `ovstage_wait_op` (or the matching `fetch_*`).

## Instructions

1. Identify the failure point: synchronous `ovstage_api_status_t`, async enqueue, wait, or fetch.
2. For synchronous calls, compare the returned `ovstage_api_status_t` against `OVSTAGE_OK`;
   on failure call `ovstage_get_error_string(instance, code)` for a human-readable message.
3. For async enqueues, check `ovstage_enqueue_result_t.status == OVSTAGE_OK` before using
   `op_index`. A non-OK status leaves `op_index == OVSTAGE_INVALID_OP_ID`.
4. After `ovstage_wait_op`, inspect `ovstage_op_wait_result_t.error_op_ids` /
   `error_op_id_count`; for each failed id call `ovstage_get_last_op_error(instance, op_id)`
   immediately (the strings are thread-local and invalidated by the next wait on the
   same thread).
5. Release op-tracking state with `ovstage_release_op(instance, op_id)` once the op is
   known complete.
6. When changing code, run the narrow ovstage unit test that exercises the failing
   operation whenever practical.

## Output Format

- For explanations, cite the relevant API names, source snippets, and caveats.
- For code changes, summarize the files changed, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before documenting new API usage.
- **Logging is separate from error handling.** Error reporting is via the return codes
  and error-string accessors below. For diagnostic *logging* — a process-global callback
  with a severity threshold and RUST_LOG-style channel filter (`ovstage_set_log_callback`
  / `ovstage_flush_log`) — see the `logging` skill; it is a distinct surface, not an
  error-reporting mechanism.
- **Diagnostics.** `ovstage_get_error_string(instance, code)` (returns a static
  `const char*`, never NULL) and `ovstage_get_last_op_error(instance, op_id)` (returns an
  `ovx_string_t`, `{NULL, 0}` if the op id is unknown / did not fail) are vtable-dispatched
  (take the `instance`). `ovstage_get_last_error(void)` is a free function (no instance)
  returning the most recent thread-local synchronous error as an `ovx_string_t` — readable
  even when `ovstage_create_instance` itself failed.
- **Python:** covered in the Python section — failures surface as `OvstageError` /
  `OvxError` exceptions (both `RuntimeError`); async errors raise from `.wait()`. Inline
  Python pending a shipping Python example to snippet-source.
- **Snippets** are sourced from the shipping example `examples/c/minimal/main.cpp`.

## Overview

Every synchronous ovstage call returns an `ovstage_api_status_t` (a typed enum of status codes).
`OVSTAGE_OK` (0) is success; any non-zero value is an error. Asynchronous enqueue calls
return an `ovstage_enqueue_result_t { status; op_index; }`; completion and per-op errors are
observed later via `ovstage_wait_op`. Convert any code to text with
`ovstage_get_error_string(instance, code)` (returns a static string, never NULL). The
most recent thread-local error is also available as an `ovx_string_t` via
`ovstage_get_last_error()`.

## Error codes

| Code | Meaning |
|------|---------|
| `OVSTAGE_OK` (0) | success |
| `OVSTAGE_ERROR_INVALID_ARGUMENT` (1) | NULL pointer or invalid parameter |
| `OVSTAGE_ERROR_INVALID_HANDLE` (2) | stale/invalid handle |
| `OVSTAGE_ERROR_NOT_FOUND` (3) | token/path/list not found |
| `OVSTAGE_ERROR_PRIM_NOT_FOUND` (4) | INSERT mode and prim already exists |
| `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` (5) | write at/below the floor, or latest state / a selected in-range change above it |
| `OVSTAGE_ERROR_NOT_SUPPORTED` (6) | unsupported operation |
| `OVSTAGE_ERROR_QUEUE_FULL` (7) | backpressure: submit queue full |
| `OVSTAGE_ERROR_END_OF_ITERATION` (8) | no more groups to iterate |
| `OVSTAGE_ERROR_OUT_OF_MEMORY` (9) | allocation failure |
| `OVSTAGE_ERROR_LAYOUT_CHANGED` (10) | layout changed during map |
| `OVSTAGE_ERROR_TIMEOUT` (11) | fetch/wait did not complete within timeout |
| `OVSTAGE_ERROR_OP_FAILED` (12) | an enqueued op failed; see `ovstage_get_last_op_error` |
| `OVSTAGE_ERROR_OUT_OF_RANGE` (13) | requested ordinal range cannot be materialized from retained payloads |
| `OVSTAGE_ERROR_INTERNAL` (99) | internal error |

## C — synchronous calls

Compare the returned code against `OVSTAGE_OK`; stringify failures with
`ovstage_get_error_string`:

> **Source:** `examples/c/minimal/main.cpp` snippet `check-sync-error`

`ovstage_get_error_string` maps any status code to a human-readable string (never NULL,
distinct per code); `ovstage_get_version` reports the runtime version. The public
support-API test asserts both:

> **Source:** `tests/c/test_support_api.cpp` snippet `version-and-error-c`

A concrete synchronous rejection: an `INSERT` write whose target prim already exists is
refused by the synchronous preflight with `OVSTAGE_ERROR_PRIM_NOT_FOUND` (before anything is
written). The C enqueue returns that status; the Python `Operation.wait()` raises it. The
public write-modes tests assert this admission rule against UPSERT:

> **Source:** `tests/c/test_write_modes.cpp` snippet `upsert-vs-insert-c`

> **Source:** `tests/python/test_write_modes.py` snippet `upsert-vs-insert`

## C — async enqueue + wait

Check the enqueue `status` before using `op_index`, wait on the op, then inspect per-op
errors from the wait result:

> **Source:** `examples/c/minimal/main.cpp` snippet `enqueue-wait-error`

`OVSTAGE_ERROR_TIMEOUT` from `ovstage_wait_op` is not a failure of the op — it means the
op (or a dependency) has not completed yet; `ovstage_op_wait_result_t.lowest_pending_op_id`
reports the lowest still-pending id in the waited op's dependency chain.

A concrete asserted failure: an op that runs and fails surfaces through `error_op_ids`
with the wait itself still returning `OVSTAGE_OK`, and the reason lives in the per-op
error string. The public test drives a clone onto an already-existing target and checks
both the failure and the "already exists" reason:

> **Source:** `tests/c/test_error_handling.cpp` snippet `clone-target-exists-error-c`

## Python

The Python bindings turn return-code failures into **exceptions** (both subclass
`RuntimeError`, carrying the numeric `code` + message):

- **`ovstage.OvstageError`** — raised by `Stage` operations (wraps `ovstage_get_error_string`
  / `ovstage_get_last_op_error`).
- **`ovstage.OvxError`** — raised by `PathDictionary` operations.

Synchronous calls raise on failure. For **async** ops, the enqueue returns a handle object
(`Query` / `Read` / `Map`) whose **`.wait(timeout)` raises `OvstageError`** if the op (or a
dependency) failed — the equivalent of inspecting `error_op_ids` in C.

> **Source:** `examples/python/minimal/main.py` snippet `error-handling`

An op that ran and failed raises `OvstageError` from `.wait()` with the op-level code
`ErrorCode.OP_FAILED` and the reason in `.message`. The public test asserts this on a
clone onto an already-existing target:

> **Source:** `tests/python/test_error_handling.py` snippet `clone-target-exists-error`

A timeout surfaces as the `OVSTAGE_ERROR_TIMEOUT` code (`timeout=0` polls,
`OVSTAGE_TIMEOUT_INFINITE` blocks, other finite values wait up to the timeout).

## Key Types / Functions

| Symbol | Role |
|--------|------|
| `ovstage_api_status_t` | typed enum return/status code; `OVSTAGE_OK` = success |
| `ovstage_get_error_string(instance, code)` | code → static `const char*` (never NULL) |
| `ovstage_get_last_error()` | free function; most recent thread-local error → `ovx_string_t` |
| `ovstage_enqueue_result_t { status; op_index; }` | result of an async enqueue |
| `ovstage_wait_op(instance, op_id, timeout, &wait_result)` | wait for an op + its deps |
| `ovstage_op_wait_result_t { error_op_ids; error_op_id_count; lowest_pending_op_id; }` | per-wait error/progress data |
| `ovstage_get_last_op_error(instance, op_id)` | failed-op id → `ovx_string_t` (`{NULL, 0}` if unknown), transient (thread-local) |
| `ovstage_release_op(instance, op_id)` | release op-tracking state after completion |

## Troubleshooting

- An enqueue returning `OVSTAGE_OK` only means *accepted*. Always wait (or fetch) and
  check `error_op_ids` before trusting the op's effect.
- `ovstage_get_last_op_error(instance, op_id)` and the `error_op_ids` array are transient
  thread-local memory, invalidated by the **next** `ovstage_wait_op` on the same thread.
  Copy the string if you need to keep it. (See the `string-handling` skill for copying
  `ovx_string_t`.)
- `ovstage_wait_op` waits for all ops up to and including `op_id` within its dependency
  chain, not just that single op.
- Treat `OVSTAGE_ERROR_TIMEOUT` as "not ready yet," distinct from `OVSTAGE_ERROR_OP_FAILED`
  ("the op ran and failed").
- `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` means a write or delete targeted an ordinal at or
  below the effective write floor, a latest read targets current recorded state above it, or
  an explicit range selects an in-range change above it. The effective floor starts at 0, so
  advance it to cover positive-ordinal state before reading. `advance_write_floor` itself never
  raises this: backwards advances clamp via `max(...)` rather than rejecting.
- `OVSTAGE_ERROR_OUT_OF_RANGE` on an explicit range means a selected `(attribute, path)` also
  has a retained change after the range end, so the payload for that fixed range is no longer
  available. Widen/rebase the range or request current state. Sealing the later change alone does
  not resolve this error.
- Call `ovstage_release_op` once an op is known complete; afterwards the `op_id` must not
  be reused with `ovstage_wait_op` or `ovstage_get_last_op_error`.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `string-handling` skill — `ovx_string_t` handling (including `ovstage_get_last_op_error` /
  `ovstage_get_last_error`; `ovstage_get_error_string` remains static `const char*`).
- `cpu-ahead-gpu-async` skill — op-id / wait lifecycle and async completion semantics.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
