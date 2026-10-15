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

For USD export operations, wait with the matching export-specific wait to obtain
its report. That wait does not consume unrelated generic population failures;
observe and drain those separately with the generic population wait. Route
destination ownership and save/rollback questions to `exporting-to-usd`.

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
| `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` (5) | mutation blocked by a floor or the configured admission policy, or read selecting an unsealed change at/before its end |
| `OVSTAGE_ERROR_NOT_SUPPORTED` (6) | unsupported operation |
| `OVSTAGE_ERROR_QUEUE_FULL` (7) | backpressure: submit queue full |
| `OVSTAGE_ERROR_END_OF_ITERATION` (8) | no more groups to iterate |
| `OVSTAGE_ERROR_OUT_OF_MEMORY` (9) | allocation failure |
| `OVSTAGE_ERROR_LAYOUT_CHANGED` (10) | layout changed during map |
| `OVSTAGE_ERROR_TIMEOUT` (11) | fetch/wait did not complete within timeout |
| `OVSTAGE_ERROR_OP_FAILED` (12) | an enqueued op failed; see `ovstage_get_last_op_error` |
| `OVSTAGE_ERROR_OUT_OF_RANGE` (13) | older values requested by the read are no longer available |
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
the failure, the "already exists" reason, and that the colliding target path is named:

> **Source:** `tests/c/test_error_handling.cpp` snippet `clone-target-exists-error-c`

## Python

The Python bindings turn return-code failures into **exceptions** (both subclass
`RuntimeError`, carrying the numeric `code` + message):

- **`ovstage.OvstageError`** — raised by `Stage` operations (wraps `ovstage_get_error_string`
  / `ovstage_get_last_op_error`).
- **`ovstage.OvxError`** — raised by `PathDictionary` operations.

`Operation.ok` reports enqueue acceptance only, even when execution later fails.
Retain mutation operations until `wait()` observes their completion. Use
query/read/map context managers for handle lifetimes and explicitly observe
their wait/fetch/commit results; finalizers are not an error-reporting mechanism.

Synchronous calls raise on failure. For **async** ops, the enqueue returns a handle object
(`Query` / `Read` / `Map`; mutations return an `Operation`, `Stage.clone_async` a
`CloneOperation`) whose **`.wait(timeout)` raises `OvstageError`** if the op (or a
dependency) failed — the equivalent of inspecting `error_op_ids` in C. For a failed clone the
two error views are mutually exclusive in Python: `.wait()` (and `CloneOperation.result()`)
raise `OP_FAILED` plus the message and release the op together with its report, so to observe
the clone's own code (for example `NOT_FOUND` / `PRIM_NOT_FOUND` / `WRITE_FLOOR_VIOLATION`) call
`Stage.fetch_clone_result(op)` *instead*, before any wait. Only the C sequence `ovstage_wait_op`
→ `ovstage_fetch_clone_result` → `ovstage_release_op` yields both. On success `.wait()`
returns the object it was called on (`Operation.wait()` returns the `Operation`), so the
follow-on call chains — `group = read.wait().fetch_next()` — inside the handle's `with`
block; chaining on the temporary leaks the handle. The public test asserts the contract:

> **Source:** `tests/python/test_minimal.py` snippet `wait-chaining`

> **Source:** `examples/python/minimal/main.py` snippet `error-handling`

An op that ran and failed raises `OvstageError` from `.wait()` with the op-level code
`ErrorCode.OP_FAILED` and the reason — including the colliding target path — in
`.message`. The public test asserts this on a clone onto an already-existing target:

> **Source:** `tests/python/test_error_handling.py` snippet `clone-target-exists-error`

A timeout surfaces as the `OVSTAGE_ERROR_TIMEOUT` code (`timeout=0` polls,
`OVSTAGE_TIMEOUT_INFINITE` blocks, other finite values wait up to the timeout). A timed-out
`.wait()` does not consume the op: it stays waitable and its input buffers stay pinned.

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
- `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` means a mutation conflicts with an effective floor
  or the configured admission policy for affected cells or prim lifecycle, or a read selects
  an unsealed change at or before its end. Snapshots select all queried paths; change ranges
  select `(attribute, path)` pairs changed in the requested interval. See
  [Ordinal Admission Policies](../../docs/scene/writing_attributes.rst) when diagnosing
  retained-history conflicts. The effective floor starts at 0. `advance_write_floor` itself never
  raises this: backwards advances clamp via `max(...)` rather than rejecting.
- `OVSTAGE_ERROR_OUT_OF_RANGE` means a selected `(attribute, path)` has a recorded change
  after the requested end, sealed or not, and older values are not retained. For each attribute,
  the read checks for the write-floor violation above first. If newer values are acceptable,
  move the end forward or use `UINT64_MAX` (Python: `(1 << 64) - 1`) to request current values,
  but a selected unsealed change still returns `WRITE_FLOOR_VIOLATION`. Sealing alone does not restore older values.
- Call `ovstage_release_op` once an op is known complete; afterwards the `op_id` must not
  be reused with `ovstage_wait_op` or `ovstage_get_last_op_error`.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `string-handling` skill — `ovx_string_t` handling (including `ovstage_get_last_op_error` /
  `ovstage_get_last_error`; `ovstage_get_error_string` remains static `const char*`).
- `cpu-ahead-gpu-async` skill — op-id / wait lifecycle and async completion semantics.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
