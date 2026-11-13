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
  (usd-path, usd-prim-type, usd-parent, usd-children, usd-schemas, usd-ancestor-types) and attribute
  presence — instead of explicit path lists, then introspect what a query matched.
  Use when the user asks to query, filter, select, or find prims by type, path,
  parent, child, applied schema, base type, or attribute presence, to read a query result's
  count / attributes / reusable handle, or to read all attributes or the attributes with a name
  prefix with the `*` and `prefix*` read selectors.
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
- Whether the prims carry the metadata needed by the predicate. `usd-prim-type`,
  `usd-schemas`, and `usd-ancestor-types` can be authored directly or populated from USD;
  `usd-path`, `usd-parent`, and `usd-children` derive from the stage's own structure.
- Target API surface: C, Python, or both.
- Whether the caller needs just a count, the matched set to read from, or the reported attributes.
- The shipped headers and the referenced example/test snippets are the authoritative contract.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced snippets.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the async **submit/observe** model (`cpu-ahead-gpu-async`): `query` is an enqueue
  returning a handle; drive it to completion (`ovstage_wait_op` / `.wait()`, which returns the
  query so `q.wait().result()` chains) before fetching.
- Queries resolve against the **latest committed state** — no write-floor advance is needed for a
  query to see a populated/written prim (reads of matched columns still target sealed data).

## Instructions

1. Build the filter: a `Filter` is a conjunction (AND) of `Predicate`s; each predicate is an
   attribute + `FilterOp` + string values. A `None`/empty filter matches all prims.
2. Enqueue `ovstage_query` (C) / `Stage.query` (Python) and drive it to completion.
3. Fetch the result (`ovstage_fetch_query_result` / `Query.result()`): read `total_prim_count`,
   the reported `attributes`, and `all_handle` (the same query handle, echoed back).
4. To read columns from the matched set, feed the query handle (or `all_handle`) to
   `read_attributes`. To read every attribute, or every attribute whose name starts with a
   prefix, pass the selector `*` or `prefix*` (for example `primvars:*`) as an attribute token.
   A pattern has one `*`, as its last character. Each read finds the matching names again.
   A latest `*` read also returns `usd-path`. Iterate each group's columns and key values by
   `column.attribute`. One attribute can come in more than one group, so keep every group's
   rows, keyed by prim. Names that
   start with `_` or `usd-` (except `usd-path`) are hidden from a pattern unless its prefix starts
   with the same namespace: use `usd-*` to read the built-ins (`usd-parent`, `usd-children`,
   `usd-prim-type`, `usd-schemas`, `usd-ancestor-types`).

   > **Source:** `tests/python/test_read_patterns.py` snippet `read-attribute-patterns`,
   > `tests/c/test_read_patterns.cpp` snippet `read-attribute-patterns-c`
5. Release the query handle (and the result payload in C) before destroying the instance.
6. For a known, explicit set of prims, prefer `query_from_path_list` (see `path-dictionary`).

## Output Format

- For explanations, cite the API names, the source snippet(s), the supported predicate matrix,
  and the metadata requirements.
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
- **Recorded metadata must be authored.** `usd-prim-type`, `usd-schemas`, and
  `usd-ancestor-types` are writable recorded columns; USD population is one way to author them.
  Changing `usd-prim-type` does not update `usd-ancestor-types` automatically; direct authors
  must keep them consistent. `usd-path`, `usd-parent`, and `usd-children` derive from stage
  structure, including for client-authored prims, and cannot be written directly.
- **Instancing queries require populated content.** C exposes the native `ovstage_instancing.h`
  path-list handles; Python exposes string-returning wrappers in `ovstage.instancing`.
- **⚠️ Draft — API in flux.** Treat exact symbols/ordering as provisional against the headers.

## Overview

`query` finds prims by predicate and reserves a query handle; `fetch_query_result` reports what it
matched. The supported predicate matrix:

| Attribute | Operators | Selects |
|-----------|-----------|---------|
| any attribute | `HAS` | prims where the attribute exists (no value test) |
| any attribute | `NOT_HAS` | prims where the attribute is absent (complement of `HAS`; none for `usd-path`) |
| `usd-path` | `IN`, `PREFIX` | exact path(s) / subtree (byte-prefix; trailing `/` scopes) |
| `usd-parent` | `IN` | direct children of the named parent(s) |
| `usd-children` | `CONTAINS` | prims having the named child |
| `usd-prim-type` | `IN` | prims of the named type(s) |
| `usd-schemas` | `CONTAINS` | applied-schema membership |
| `usd-ancestor-types` | `CONTAINS` | prims whose type derives from the named type |

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
- **Zero matches for a metadata predicate** — check the queried values and committed prim
  structure. For `usd-prim-type`, `usd-schemas`, or `usd-ancestor-types`, check that the needed
  column was authored and the write completed; author it directly or populate it from USD.
  For `usd-parent` or `usd-children`, check the actual parent/child paths in the stage.
- **`PREFIX "/World"` matches `/Worldwide`** — `usd-path` PREFIX is byte-prefix matching on the
  path string. Append a trailing `/` to scope to the subtree (`PREFIX "/World/"`).
- **`OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` reading a matched set** — the selected columns weren't
  sealed; advance the write floor to/above the write ordinal before reading (queries see latest
  committed state, but reads target sealed data). See `application-flow`.
- **`OVSTAGE_ERROR_INVALID_ARGUMENT` for a name that contains `*`** — `*` is a read selector, so
  writes, maps, deletes, query filters and attribute lists, `get_attribute_write_floor`, and the
  lists of `SCOPE_INCLUDE` / `SCOPE_EXCLUDE` floor advances reject such a name (`SCOPE_ALL` does
  not check its list). In `read_attributes`, only `*` and a trailing `prefix*` are valid; `*mass` and `a*b` are not.
- **Range read with `*` or `prefix*` fails with `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`, but the
  same read with attribute names passes** — the stage-wide floor is below the range end. A pattern
  can match a name that no prim has yet, and only the stage-wide floor seals that name, so
  per-attribute floors are not enough. Advance the floor to the range end with `SCOPE_ALL`. Do not
  check it with `get_attribute_write_floor` and an empty attribute: that call can report a higher
  value.
- **Range read with a pattern fails with `OVSTAGE_ERROR_OUT_OF_RANGE` at an old start** — the
  range starts below `ovstage_get_oldest_preserved_ordinal`. Start the range at or above it.
- **`OVSTAGE_ERROR_OUT_OF_RANGE` when reading** — a selected `(attribute, path)` has a recorded
  change after the requested end, and older values are not retained.
  If newer values are acceptable, move the end forward or request current values with end
  `OVSTAGE_ORDINAL_MAX` (Python: `ovstage.ORDINAL_MAX`); a selected unsealed change still returns `WRITE_FLOOR_VIOLATION`.
  Pairs selected with `ovstage_enable_history()` on an instance of `history_selective`
  (`ovstage_instance_desc_t.implementation` set to `history_selective`) answer the window from retained history instead.
  History support (the `history_selective` implementation and `ovstage_enable_history()`) is
  experimental and can change between releases without a deprecation period.
- **Handle leak / teardown crash** — release the query (and in C the query result) before
  destroying the instance; read-group prim lists are borrows, never `destroy_path_list` them.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `path-dictionary` — building path lists and `query_from_path_list` for a known prim set.
- `loading-usd` — USD population, one way to author recorded metadata for these filters.
- `cpu-ahead-gpu-async` — the async submit/observe model queries share with every op.
- `application-flow` — where queries fit in the create → write → seal → read lifecycle.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
