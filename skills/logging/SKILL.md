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
name: logging
description: >
  Route ovstage's diagnostic log messages (and messages from its USD support layer)
  to a process-global callback with a severity threshold and a RUST_LOG-style
  per-channel filter, and force delivery with flush_log. Use when the user asks about
  ovstage logging, a log callback, log severity or verbosity, channel filtering, or
  capturing/silencing ovstage/USD runtime log output. This is distinct from error
  handling (return codes) — see the error-handling skill for that.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - logging
  - diagnostics
tools:
  - Read
  - Grep
---

# Logging

## When to Use

Use this skill when the user wants to **capture, route, filter, or silence** ovstage's
log output: install a log callback, set a severity threshold, filter by channel, or
flush pending messages before a checkpoint. For operation/return-code error handling
(not logging), use the `error-handling` skill instead.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- The desired default severity threshold (`LogSeverity` / `ovstage_log_severity_t`).
- Any per-channel overrides (a comma-separated `<channel>=<level>` filter).
- Target API surface: C, Python, or both.
- Whether the caller needs a barrier (`flush_log`) before reading captured output.
- The shipped headers (`ovstage.h`, `ovstage_types.h`) and the referenced test snippet
  are the authoritative contract.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced snippet.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Hold a **live instance/`Stage`** when installing a callback — the callback is
  process-global but the runtime must be bootstrapped first.
- Do **not** emit ovstage/USD log messages from inside the callback (it re-enters the
  dispatcher and can feed back indefinitely).

## Instructions

1. Install the callback: C `ovstage_set_log_callback(severity, channel_filter, cb, user_data)`;
   Python `set_log_callback(cb, severity=, channel_filter=)`. The callback receives
   `(severity, timestamp, message)`. In C, `message` is valid only during the call, so copy it
   to keep it; the Python binding hands the callback an owned `str`, so there is no such hazard.
2. Choose the default `severity` threshold for channels not named in `channel_filter`
   (`NONE` disables all unmatched channels); add `<channel>=<level>` overrides as needed.
3. Do the log-producing work.
4. Call `flush_log(timeout)` to force pending (asynchronously-dispatched) messages through
   before reading your captured output.
5. Clear delivery by installing a NULL/`None` callback (which also flushes and tears down
   the dispatcher thread).

## Output Format

- For explanations, cite the API names, the source snippet, the severity/threshold and
  channel-filter semantics, and the async-delivery + `flush_log` caveat.
- For code changes, summarize the files touched, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippet is the source of truth; this skill composes it and describes the
  surrounding logging API rather than introducing new code.
- **Process-global, single callback.** `set_log_callback` replaces any prior callback for
  the whole process; it is not per-instance.
- **Asynchronous delivery.** Messages are dispatched on a dedicated thread, so they may
  arrive after the call that produced them — use `flush_log` as a barrier before asserting
  on or reading captured output.
- **Callback must not log.** Emitting ovstage/USD messages from the callback re-enters the
  dispatcher; a Python callback that raises has its traceback printed and is then suppressed.
- **Not an error channel.** Logging is diagnostic output; operation success/failure is
  reported via return codes / `OvstageError` (see `error-handling`).
- **⚠️ Draft — API in flux.** Treat exact symbols/ordering as provisional against the headers.

## Overview

`ovstage_set_log_callback` routes ovstage's log messages — and messages from its USD
support layer — to one process-global callback. `severity` is the default threshold
for channels not matched by a rule in `channel_filter` (messages below it are dropped);
`channel_filter` is an optional comma-separated `<channel>=<level>` list (e.g.
`"omni.ovstage=verbose"`), `NULL` applying `severity` uniformly. Delivery is
asynchronous on a dispatcher thread; `ovstage_flush_log(timeout)` blocks until messages
emitted before the call have drained.

Severities (`ovstage_log_severity_t` / `LogSeverity`): `VERBOSE` (-2), `INFO` (-1),
`WARNING` (0), `ERROR` (1), `NONE` (3, a threshold sentinel that disables all logging and
is never delivered).

## C

Install a callback, prove a bogus channel-prefix filter with a `NONE` default threshold
suppresses everything, flush, and clear:

> **Source:** `tests/c/test_logging.cpp` snippet `log-callback-filter-c`

## Python

The Python binding takes the callback first, then `severity` / `channel_filter`; a `None`
callback clears delivery:

> **Source:** `tests/python/test_logging.py` snippet `log-callback-filter`

## Key Types / Functions

| Purpose | C | Python |
|---------|---|--------|
| Install / clear callback | `ovstage_set_log_callback(severity, channel_filter, cb, user_data)` | `set_log_callback(cb, severity=, channel_filter=)` (`None` clears) |
| Flush pending messages | `ovstage_flush_log(timeout)` | `flush_log(timeout=)` |
| Severity levels | `ovstage_log_severity_t` (`OVSTAGE_LOG_*`) | `LogSeverity` |
| Callback signature | `void(ovstage_log_severity_t, double, ovx_string_t, void*)` | `f(severity, timestamp, message)` |

## Troubleshooting

- **No messages delivered** — the default `severity` is too high (or `NONE`), or a channel
  filter excludes the channels you expected; lower the threshold / adjust the filter. Also
  confirm you called `flush_log` before checking (delivery is asynchronous).
- **`set_log_callback` fails (`OP_FAILED`)** — the runtime is not bootstrapped; create a
  `Stage` / instance first.
- **Filter rejected (`INVALID_ARGUMENT`)** — the `channel_filter` string failed to parse;
  use the `<channel>=<level>` form with levels verbose|debug|info|warn|warning|error|fatal|none.
- **`flush_log` hangs** — a callback that blocks stalls the dispatcher; pass a finite
  timeout, and never block (or log) inside the callback.
- **Message text garbage after the call (C only)** — the C `message` is valid only during the
  callback; copy it if it must outlive the call. The Python binding gives you an owned `str`.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `error-handling` — operation return codes and per-op errors (the non-logging failure surface).
- `loading-usd` — population is a convenient source of runtime log traffic to capture.
- The shipped `include/ovstage/ovstage.h` / `ovstage_types.h` headers are the authoritative contract.
