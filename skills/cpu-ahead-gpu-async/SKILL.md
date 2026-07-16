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
name: cpu-ahead-gpu-async
description: >
  ovstage's asynchronous, ordinal-keyed submit/observe model: enqueue returns an op id
  before queued execution completes, and the CPU can run ahead. Covers the current synchronous
  submission, serialized backend-lane, and overlapping read/write/map limitations. Use when user
  asks about async operations, op ids, waiting/polling, timeouts, overlap failures, non-blocking
  submission, or CPU-ahead execution.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - async
  - concurrency
tools:
  - Read
  - Grep
---

# CPU-Ahead / GPU-Async

## When to Use

Use this skill when the user asks how ovstage runs work asynchronously — enqueue vs.
execution, op ids, waiting vs. polling, timeouts, submitting more work before prior work
finishes (CPU running ahead of the GPU), or how ordinal ordering interacts with
concurrency.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- Target API surface: C/C++ (the patterns here are C; Python wraps the same model — see Python).
- Whether the caller needs to **block** (wait until done), **poll** (non-blocking, do other
  work), or fire-and-keep-submitting.
- The op being driven: a state-mutating enqueue (`write_attribute`, `advance_write_floor`,
  `delete_attributes`, `clone`) or a data-producing enqueue (`query`, `read_attributes`,
  `map_attribute`) that also yields a fetch handle.
- Whether ordering matters: same-ordinal ops serialize; different-ordinal ops are independent
  by contract, but the current implementation does not execute backend tasks in parallel.
- Whether the same attribute and prim paths are covered by a pending write, a fetched read group,
  or an active map whose lifetime has not ended.
- The shipped `include/` headers (notably `ovstage_api/ovstage_api.h`'s execution-model
  notes) are the authoritative contract — treat them as the source of truth.

## Prerequisites

- Use an ovstage checkout with the `include/` headers and the referenced example.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the enqueue/observe split before adding waits: enqueue success only means the
  op was *accepted*, not that it *executed*.

## Instructions

1. Treat every state-mutating / data-producing call as an **async enqueue**: it returns an
   `ovstage_enqueue_result_t` (`status` + `op_index`) before queued execution completes. Do not
   treat submission itself as wait-free: admission, request preparation, and prerequisite-handle
   readiness may still do synchronous work.
2. Check the enqueue `status == OVSTAGE_OK` before using `op_index` (a non-OK enqueue yields
   `OVSTAGE_INVALID_OP_ID`).
3. Observe completion with `ovstage_wait_op(instance, op_id, timeout, &wait_result)`:
   `timeout == 0` polls (non-blocking), `OVSTAGE_TIMEOUT_INFINITE` blocks,
   `OVSTAGE_ERROR_TIMEOUT` means not-ready-yet.
4. After a wait, inspect `wait_result.error_op_ids` / `error_op_id_count` for per-op
   failures and retrieve each message with `ovstage_get_last_op_error(instance, op_id)`.
5. To run the CPU ahead: keep enqueuing across ordinals without adding explicit waits; only
   `wait_op` (or `fetch_*` with a timeout) when you actually need a result. This pipelines work
   but does not guarantee concurrent backend execution.
6. Before submitting an operation that overlaps the same attribute and prim paths, release fetched
   read groups with `release_group` and finish active maps with `unmap_*`. If a read overlaps a
   pending write, wait for the write, then retry the read.
7. Release op-tracking state with `ovstage_release_op` once an op is known complete.
8. When changing code, run the narrow example/test that exercises the op whenever practical.

## Output Format

- For explanations, cite the relevant API names, source snippets, and caveats.
- For code changes, summarize the files changed, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before documenting new API usage.
- **Non-blocking poll / pipelined-submission patterns are snippet-backed** — the
  write-flavors example ships the `timeout == 0` poll loop and the multi-ordinal
  CPU-ahead pattern (`pipelined-submission`, `poll-wait-release`); the minimal example
  additionally shows the simple blocking form (`OVSTAGE_TIMEOUT_INFINITE`).
- **Async execution does not mean wait-free submission or parallel backend execution.** The
  current implementation performs validation and request preparation on the submitting thread;
  an enqueue that consumes a not-yet-ready handle may also wait for that handle's producer.
  Accepted work then runs through one serialized backend lane per instance. Different ordinals
  are dependency-independent and may execute concurrently under the API contract, but this build
  does not provide that execution parallelism. Use CPU-ahead submission to pipeline work and
  defer observation, not as a guarantee of submission latency, multi-core scaling, or GPU overlap.
- **Fail-on-overlap is a current-backend policy, not a general OVStage contract rule.** The
  current implementation may reject a read overlapping an earlier pending write,
  or a write/map/delete overlapping a live read group or active map on the same attribute and
  prim paths. This protects borrowed read storage and map layout. The API contract requires safe
  lifetimes and ordering, but another implementation could serialize, add dependencies, or retain
  versions instead of rejecting. Do not confuse this policy with genuine contract-level failures
  such as write-floor violations, INSERT conflicts, schema mismatches, or a map whose layout
  actually changed.
- **Latest-snapshot payloads:** this build retains only the latest committed payload;
  async consumers may lag within the bounded dirty-membership frontier reported by
  `ovstage_get_oldest_preserved_ordinal`.

## Overview

ovstage is an **asynchronous, ordinal-keyed submit/observe** system:

- **Submission is synchronous; execution is asynchronous.** State-mutating and data-producing
  calls return an `ovstage_enqueue_result_t { status; op_index; }` before queued execution
  completes. Submission may still validate, prepare/copy request state, or wait for a pending
  input handle, so it has no wait-free or constant-time guarantee.
- **Ordinal-keyed ordering.** Writes/deletes/advances carry an explicit `ordinal`.
  Same-ordinal ops execute in submission order; **different-ordinal ops are independent by
  contract and may run concurrently in another implementation**. This build currently sends
  all backend tasks for an instance through one serialized execution lane.
- **Observe on demand.** Block or poll with `ovstage_wait_op`; obtain stage data with
  `ovstage_fetch_*(handle, timeout, out)`. `timeout == 0` polls; `OVSTAGE_TIMEOUT_INFINITE`
  blocks; `OVSTAGE_ERROR_TIMEOUT` means the result is not ready yet.

This is what lets a producer (physics, etc.) push many ordinals of data while consumers
observe at their own cadence.

## C — enqueue, wait, and per-op errors

The minimal example's helper checks the enqueue status, waits on the `op_id`, and reports
any per-op errors surfaced by the wait — the core async pattern:

> **Source:** `examples/c/minimal/main.cpp` snippet `enqueue-wait-error`

A blocking write → seal → read sequence (each step waited with `OVSTAGE_TIMEOUT_INFINITE`):

> **Source:** `examples/c/minimal/main.cpp` snippet `minimal-write-read`

## C — non-blocking poll (CPU ahead)

To run the CPU ahead, submit without adding explicit waits and poll with `timeout == 0`, doing
other work between polls. A submission can still spend time in synchronous preparation or wait
for a prerequisite handle, and queued backend tasks currently execute serially per instance:

```c
// op_id from a prior enqueue (e.g. ovstage_write_attribute(...).op_index)
ovstage_op_wait_result_t wait{};
ovstage_api_status_t s = ovstage_wait_op(stage, op_id, /*timeout=*/0, &wait);
if (s == OVSTAGE_ERROR_TIMEOUT) {
    // not done yet — go do other CPU work / submit the next ordinal, retry later.
    // wait.lowest_pending_op_id reports the lowest still-pending op in the chain.
} else if (s == OVSTAGE_OK) {
    // completed (check wait.error_op_id_count for per-op failures)
    ovstage_release_op(stage, op_id);
}
```

The shipping form of this pattern — several ordinals submitted ahead, then drained with
zero-timeout polls that release each completed op:

> **Source:** `examples/c/write-flavors/main.cpp` snippets `pipelined-submission`, `poll-wait-release`

## Key Types / Functions

| Symbol | Role |
|--------|------|
| `ovstage_enqueue_result_t { status; op_index }` | result of an async enqueue; `op_index == OVSTAGE_INVALID_OP_ID` when `status != OVSTAGE_OK` |
| `ovstage_wait_op(instance, op_id, timeout, &wait_result)` | wait/poll for an op + its dependency chain |
| `ovstage_timeout_ns_t` | `0` = poll, `OVSTAGE_TIMEOUT_INFINITE` = block, else nanoseconds |
| `ovstage_op_wait_result_t { error_op_ids; error_op_id_count; lowest_pending_op_id }` | per-wait errors + partial-progress cursor (on timeout) |
| `ovstage_get_last_op_error(instance, op_id)` | message for a failed op (transient; see `error-handling`) |
| `ovstage_release_op(instance, op_id)` | release op-tracking state after completion |
| `ovstage_fetch_*(handle, timeout, out)` | obtain query/read/ordinal results; same timeout semantics |

## Troubleshooting

- **Enqueue OK but no effect yet** — enqueue success means *accepted*, not *executed*. Wait
  (or fetch) before relying on the result, and check `error_op_ids` after the wait.
- **`OVSTAGE_ERROR_TIMEOUT` from a poll is normal** — it means not-ready-yet, distinct from
  `OVSTAGE_ERROR_OP_FAILED` (ran and failed). Use `lowest_pending_op_id` for progress.
- **Concurrency surprise** — a `wait_op` returning `OVSTAGE_OK` does not mean *all* your
  enqueues finished; ops in other ordinal buckets may still be in flight. Wait on the
  specific op id you care about.
- **Pipelining does not improve backend parallelism** — expected in this build: one backend lane
  executes queued tasks per instance. Pipelining reduces caller-side wait placement; it does not
  promise parallel execution or bounded enqueue latency.
- **`OVSTAGE_ERROR_OP_FAILED` mentions an overlapping outstanding read/write/map** — release the
  fetched group or active map, or wait for the pending write, then retry. Narrow the operation to
  disjoint attribute/path coverage when possible. This is current-backend conflict handling, not
  evidence that overlap is universally invalid in the OVStage contract.
- **Op-error strings are transient** — `ovstage_get_last_op_error` is valid only until the
  next `wait_op` on the same thread; copy it if needed (see `error-handling`).

## Python

The Python bindings expose the same model: enqueues return handle objects (`Query` /
`Read` / `Map`) with a `.wait(timeout=…)` that raises `ovstage.OvstageError` on op failure
(`timeout=0` polls, `TIMEOUT_INFINITE` blocks, and other finite values wait up to the
timeout). See the `error-handling` skill for the exception surface.

> **Source:** `examples/python/minimal/main.py` snippet `minimal-write-read`
>
> Followed by: `examples/python/minimal/main.py` snippet `nonblocking-poll`

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `error-handling` skill — enqueue/wait error reporting and transient op-error strings.
- `application-flow` skill — where waiting fits in the end-to-end lifecycle.
- `dlpack-tensor-exchange` skill — async data exchange + GPU sync.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
