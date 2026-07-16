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
name: stage-queries
description: >
  Find prims with filter queries — predicates over reserved metadata built-ins
  (usd-path, usd-prim-type, usd-parent, usd-children, usd-schemas) and attribute
  presence — instead of explicit path lists, then introspect what a query matched.
  Use when the user asks to query, filter, select, or find prims by type, path,
  parent, child, applied schema, or attribute presence, or to read a query result's
  count / attributes / reusable handle.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - queries
  - filters
tools:
  - Read
  - Grep
---

# Stage Queries

## When to Use

Use this skill when the user wants to **find prims by a predicate** rather than by an
explicit path list: select by prim type, path (exact or subtree), parent, child, applied
schema, or attribute presence — and inspect what a query matched (count, reported
attributes, reusable handle). For building an explicit path list to query a *known* set of
prims, use `path-dictionary` instead.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- The selection criterion: which built-in metadata attribute + operator, or attribute-presence (`HAS`).
- Whether the prims carry the metadata built-ins (these require **population**; a purely
  client-written attribute column has `usd-path` and `HAS`-able columns but not
  `usd-prim-type`/`usd-parent`/`usd-children`/`usd-schemas`).
- Target API surface: C, Python, or both.
- Whether the caller needs just a count, the matched set to read from, or the reported attributes.
- The shipped headers and the referenced example/test snippets are the authoritative contract.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced snippets.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the async **submit/observe** model (`cpu-ahead-gpu-async`): `query` is an enqueue
  returning a handle; drive it to completion (`ovstage_wait_op` / `.wait()`) before fetching.
- Queries resolve against the **latest committed state** — no write-floor advance is needed for a
  query to see a populated/written prim (reads of matched columns still target sealed data).

## Instructions

1. Build the filter: a `Filter` is a conjunction (AND) of `Predicate`s; each predicate is an
   attribute + `FilterOp` + string values. A `None`/empty filter matches all prims.
2. Enqueue `ovstage_query` (C) / `Stage.query` (Python) and drive it to completion.
3. Fetch the result (`ovstage_fetch_query_result` / `Query.result()`): read `total_prim_count`,
   the reported `attributes`, and `all_handle` (the same query handle, echoed back).
4. To read columns from the matched set, feed the query handle (or `all_handle`) to
   `read_attributes`.
5. Release the query handle (and the result payload in C) before destroying the instance.
6. For a known, explicit set of prims, prefer `query_from_path_list` (see `path-dictionary`).

## Output Format

- For explanations, cite the API names, the source snippet(s), the supported predicate matrix,
  and the population caveat.
- For code changes, summarize the files touched, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before
  documenting new API usage.
- **Narrow support matrix.** The bindings/headers describe more operators than the current
  implementation accepts; anything outside the table below is rejected at enqueue
  (`NOT_SUPPORTED`).
- **Values are always strings**, even for token/type attributes. Predicates in one `Filter` AND
  together.
- **Metadata built-ins need population.** `usd-prim-type`/`usd-parent`/`usd-children`/`usd-schemas`
  are auto-maintained by the population bridge; a scene built only with `write_attribute` exposes
  `usd-path` and `HAS`-able user columns but not those relational built-ins.
- **Instancing queries require populated content.** C exposes the native `ovstage_instancing.h`
  path-list handles; Python exposes string-returning wrappers in `ovstage.instancing`.
- **⚠️ Draft — API in flux.** Treat exact symbols/ordering as provisional against the headers.

## Overview

`query` finds prims by predicate and reserves a query handle; `fetch_query_result` reports what it
matched. The supported predicate matrix:

| Attribute | Operators | Selects |
|-----------|-----------|---------|
| any attribute | `HAS` | prims where the attribute exists (no value test) |
| `usd-path` | `IN`, `PREFIX` | exact path(s) / subtree (byte-prefix; trailing `/` scopes) |
| `usd-parent` | `IN` | direct children of the named parent(s) |
| `usd-children` | `CONTAINS` | prims having the named child |
| `usd-prim-type` | `IN` | prims of the named type(s) |
| `usd-schemas` | `CONTAINS` | applied-schema membership |

> **`usd-active` is not supported.** It appears in the header contract for
> stability, but a live prim is always active, so the attribute carries no
> information; any read or predicate naming it is rejected with
> `NOT_SUPPORTED`. Subject to removal in a future release.

> **`usd-active` is not supported.** It appears in the header contract for
> stability, but a live prim is always active, so the attribute carries no
> information; any read or predicate naming it is rejected with
> `NOT_SUPPORTED`. Subject to removal in a future release.

## C

Run one filter query per predicate (no path list — the stage finds the prims), then map the
scene-graph-instancing structure:

> **Source:** `examples/c/queries/main.cpp` snippet `filter-predicates`
>
> Followed by: `examples/c/queries/main.cpp` snippet `query-introspection`
>
> Followed by: `examples/c/queries/main.cpp` snippet `instancing-queries-c` (scene-graph instancing)

The public C test asserts the documented match set for each predicate against the produced package:

> **Source:** `tests/c/test_queries.cpp` snippet `query-predicate-matrix-c`

usd-path `IN` and attribute `HAS` also work on purely client-written prims (no population):

> **Source:** `tests/c/test_queries.cpp` snippets `query-by-usd-path-c`, `query-has-attribute-c`

Introspection — match count, the scoped attribute list, and the reusable `all_handle`:

> **Source:** `tests/c/test_queries.cpp` snippet `query-result-introspection-c`

## Python

`Stage.query(filter=Filter([...]))` returns a `Query` handle; `Query.result()` reports the match:

> **Source:** `examples/python/queries/main.py` snippet `filter-predicates`
>
> Followed by: `examples/python/queries/main.py` snippet `query-introspection`
>
> Followed by: `examples/python/queries/main.py` snippet `instancing-queries`

The public Python test asserts the documented match set for each predicate against the wheel:

> **Source:** `tests/python/test_queries.py` snippet `query-predicate-matrix`

usd-path `IN` and attribute `HAS` also work on purely client-written prims (no population):

> **Source:** `tests/python/test_queries.py` snippets `query-by-usd-path`, `query-has-attribute`

Introspection — match count, the scoped attribute list, and the reusable `all_handle`:

> **Source:** `tests/python/test_queries.py` snippet `query-result-introspection`

## Key Types / Functions

| Python | C |
|--------|---|
| `Stage.query(filter=, attrs=)` → `Query` | `ovstage_query(instance, filter, attrs, attr_count, &handle)` |
| `Query.result()` → `QueryResult` | `ovstage_fetch_query_result(instance, handle, timeout, &result)` |
| `QueryResult.total_prim_count` / `.attributes` / `.all_handle` | `ovstage_query_result_t.total_prim_count` / `.attributes` / `.all_handle` |
| `Filter([Predicate(attr, FilterOp.IN, [..])])` | `ovstage_filter_t` / `ovstage_predicate_t` / `ovstage_filter_op_t` |
| `Stage.release_query(handle)` | `ovstage_release_query` (+ `ovstage_release_query_result`) |

## Troubleshooting

- **Enqueue rejected `OVSTAGE_ERROR_NOT_SUPPORTED`** — the attribute+operator pairing is outside
  the matrix above. Use a supported pairing; the bindings advertise more than the engine accepts.
- **Zero matches for a metadata predicate** — the scene wasn't populated, so
  `usd-prim-type`/`usd-parent`/`usd-children`/`usd-schemas` aren't authored. Populate first, or
  filter on `usd-path`/`HAS`, which work on client-written prims.
- **`PREFIX "/World"` matches `/Worldwide`** — `usd-path` PREFIX is byte-prefix matching on the
  path string. Append a trailing `/` to scope to the subtree (`PREFIX "/World/"`).
- **Empty read from a matched set** — the columns you read weren't sealed; advance the write floor
  to/above the write ordinal before reading (queries see latest committed state, but reads target
  sealed data). See `application-flow`.
- **Handle leak / teardown crash** — release the query (and in C the query result) before
  destroying the instance; read-group prim lists are borrows, never `destroy_path_list` them.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `path-dictionary` — building path lists and `query_from_path_list` for a known prim set.
- `loading-usd` — population, which authors the metadata built-ins these filters match.
- `cpu-ahead-gpu-async` — the async submit/observe model queries share with every op.
- `application-flow` — where queries fit in the create → write → seal → read lifecycle.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
