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
name: update-0-3-to-0-4
description: >
  Upgrade an existing C, C++, or Python application from ovstage 0.3.x to 0.4.x. Use when asked to upgrade ovstage, migrate from 0.3, port from 0.3 to 0.4 ("0.3 to 0.4"), or fix code that broke after moving to ovstage 0.4: signed ordinals and OVSTAGE_ORDINAL_MAX, multi-column read groups, ovstage_release_groups, typed handles and tokens, ovstage_data_plane_vtable_t, OVX_INVALID_TOKEN, the removed PHYSICS and ALL domains, or instance-root and PointInstancer data changes.
license: LicenseRef-NvidiaProprietary
version: "0.4.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - upgrade
  - "0.4"
  - migration
tools:
  - Read
  - Grep
  - Edit
---

# Update ovstage 0.3 to 0.4

Port an existing application with the minimum changes the 0.4 public contract requires. Do not adopt unrelated 0.4 features or rewrite working code.

## When to Use

Use this skill when upgrading C, C++, or Python code from ovstage 0.3.x to 0.4.x, or when code that worked on 0.3 fails to compile, raises `TypeError`, or returns different results on 0.4.

Do not use it for applications on 0.2.x or earlier; apply `update-0-2-to-0-3` first. For migrating `pxr` USD code to ovstage, use `usd-to-ovstage-migration`.

## Inputs

Resolve these before editing:

- Source and target ovstage versions, and where the package is pinned or vendored.
- Languages in scope: C, C++, Python, or mixed.
- Whether the application populates with `PHYSICS` or `ALL`, reads instance roots or `PointInstancer.prototypes`, or reads `usd-path` change ranges.
- Whether it calls ovstage through the vtable directly or only through the `ovstage_*` functions.
- Whether it keeps read groups fetched while it writes, maps, clones or deletes, or builds population selectors in C/C++.
- Files in scope: sources, build files, and tests.

## Prerequisites

- Access to the application's source, package pins, and build configuration.
- An ovstage 0.4.x package (headers, libraries, or the Python wheel). Native libraries, headers and Python bindings must all come from that package.
- Read the `[0.4.0]` section of [`../../CHANGELOG.md`](../../CHANGELOG.md) and the page [`../../docs/guides/upgrading_from_0_3.rst`](../../docs/guides/upgrading_from_0_3.rst) before changing code. The installed 0.4 headers and Python signatures override both if they differ.

## Instructions

Steps 3 to 16 follow the groups of the guide's breaking-change tables, from changes the compiler reports to changes that only alter populated data, then behavior changes.

1. **Pin 0.4.** Update the package pin or vendored copy. Rebuild every translation unit that includes ovstage headers, and both sides of any C++ interface that passes ovstage types; unchanged struct sizes do not mean binary compatibility.
2. **Run the audit searches below before editing.** Apply a change only where a search matches. Skip items that do not match.

**C/C++ names and layouts**

3. **Renamed C types.** Grep and rename with no alias: `ovstage_vtable_t` to `ovstage_data_plane_vtable_t`, `ovstage_context_t` to `ovstage_data_plane_context_t`, and `ovx_string_or_token_t` to `ovx_token_or_string_t`. Python ctypes users rename `ovstage_vtable_t`, `ovstage_context_p` and `ovx_string_or_token_t` to `ovstage_data_plane_vtable_t`, `ovstage_data_plane_context_p` and `ovx_token_or_string_t`.
4. **Instance layout and family functions.** `ovstage_instance_t` is now `{data_plane, ext_instances}`. Grep:
    - `instance->vtable` and `instance->context`: use `instance->data_plane.vtable` and `instance->data_plane.context`, or call the `ovstage_*` functions.
    - `dlsym` / `GetProcAddress` (or any by-name lookup) of `ovstage_get_hierarchy`, `ovstage_compute_hierarchy`, `ovstage_fetch_hierarchy_result`, `ovstage_release_hierarchy`, `ovstage_release_hierarchy_result`, `ovstage_get_hierarchy_computation_models`, `ovstage_get_usd_stage_id`, `ovstage_population_*`, `ovstage_instancing_*`, `ovstage_clone` and `ovstage_fetch_clone_result`. The functions of the hierarchy, usd, population and instancing families, `ovstage_clone` and `ovstage_fetch_clone_result` became `static inline` wrappers with no exported symbol; call them through the headers. That includes the 0.3 exports `ovstage_instancing_get_instance_roots`, `ovstage_instancing_get_prototype_root` and `ovstage_instancing_get_prototype_roots`. Keep lookups only of the functions exported in 0.4: `ovstage_population_get_last_error`, `ovstage_population_get_last_op_error`, `ovstage_population_register_usd_schemas`, `ovstage_population_export_desc_init` and `ovstage_population_export_desc_init_snapshot`, plus the new `ovstage_instancing_get_instance_roots_batch` and `ovstage_instancing_resolve_paths` (not in 0.3).
    - C++ wrapper calls whose first argument is a literal `NULL` or `nullptr`, and C++ uses of a wrapper's address (`&ovstage_wait_op`, `auto f = &ovstage_wait_op`, a wrapper passed to a template). C++ overloads each wrapper for the instance and for its pair type, so these are ambiguous: pass a typed null, give the address an explicit function-pointer type or cast, wrap the call in a lambda, or use the `_ex` form.
    - Code that defines, forwards or interposes `ovstage_data_plane_vtable_t`, or builds an `ovstage_instance_t` (mocks, interposers, alternate implementations): rename the `release_group` slot to `release_groups`, adopt the new `fetch_read_next` slot signature (output array, capacity, count), append the new `clone` and `fetch_clone_result` slots after `query_extension`, and fill `ext_instances` with one vtable and context per family, including `lifecycle`, as `ovstage_ext_vtables.h` describes. Rebuild providers and consumers together.
5. **Population struct fields (C/C++).** `ovstage_population_selector_t` gains `derived_properties` and `derived_property_count`, which population validates. Grep every selector declaration, including the entries of `ovstage_population_desc_t.selectors`, and any foreign-function layout of the selector or description. A selector set member by member without an initializer still compiles on 0.4 but leaves both fields indeterminate, so the description can be rejected or an unintended pointer followed. Zero-initialize it (`= {0}` in C; `{}` in C++, where `{0}` does not convert to the predicate's leading enum) or set `derived_properties` to NULL and `derived_property_count` to 0. `ovstage_population_prim_predicate_t` grows (`targeted_by` in its `values` union, `value_count` moves), and `ovstage_population_selector_t` and `ovstage_population_export_desc_t` embed it by value. Rebuild, do not hard-code these sizes or offsets in C or foreign-function layouts, and initialize export descriptors with `ovstage_population_export_desc_init` or `ovstage_population_export_desc_init_snapshot`.

**Typed identities**

6. **Typed handles and ids.** Operation ids and handles (`ovstage_op_id_t`, `ovstage_query_handle_t`, `ovstage_read_handle_t`, `ovstage_map_handle_t`, `ovstage_read_group_id_t`, the hierarchy, population and result ids) are distinct structs with `.raw_id`. Grep integer variables, arrays, casts, implicit conversions and integer comparisons that hold or test them. Keep them in their typed form; wrap a raw id explicitly only when importing one (`ovstage_op_id_t id = {raw};` in C). Python uses `OpId`, `QueryHandle`, `ReadHandle`, `MapHandle`, `ReadGroupId`, `PopulationOpId` and the other handle classes; passing an integer raises `TypeError`, but comparing with one does not: a handle is never equal to an integer, so `op_id == 0` silently becomes `False`. Grep Python `== 0`, `!= 0`, `if handle` and `not handle` on handles, and compare with the `ovstage.INVALID_*` constants (`INVALID_OP_ID`, `INVALID_QUERY_HANDLE`, …) or typed values; `bool(handle)` is true when the handle is not its kind's invalid value. Use `.raw_id` only where an integer is required (diagnostics, numeric storage or interchange, C comparisons). C++ value initialization (`Handle{}`) gives the invalid value.
7. **Typed tokens and paths.** `ovx_token_t`, `ovx_primpath_t` and `ovx_primpath_list_t` are structs with `raw_id`. Grep arithmetic, `==` with integers, integer containers and implicit conversions on them. In C, compare `.raw_id`; in C++ use same-kind `==`, `<` and `std::hash`. Python `PathDictionary` returns and requires `Token`, `PrimPath` and `PrimPathList`; write `handle.raw_id` into NumPy `uint64` tensors and wrap readback (`PrimPath(value)`). No Python handle supports `int(handle)`; `Token`, `PrimPath` and `PrimPathList` also do not support ordering (resource and operation ids support same-kind `<`). A Python `Token` compared with an integer is never equal.
8. **`OVX_INVALID_TOKEN` and empty strings.** Grep `OVX_INVALID_TOKEN`, `token == 0`, `!token`, Python `not token` / `if token`, and `ovx_token_or_string_t`. The invalid sentinel is now `UINT64_MAX`; token `0` is the empty string, and so is a zero-initialized token (`ovx_token_t{}` in C++, `{0}` in C, Python `Token()`). Replace tests of `0` as "no token" with `OVX_INVALID_TOKEN`; in Python, `bool(token)` is false only for the invalid token, so `not token` tests for it. In `ovx_token_or_string_t`, a non-null `string.ptr` takes precedence over `token`, even for an empty or stale but valid string, so the call can address a different attribute. Check every `ovx_token_or_string_t` built for a token, not only dangling pointers, and set `string.ptr` to NULL in each. Remove token `0` and `OVX_INVALID_TOKEN` from query attribute lists; `ovstage_query` now rejects them.

**Ordinals and reads**

9. **Signed ordinals.** `ovstage_ordinal_t` is `int64_t`. Grep `UINT64_MAX`, `2**64`, `(1 << 64)`, and `0xFFFFFFFFFFFFFFFF` where they name an ordinal, and replace a current-value read end with `OVSTAGE_ORDINAL_MAX` (Python `ovstage.ORDINAL_MAX`). Change variables, struct fields and arithmetic that hold ordinals from `uint64_t` to `ovstage_ordinal_t`. Ordinal `0` and negative ordinals above `OVSTAGE_ORDINAL_MIN` are now writable; only `OVSTAGE_ORDINAL_MIN` is sealed. Remove code that treated `0` as "never written" or as a sealed floor.
10. **Multi-column read groups.** Grep `ovstage_fetch_read_next` and `ovstage_release_group`, and every use on a read group of the members that moved to its columns: `attribute`, `ordinal`, `is_delete`, `is_array`, `semantic`, `meta`, `data` (C), and in Python also `tensor_count`, `data_count`, `has_data_index_map`, `data_row_index`, `tensor`, `array` and `dlpack`. What stays on the group: in C, `prims` and `read_group_id`; in Python, `prim_list`, `prim_offset`, `prim_count`, `has_prim_index_map`, `prim_index`, `release` and the context manager.
    - C: `ovstage_fetch_read_next(instance, read, timeout, out_groups, capacity, &out_count)` fills an array; `capacity` must be positive. `OVSTAGE_OK` delivers 1 to `capacity` groups and only the first `out_count` slots are valid; a short batch does not mean the read is complete. Keep fetching while the status is `OVSTAGE_OK`, processing each batch. `OVSTAGE_ERROR_END_OF_ITERATION` is normal completion; any other status (`OVSTAGE_ERROR_TIMEOUT`, or the read's error, which can follow an OK batch and then repeats on every fetch) is a failure. Each group has `prims` and `columns[0..column_count)`; read the moved members from a column. Release with `ovstage_release_groups(instance, groups, count)`; the singular C `ovstage_release_group` is gone (Python `Stage.release_group` and `ReadGroup.release` remain).
    - Python: `group.columns[i]` is a `ReadColumn` that carries the moved members; accessing them on `ReadGroup` raises `AttributeError`. A `ReadColumn` holds only a weak reference to its group, and `array()` and `dlpack()` return views borrowed from the group. Keep the `ReadGroup` referenced and unreleased (no `release()`, no exit from its `with` block) while its columns or views are in use, and do not replace stored groups with stored columns: `reader = read.fetch_next().array` becomes `group = read.fetch_next()` and `reader = group.columns[0].array` with `group` kept. A `ManagedDLTensor` from `dlpack()` keeps the group referenced, but releasing the group still invalidates it; copy data that must outlive the release. `Read.fetch_next()` keeps its single-group behavior.
    - A group can now hold several requested attributes. Requested tokens are deduplicated, and an attribute absent from every row is omitted, not filled with defaults, so column positions need not match the request. `group.columns[0]` is the attribute only when the read requests one attribute; for several, iterate `group.columns` and match `column.attribute`.

**Populated data**

11. **`PHYSICS` and `ALL` removed.** Grep `OVSTAGE_POPULATION_DOMAIN_PHYSICS`, `OVSTAGE_POPULATION_DOMAIN_ALL`, `PopulationDomain.PHYSICS`, and `PopulationDomain.ALL`. Replace `PHYSICS` with the population description the physics library provides, passed through an `open_usd_*_with_desc` or `repopulate_usd_with_desc` call. Replace `ALL` with that description plus `RENDERING` in the `domains` of a description passed in the same call (`_with_desc` takes no separate domains argument). A raw former `PHYSICS` bit is ignored and populates nothing; do not keep it.
12. **Instance roots and `PointInstancer.prototypes`.** Grep `XformInstance`, `ScopeInstance`, `GeomInstanceAPI`, `GeomInstanceRootAPI`, `usd-metadata:typeName`, and `prototypes:usd-targets`.
    - An instance root's `usd-prim-type` is its authored type in every domain, and typed export uses it too. Find instance roots with the instancing queries (`ovstage_instancing_*`), not by type or schema tag.
    - Without `RENDERING`, instance roots no longer carry `GeomInstanceAPI` or `GeomInstanceRootAPI` in `usd-schemas`, and `usd-ancestor-types` lists the authored type's ancestors.
    - Instance roots no longer carry `usd-metadata:typeName` unless a description selects that metadata path.
    - `prototypes` holds the authored targets in authored order, which `protoIndices` indexes, in every domain; `prototypes:usd-targets` is gone. Read `prototypes` instead.
    - Grep reads of `RenderProduct.camera` / `RenderSettings.camera` under `RENDERING`: targets of any type are now kept in authored order, including unresolved ones (property paths stay excluded); filter by type.
    - Grep reads of `extent` on `ParticleField` / `ParticleField3DGaussianSplat` prims: it is one `{kDLFloat, 64, 6}` value, also where the particle field schema is not registered.

**Behavior changes**

13. **Reserved `*` in attribute names.** Grep attribute names that contain `*` used in writes, maps, deletes, query filter predicates and attribute lists, `advance_write_floor` with `SCOPE_INCLUDE` or `SCOPE_EXCLUDE`, or `get_attribute_write_floor`. They now fail with `OVSTAGE_ERROR_INVALID_ARGUMENT`; rename the attribute. In reads, a single trailing `*` (`*` or `prefix*`) is a selector pattern; any other `*` in a read selector fails with `OVSTAGE_ERROR_INVALID_ARGUMENT`.
14. **`usd-path` range reads, only if used.** A range read of `usd-path` now returns a tombstone group for prims deleted whole in the range, and fails with `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` unless the `usd-path` write floor is at or above the range end. Advance the floor with `SCOPE_ALL`, or `SCOPE_INCLUDE` that lists `usd-path`, before the read. Prim creation and deletion history can raise `ovstage_get_oldest_preserved_ordinal`; re-query the frontier rather than caching it.
15. **Retained reads and the scene hierarchy.** Grep for fetched read groups (C `ovstage_read_group_t` not yet passed to `ovstage_release_groups`; Python `ReadGroup` objects kept across calls) that are still held when the code writes, maps, clones or deletes. Prims are now reconnected under late-created parents, deletes reattach surviving children, and parents and ancestors can gain `omni:fabric:worldMatrix`; that relocates their storage, so a write, map, clone or delete fails with `OVSTAGE_ERROR_OP_FAILED` when a held group covers such a prim, even one that is not a target: the destination parent or one of its ancestors, a prim with a transform input between a target and a descendant that carries the attribute, or an existing descendant a clone merges into. Cloned prims also now appear under their parents in `ovstage_get_hierarchy()` answers, where 0.3 reported no parent and no children for them.
    - Release groups before structural operations where possible. Prefer creating destination parents before cloning.
    - Where `OP_FAILED` is handled, read the diagnostic first: the code has other causes. If a held group is the cause, release it before trying again; releasing it does not undo or complete the failed operation.
    - Writes: entries that completed before the failure stay applied, and the API does not report which. Treat the batch as possibly partial, do not infer completion from query visibility, and prefer repairing with a newer ordinal. A same-ordinal retry is safe when no entry completed; under `STRICTLY_INCREASING`, replaying completed cells at their recorded ordinal is rejected, while the other admission policies may admit it. Finish the repair before advancing the watermark, following the write-recovery section of [`../../docs/concepts/error_handling.rst`](../../docs/concepts/error_handling.rst).
    - Map commits: a failed commit is not undone, so some of its changes can remain. After an accepted group commit fails, the session is terminal and its groups cannot be committed again, even after the read group is released. Call `unmap_attribute` to end it, then repair with a new map session or a write.
    - A population step in that situation succeeds and defers the connection to the next population step or `ovstage_compute_hierarchy()` after release, or to the next write, map or clone that targets the parent and adds attributes or maps its transform input.
    - Report any code that asserts on `PARENT`, `CHILDREN`, `SIBLINGS`, `prims_cloned` or world transforms of clones and populated prims; their results can change (see the guide's Scene hierarchy section).
16. **Metadata maps, only if used.** Grep `map_attribute` / `ovstage_map_attribute` on `usd-prim-type`, `usd-schemas` or `usd-ancestor-types`. These maps now update the prim's tags; in 0.3 they could land in a value column no reader consulted. Make each host-resident and canonical: `dtype` omitted or `{kDLUInt, 64, 1}`, semantic `NONE`, `element_sizes` given for `usd-schemas` and `usd-ancestor-types` and omitted for `usd-prim-type`, and a query that names each prim once. A CUDA placement returns `OVSTAGE_ERROR_NOT_SUPPORTED`, even for an empty query that 0.3 completed as a no-op. A commit is refused while any attribute of a mapped prim has an outstanding read; release those groups before committing. A failed accepted commit ends the session; recover as step 15 describes for map commits.
17. **Validate.** Build, run the existing tests, and exercise open, query, read, write, map, and teardown. Do not add new tests unless asked.

## Audit Searches

| Pattern | Step |
|---|---|
| `ovstage_vtable_t`, `ovstage_context_t`, `ovx_string_or_token_t`, `ovstage_context_p` | 3 |
| `instance->vtable`, `instance->context`; `dlsym` / `GetProcAddress` of `ovstage_get_hierarchy`, `ovstage_compute_hierarchy`, `ovstage_fetch_hierarchy_result`, `ovstage_release_hierarchy`, `ovstage_get_usd_stage_id`, `ovstage_population_`, `ovstage_instancing_`, `ovstage_clone`, `ovstage_fetch_clone_result`; C++ wrapper calls with `NULL` / `nullptr` first; `&ovstage_` in C++; definitions or wrappers of `ovstage_data_plane_vtable_t`, `release_group` / `query_extension` slots, `ovstage_instance_t` initializers | 4 |
| `ovstage_population_selector_t` without an initializer, `ovstage_population_desc_t` selector arrays, hand-built `ovstage_population_prim_predicate_t` or `ovstage_population_export_desc_t`, `sizeof` / `offsetof` / FFI layouts of them | 5 |
| op ids and handles stored as integers, compared with integers (Python `== 0`, `!= 0`), tested for truth (`if handle`, `not handle`), converted implicitly or cast; `int(handle)` | 6 |
| tokens or paths in arithmetic, compared with integers, converted implicitly or held in integer containers; `int(token)`; ordering of Python `Token` / `PrimPath` | 7 |
| `OVX_INVALID_TOKEN`, `token == 0`, `!token`, Python `not token` / `if token`, zero-initialized tokens used as "no token", every `ovx_token_or_string_t` that carries a token | 8 |
| `UINT64_MAX`, `0xFFFFFFFFFFFFFFFF`, `2**64`, `(1 << 64)`, `uint64_t` ordinals, ordinal `0` as a sentinel | 9 |
| `ovstage_fetch_read_next`, `ovstage_release_group`; on a read group: `.attribute`, `.ordinal`, `.is_delete`, `.is_array`, `.semantic`, `.meta`, `.data`, `.tensor_count`, `.data_count`, `.has_data_index_map`, `.data_row_index`, `.tensor(`, `.array(`, `.dlpack(` | 10 |
| `DOMAIN_PHYSICS`, `DOMAIN_ALL`, `PopulationDomain.PHYSICS`, `PopulationDomain.ALL` | 11 |
| `XformInstance`, `ScopeInstance`, `GeomInstanceAPI`, `GeomInstanceRootAPI`, `usd-ancestor-types` on instance roots, `usd-metadata:typeName`, `prototypes:usd-targets`, `RenderProduct.camera`, `RenderSettings.camera`, `extent` on particle fields | 12 |
| attribute names containing `*`; read selectors with a `*` that is not the single last character | 13 |
| range reads of `usd-path`, cached `ovstage_get_oldest_preserved_ordinal` | 14 |
| read groups held across `write`, `map`, `clone` or `delete`; `OP_FAILED` handling; assertions on `PARENT`, `CHILDREN`, `SIBLINGS`, `prims_cloned`, `omni:fabric:worldMatrix` or other world transforms | 15 |
| `map_attribute` on `usd-prim-type`, `usd-schemas`, `usd-ancestor-types` | 16 |

## Output Format

Report, per step, the files changed and the search that justified each change, then the steps that did not apply. List any behavior change you accepted without an edit (for example, `usd-path` tombstones or instance-root types) so the user can confirm it is intended.

## Scripts

None. The audit searches above use Grep.

## Limitations

- Covers only changes that can require source, build, or data-handling edits. It does not cover bug fixes (except the populated-data and hierarchy changes in steps 12 and 15), performance changes, or optional 0.4 features such as `NOT_HAS`, attribute selector patterns, derived properties, the new prim and property predicates, `ovstage_instancing_resolve_paths`, `ovstage_instancing_get_instance_roots_batch`, the population thread limit, or strict resolution.
- The physics population description is provided by the physics library, not by ovstage; this skill does not author one.
- Live USD edits now republish only what a property change affects, and rewrite computed values on every property edit even when unchanged. Code that relied on a property add or remove republishing the whole subtree is not detected by a search; see the guide's Behavior changes.
- Process-level changes (OpenUSD environment variables, resolver-context capture at instance creation, the bundled Omniverse Client Library) need no source edit and are listed in the guide's Behavior changes.
- Applications on 0.2.x need `update-0-2-to-0-3` first.

## Troubleshooting

- Unknown type `ovstage_vtable_t`, `ovstage_context_t` or `ovx_string_or_token_t`: rename (step 3).
- No member `vtable` in `ovstage_instance_t`, or a symbol lookup of a hierarchy, usd, population, instancing or clone function that now fails: use `data_plane` or the inline wrappers (step 4).
- Compile error assigning an integer to an op id, handle, token or path: keep the typed value or wrap the raw id explicitly (steps 6 and 7).
- `TypeError` from a Python dictionary or stage call given an `int`: pass `Token`, `PrimPath`, `PrimPathList` or the handle class (steps 6 and 7).
- A query rejected with `INVALID_ARGUMENT` that worked on 0.3: remove token `0` from the attribute list (step 8), or a `*` name (step 13).
- Current-value reads return nothing or old data (in C, `UINT64_MAX` becomes ordinal `-1`), or Python raises `ValueError` for `2**64 - 1`: use `OVSTAGE_ORDINAL_MAX` (step 9).
- A Python comparison with `0` or an integer is always false, or `if token` behaves differently: compare with `ovstage.INVALID_*`, typed values, or use `bool(handle)` (steps 6 to 8).
- A vtable implementation or wrapper fails to compile, or calls land in the wrong slot: rename `release_group` to `release_groups`, adopt the new `fetch_read_next` signature, append `clone` and `fetch_clone_result`, fill `ext_instances` (step 4).
- `AttributeError` for `ReadGroup.array`, `.tensor`, `.meta`, `.attribute`, `.semantic` or another data member: read it from `group.columns[i]` (step 10).
- Physics attributes missing after open: the `PHYSICS` bit is ignored; pass the physics description (step 11).
- Instance roots no longer found by `XformInstance` or `GeomInstanceRootAPI`: use the instancing queries (step 12).
- `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` from a `usd-path` range read: advance its floor first (step 14).
- A population call rejects a selector, or behaves differently between runs, after a rebuild with no source change: zero-initialize the selector (step 5).
- Ambiguous overload in C++ for a wrapper called with `NULL` / `nullptr`, or for `&ovstage_wait_op` or another wrapper address: use a typed null, an explicit function-pointer type, a lambda, or the `_ex` form (step 4).
- A Python `ReadColumn` or one of its views reports its group released or collected: keep the `ReadGroup` referenced and unreleased while they are used (step 10).
- `OVSTAGE_ERROR_OP_FAILED` from a write, map, clone or delete that worked on 0.3: read the diagnostic; if a held read group is the cause, release it, repair a write batch (preferably with a newer ordinal), and replace a failed map session with a new one (step 15).
- A metadata map rejected with `NOT_SUPPORTED` or a refused commit: use a canonical host-resident map and release read groups on the mapped prims; start a new session after a failed commit (step 16).
- Read groups missing after migrating the fetch loop: a short batch is not the end; keep fetching while the status is `OVSTAGE_OK` and treat any final status other than `OVSTAGE_ERROR_END_OF_ITERATION` as a failure (step 10).
- Wrong values from `group.columns[0]` in a read of several attributes: match `column.attribute` (step 10).

## References

- [`../../CHANGELOG.md`](../../CHANGELOG.md), section `[0.4.0]`
- [`../../docs/guides/upgrading_from_0_3.rst`](../../docs/guides/upgrading_from_0_3.rst)
- [`../update-0-2-to-0-3/SKILL.md`](../update-0-2-to-0-3/SKILL.md)
- [`../path-dictionary/SKILL.md`](../path-dictionary/SKILL.md)
- [`../loading-usd/SKILL.md`](../loading-usd/SKILL.md)
- [`../string-handling/SKILL.md`](../string-handling/SKILL.md)
- [`../error-handling/SKILL.md`](../error-handling/SKILL.md)
