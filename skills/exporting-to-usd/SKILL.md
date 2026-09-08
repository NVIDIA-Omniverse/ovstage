---
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
name: exporting-to-usd
description: >
  Export selected ovstage runtime state to USD from C or Python. Use for saved
  typed hierarchies, sparse overlays, reusable USD destinations, async export
  completion, or changed-property export.
  Prefer the metadata-driven typed-hierarchy workflow for ordinary USD-populated
  subtrees and explicit rules only for intentional mapping differences.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - population
  - usd
  - export
  - persistence
tools:
  - Read
  - Grep
---

# Exporting ovstage to USD

## When to Use

Use this skill for **ovstage runtime -> USD** authoring, destination ownership,
and persistence. For USD -> ovstage ingestion use `loading-usd`; for an
application-level workflow use `application-flow`; for status/error handling
use `error-handling`; and for replacing a direct USD workflow consult
`usd-to-ovstage-migration`.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- The source ovstage instance, kept alive for the whole export.
- A committed `ordinal` to export from, and `since_ordinal` when exporting only
  changed properties.
- The source selection: a subtree root for the typed helper, or the prim and
  property predicates for an explicit descriptor.
- The destination identifier and its form: one-shot file or reusable destination
  (`EMPTY` or `OPEN_EXISTING`).
- Whether the call is synchronous or asynchronous, since async adds a terminal
  wait and changes who may touch the destination while it is pending.
- The shipped `ovstage_population_export.h` header and the referenced test
  snippets are the authoritative contract.

## Prerequisites

- Read `AGENTS.md`, then `include/ovstage/ovstage_population_export.h`.
- Check `population.export_available()` only as an optional capability preflight.
  It does not alter export selection, projection, persistence, or error behavior.
- Keep the source stage alive, use a committed `ordinal`, and state the source
  selection and destination identifier before proposing an export.
- For build/link/runtime setup, use `project-setup-c` or `project-setup-python`.
  `ovx_string_t` is a pointer-plus-length view, not a null-terminated C string.

## Instructions

1. **Pick the smallest correct route** from the table below, and note who owns
   the save for that route.
2. **Build the source selection.** Prefer the typed-hierarchy recipe for an
   ordinary USD-populated subtree; fall back to explicit rules only for a
   deliberate mapping difference.
3. **Export.** Call the synchronous entry point, or enqueue the async one and
   keep its operation id.
4. **Reach terminal state.** Every accepted async create/export/save/destroy
   needs its own export-specific terminal wait; a timeout is not terminal.
5. **Save if you own the save.** One-shot file routes save before return;
   reusable destinations save only on an explicit `save()` / `*_destination_save`.
6. **Verify and report** in the order given under Output Format.

## Pick the Smallest Correct Route

| Need | Route | Save owner |
|---|---|---|
| Write one typed hierarchy to a new/replaced file | Python `export_typed_hierarchy_to_usd_file` or C descriptor helper + `ovstage_population_export_to_usd_file` | The one-shot file route saves before return |
| Write one explicit sparse overlay | `export_to_usd_file` / C file API | The one-shot file route saves before return |
| Accumulate several exports or preserve a layer stack | `ExportDestination` / C destination handle with `OPEN_EXISTING` | Caller calls `save()` / `*_destination_save` |

For a resolver identifier, do not assume an OS file path. Reopen or inspect it
through USD/resolver APIs. A one-shot file route starts `EMPTY` and replaces
stored content only on successful save.

ovstage opens and owns every destination. There is no route that authors into a
caller-owned USD layer or stage: layer/stage identifiers are not portable across
USD runtimes, so hand off through storage — export to a file, then open or reload
it in the host's USD runtime.

## Typed Hierarchy: Default Recipe

For an ordinary USD-populated subtree, use the typed helper. It selects the root
and descendants, uses `DEF`, reads recorded prim types, applies recorded API
schemas, and projects supported schema-declared attributes. It does not require
passing `pxr` objects across the public ABI.

Python uses `population.export_typed_hierarchy_to_usd_file(stage,
destination_identifier, "/World/Props", ordinal)`. C uses the public
`ovstage_population_export_desc_init_typed_hierarchy` convenience initializer,
then the existing `ovstage_population_export_to_usd_file` API. Follow the
tested/shipped patterns below rather than recreating descriptor setup from prose.

The C descriptor retains the caller-owned `root` view and its referenced
character bytes through synchronous return or successful async enqueue; the
async enqueue deep-copies the descriptor graph. Do not change or free either
early. Let normal export validation reject null, relative, or malformed
selections atomically. `/` is the valid absolute-root selection for the whole
stage, matching the shared population predicate contract.

Typed `DEF` projection uses recorded prim types, recorded applied schemas, and
schema-declared projection. Do not claim every `DEF` export needs both policies:
explicit rules remain appropriate for a deliberate type, schema, rename, custom
namespace, relationship, connection, or metadata mapping.

## Reusable Destination Rules

- Choose `OPEN_EXISTING` to retain existing root-layer content and its layer
  stack in memory. It does not save automatically.
- Canonical destination identifiers have process-wide active ownership. Close or
  destroy releases ownership and never saves; a successful reusable `save`
  leaves the destination open for more export/save calls.
- A dropped live Python `ExportDestination` queues a best-effort no-save release
  and emits `ResourceWarning`; it is a safety net, not normal lifecycle control.
- Each accepted create/export/save/destroy operation needs its own export-specific
  terminal wait. A timeout is not terminal and does not consume the report.
- The export/destination FIFO is separate from generic population observation:
  use generic `ovstage_population_wait_op` / the Python population wait separately
  to observe and drain unrelated population failures.
- Do not mix synchronous destination access with pending async destination work.
- A failed reusable export leaves prior in-memory destination state unchanged.
  A failed save has no portable atomic-replacement guarantee for stored output.

`since_ordinal` filters dirty membership in `(since_ordinal, ordinal]`; selected
values are authored as current USD default opinions, never intermediate time
samples.

## Output Format

For every recipe, report these facts in this order:

1. terminal status (and operation id for async work);
2. relevant report counters, including `prims_exported`,
   `attributes_exported`, and `api_schemas_applied` when applicable;
3. reopened/authored content, using USD/resolver APIs for non-filesystem ids;
4. the exact source ordinal, selection, and destination ownership/save result.

Do not invent aggregate report keys. For code changes, also summarize the export
calls touched, the snippets affected, and the validation run.

## Tested and Shipped Patterns

Python typed hierarchy and durable output:

> **Source:** `tests/python/test_population_export.py` snippet `export-typed-hierarchy-to-usd-file`

C typed hierarchy and durable output:

> **Source:** `tests/c/test_population_export.cpp` snippet `export-typed-hierarchy-to-usd-c`

Shipped C example:

> **Source:** `examples/c/usd-export/main.cpp` snippet `typed-hierarchy-c-example`

Shipped Python example:

> **Source:** `examples/python/usd-export/main.py` snippet `typed-hierarchy-python-example`

Reusable destination with one explicit save:

> **Source:** `tests/python/test_population_export.py` snippet `export-reusable-destination`

Async file completion:

> **Source:** `tests/python/test_population_export.py` snippet `export-runtime-to-usd-async`

> **Source:** `tests/c/test_population_export.cpp` snippet `export-runtime-to-usd-async-c`

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets are the source of truth; this skill composes them and
  describes the surrounding export API rather than introducing new code.
- Export consumes one committed runtime snapshot, not historical payloads or
  intermediate values.
- Custom namespaces are opt-in. Relationships, connections, and material
  bindings need an explicit property kind and documented path semantic.
- Pre-interned token-ID scalar and array values export as ``token`` and
  ``token[]``. Positive connection export remains unavailable.
- A reusable `save` has no portable atomic-replacement guarantee, so a failed
  save may already have modified stored output.

## Troubleshooting

- **Stale or future ordinal** — `source_states_unavailable` is the relevant
  diagnostic counter; re-export from a committed ordinal.
- **Typed export missing types, schemas, or attributes** — the source may need an
  explicit rule or a deliberate metadata policy; inspect the terminal report
  rather than assuming the projection is broken.
- **Async destination reserved but never opened** — wait its creation operation
  first, then close/destroy only a successfully opened destination.
- **Accepted but nothing stored** — enqueue success is not completion, and a
  reusable destination never saves implicitly: reach the terminal wait, then
  call `save()` / `*_destination_save` if you own the save.
- **Destination identifier rejected as already active** — canonical identifiers
  have process-wide active ownership; close or destroy the existing destination
  before opening the same identifier again.

## References

- `include/ovstage/ovstage_population_export.h`
- `docs/scene/exporting_to_usd.rst`
- `examples/c/usd-export` and `examples/python/usd-export`
- `project-setup-c`, `project-setup-python`, `loading-usd`,
  `application-flow`, `error-handling`, and `usd-to-ovstage-migration`
