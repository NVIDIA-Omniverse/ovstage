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
name: usd-to-ovstage-migration
description: >
  Port code written against the USD API (pxr) to the ovstage data plane: map DefinePrim /
  CreateAttribute+Set / Get / RemovePrim / Sdf.ChangeBlock onto INSERT-mode writes,
  write_attribute, read_attributes, delete_attributes, and the batched write_attributes — and
  make the vectorization mental shift from one call per prim to one call per column over a prim
  set. Use when the user wants to migrate, port, or translate USD-API authoring code to ovstage,
  or asks what the ovstage equivalent of a USD call is.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - usd
  - migration
  - authoring
tools:
  - Read
  - Grep
---

# USD to ovstage Migration

## When to Use

Use this skill when the user is **porting USD-API code to ovstage**: code that creates prims,
authors and reads attributes, or deletes prims through `pxr` (`Usd.Stage`, `UsdPrim`,
`UsdAttribute`, `Sdf.ChangeBlock`) and should do the equivalent against the ovstage data plane.
It covers the call-by-call mapping **and** the mental shift that matters more than any single
call: USD habits are one call per prim; idiomatic ovstage is one **vectorized** call per column
over a prim set (and one **batched** op for several columns).

Do **not** use it to *mirror* an existing USD stage into ovstage automatically — that is
**population**, not migration (route to `runtime-loop`) — or for the mechanics of a single
ovstage call (route to the focused skills: `application-flow`, `dlpack-tensor-exchange`, etc.).

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- The USD-API code (or described workflow) being ported: which prims it defines, which
  attributes it sets/gets, what it deletes, and what it wraps in `Sdf.ChangeBlock`.
- The prim set each authored attribute covers — this becomes the ovstage query (path list or
  filter) that one vectorized write targets.
- The caller-owned **ordinals**: one per migrated step, advanced monotonically and sealed with
  `advance_write_floor`.
- Whether any USD feature in the source has **no ovstage counterpart** (timeSamples,
  composition/layers) — call those out instead of silently dropping them.
- The shipped headers and the referenced example snippets are the authoritative contract.

## Prerequisites

- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the async **submit/observe** model (`cpu-ahead-gpu-async`): writes are enqueues;
  nothing is visible to reads until the op completes and the **write floor** is advanced to its
  ordinal. There is no USD-style immediate read-after-write.
- The **application owns the ordinal lifecycle** — pick an ordinal per step, keep them
  monotonic, seal each with `advance_write_floor`.
- Attribute names and prim paths are interned through the path dictionary (`path-dictionary` /
  `string-handling`); reserved `usd-prim-type` payloads are **interned token ids** (uint64,
  `is_array=False`, semantic NONE), not strings.

## Instructions

1. **Map each USD call with the table below**, then restructure per-prim loops into per-column
   calls (see The Vectorization Shift).
2. **Create prims via the write itself.** There is no create-prim call: an INSERT-mode
   (create-only) write over a query materializes every queried prim; writing the reserved
   `usd-prim-type` column in that same op also stamps the types.
3. **Author values one call per column.** Replace N `CreateAttribute(...).Set(...)` calls with
   one `write_attribute` whose tensor holds one row per prim in the query. A per-prim loop of
   one-prim writes still works (N ops at one ordinal, or one ordinal each) — it is the naive
   port, not the idiomatic one.
4. **Replace `Sdf.ChangeBlock` groups with `write_attributes`.** Several columns land in ONE
   batched op (one `WriteDesc` per column, sharing the ordinal). Note the semantic upgrade:
   ChangeBlock coalesces only change *notification* around per-prim `Set` calls; the ovstage
   batch is genuinely one op — but a grouping, not an atomic transaction.
5. **Seal, then read whole columns.** `advance_write_floor` to the write ordinal, then replace
   per-prim `Get()` calls with one `read_attributes` over the query
   (`OrdinalRange.latest(ordinal)`), placing rows via `prim_index` / data `index_map`.
6. **Flag the not-equivalent features** (below) present in the source code instead of inventing
   mappings for them.

## Call Mapping

| USD (pxr) | ovstage equivalent | Note |
|-----------|--------------------|------|
| `stage.DefinePrim(path, type)` | INSERT-mode `Stage.write_attribute(query, "usd-prim-type", ordinal, tokens, is_array=False, prim_mode=PrimMode.INSERT)` | No create-prim call — prims come into existence via any attribute write; INSERT is create-only (fails on existing prims), and the reserved `usd-prim-type` column stamps one interned token id per prim (uint64, semantic NONE) |
| `prim.CreateAttribute(name, type).Set(value)` | `Stage.write_attribute(query, attr, ordinal, tensors, is_array=...)` | One call authors the whole column: one tensor row per prim in the query, at a caller-owned ordinal; the write creates the column (and, in UPSERT mode, any absent prims) |
| `attr.Get()` | `Stage.read_attributes(query, [attr], OrdinalRange.latest(N))` | One read op returns the current latest committed column; `N` is not a historical upper bound. Recorded payloads must be covered by the write floor. |
| `stage.RemovePrim(path)` | `Stage.delete_attributes(query, [], ordinal)` | An **empty attribute list** deletes the prims entirely; a non-empty list removes just those attributes; ordinal-keyed like any write, surfaced to range readers as an `is_delete` tombstone group |
| `Sdf.ChangeBlock()` around N edits | `Stage.write_attributes(query, [WriteDesc, ...], ordinal)` | ChangeBlock batches only change *notification* — still one `Set` call per attribute per prim; `write_attributes` lands several columns in ONE op (a grouping, not an atomic transaction) |

## The Vectorization Shift

The single most important rewrite is structural, not lexical. USD code iterates prims and makes
one call per prim per attribute:

```python
for prim, value in zip(prims, values):
    prim.CreateAttribute("temperature", Sdf.ValueTypeNames.Double).Set(value)
```

Idiomatic ovstage makes **one call per column over a prim set**: build a query covering the
prims once, then write one tensor with one row per prim. The naive per-prim port works — it is
just N ops instead of 1, and the gap widens with the prim count. When migrating, hoist the loop
body's attribute name out as the column, the loop's values out as the tensor, and the loop's
prims out as the query; reach for `write_attributes` when several columns land together.

## Not Equivalent

Do **not** invent mappings for these:

- **`attr.Set(value, time)` / timeSamples ≠ ordinals.** A timeSample is scene *content* (a value
  on the scene-time axis); an ordinal is an application-owned *version number* on the write
  axis. Migrate authored defaults; treat time-sampled data as a separate design question.
- **Composition has no ovstage counterpart.** Layers, sublayers, references, payloads, variants,
  inherits, edit targets: the ovstage runtime table is flat, composed data. Code that *edits
  composition* cannot be ported call-for-call.
- **Mirroring is population, not migration.** If the goal is "this existing USD stage, live in
  ovstage, tracking USD edits", use the population bridge (`open_usd`, `apply_usd_changes`) —
  see the `runtime-loop` skill — rather than rewriting the authoring code.

## Python

The workflow example runs the same workflow twice in one process — plain USD via `pxr`
(usd-core), never bound to ovstage, then the ovstage equivalents — so each USD snippet below
has a matching ovstage snippet printing identical values.

The USD habits being migrated — per-prim `DefinePrim`, per-prim `CreateAttribute(...).Set(...)`
with `attr.Get()` readback, and a notification-only `Sdf.ChangeBlock` batch:

> **Source:** `examples/python/usd-to-ovstage/main.py` snippet `usd-create-prims`

> **Source:** `examples/python/usd-to-ovstage/main.py` snippet `usd-author-one-by-one`

> **Source:** `examples/python/usd-to-ovstage/main.py` snippet `usd-changeblock-batch`

Prim creation via one INSERT write of the reserved `usd-prim-type` column (create + type stamp
in one op):

> **Source:** `examples/python/usd-to-ovstage/main.py` snippet `ovstage-create-prims`

The naive port (a loop of one-prim write ops — works, unbatched) versus the idiomatic
vectorized port (one op, one column over all prims):

> **Source:** `examples/python/usd-to-ovstage/main.py` snippet `ovstage-write-one-by-one`

> **Source:** `examples/python/usd-to-ovstage/main.py` snippet `ovstage-write-vectorized`

Several columns in one batched op — the `Sdf.ChangeBlock` group's closest relative:

> **Source:** `examples/python/usd-to-ovstage/main.py` snippet `ovstage-write-batched`

Whole-column readback in one read op, rows placed via `prim_index` / data `index_map`:

> **Source:** `examples/python/usd-to-ovstage/main.py` snippet `ovstage-read-column`

## Output Format

- For explanations, answer with the mapping-table row(s), the vectorized restructuring of the
  user's loop, and the key caveats (ordinal ownership, write floor before reads, not-equivalent
  features present in the source).
- For code changes, summarize the files touched, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before
  documenting new API usage.
- The mapping covers the data plane (prims, attributes, values, deletes). Renderer binding,
  schema-rich workflows, and USD features listed under Not Equivalent are out of scope.
- `write_attributes` is a grouping, not a transaction — entries may apply incrementally; one op
  id groups completion. Do not port code that relies on all-or-nothing `SdfChangeBlock`-free
  semantics onto it without checking.
- Typed attribute *definitions* do not migrate: ovstage columns get a storage dtype (and an
  optional `AttributeSemantic`) from the first write, not an `Sdf.ValueTypeName` schema.
- Reserved metadata contract: `usd-prim-type` requires `is_array=False`; `usd-schemas` requires
  `is_array=True`; both carry interned token ids, never strings.

## Troubleshooting

- **`PRIM_NOT_FOUND` on the create write** — INSERT is create-only: some queried prim already
  exists. Use UPSERT (the default) when "define or update" semantics are wanted.
- **Read fails with `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` after a write** — advance the write floor
  to the write ordinal first; reads only see sealed data at/below the floor (there is no USD-style
  immediate read-after-write).
- **`OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`** — a write used an ordinal at/below the floor. Keep
  ordinals monotonic across migrated steps.
- **Type stamp did not land / garbage type names** — `usd-prim-type` takes interned token ids
  (uint64) via the path dictionary, not strings; resolve reads back through `token_to_string`.
- **Values come back in the wrong prim order** — place rows via `group.prim_index(local)` (and
  the data `index_map` when present) instead of assuming row `i` is prim `i`.
- **Looking for the timeSamples equivalent** — there is none on the ordinal axis; see Not
  Equivalent.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `runtime-loop` — population: mirror a USD stage into ovstage and propagate USD edits (the
  bridge to use when the goal is mirroring, not migration).
- `application-flow` — the create → write → seal → read lifecycle every migrated step follows.
- `cpu-ahead-gpu-async` — the async submit/observe model and ordinal/write-floor semantics.
- `path-dictionary` / `string-handling` — interning attribute names, prim paths, and the token
  ids `usd-prim-type` carries.
- `error-handling` — status checks and per-op error reporting for enqueues and waits.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
