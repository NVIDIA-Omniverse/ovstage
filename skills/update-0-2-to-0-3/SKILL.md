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
name: update-0-2-to-0-3
description: >
  Upgrade an existing C, C++, or Python application from ovstage 0.2.x to 0.3.x. Use when asked to upgrade ovstage, migrate from 0.2, port from 0.2 to 0.3 ("0.2 to 0.3"), or fix code that broke after moving to ovstage 0.3: PathDictionary(stage), time_code to time, connection writes, untyped prim type, ovalign.h, or open-USD merge behavior.
license: LicenseRef-NvidiaProprietary
version: "0.3.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - upgrade
  - "0.3"
  - migration
tools:
  - Read
  - Grep
  - Edit
---

# Update ovstage 0.2 to 0.3

Port an existing application with the minimum changes the 0.3 public contract requires. Do not adopt unrelated 0.3 features or rewrite working code.

## When to Use

Use this skill when upgrading C, C++, or Python code from ovstage 0.2.x to 0.3.x, or when code that worked on 0.2 fails to compile, raises `TypeError`, or returns different results on 0.3.

Do not use it for applications on 0.1.x or earlier; apply the earlier migration first. For migrating `pxr` USD code to ovstage, use `usd-to-ovstage-migration`.

## Inputs

Resolve these before editing:

- Source and target ovstage versions, and where the package is pinned or vendored.
- Languages in scope: C, C++, Python, or mixed.
- Whether the application opens USD more than once on one instance.
- Whether it writes connection attributes, reads `UsdRenderVar.sourceName` from selector-only stages, or counts prims in an unfiltered query.
- Files in scope: sources, build files, and tests.

## Prerequisites

- Access to the application's source, package pins, and build configuration.
- An ovstage 0.3.x package (headers, libraries, or the Python wheel).
- Read the `[0.3]` section of [`../../CHANGELOG.md`](../../CHANGELOG.md) and the page [`../../docs/guides/upgrading_from_0_2.rst`](../../docs/guides/upgrading_from_0_2.rst) before changing code. The installed 0.3 headers and Python signatures override both if they differ.

## Instructions

1. **Pin 0.3.** Update the package pin or vendored copy. Rebuild every translation unit that includes ovstage headers; do not reuse 0.2 objects.
2. **Run the audit searches below before editing.** Apply a change only where a search matches. Skip items that do not match.
3. **Python `PathDictionary`.** Grep `PathDictionary\(\)`, `PathDictionary\(None\)`, and `PathDictionary\(stage=None`. Replace with `PathDictionary(stage)`, passing the `Stage` that consumes the lists. Create or reuse a `Stage` and keep it alive; no `Stage` is created automatically.
4. **`time_code` to `time`.** Grep `time_code`. For each call to `open_usd`, `open_usd_from_string`, `open_usd_with_desc`, `open_usd_from_string_with_desc`, `update_from_usd_time`, or an `_async` form that passes `time_code=`, rename the keyword to `time=`. Positional calls need no change.
5. **Per-instance path dictionaries.** Grep `ovstage_get_path_dictionary` and path-list variables that cross instances. A list belongs to the instance whose dictionary built it; rebuild it in the destination instance's dictionary. Release list references before destroying the owning instance.
6. **Connection writes.** Grep for writes of connection attributes. Each write must supply exactly one `(target path id, target attribute token id)` pair per target prim in a shared tensor, or exactly one pair in each per-prim tensor. Remove code that relied on one pair applying to many prims.
7. **Untyped prim type.** Grep `untyped_type_name` and `__ovstage_population_untyped__`. Remove the call or comparison; an untyped USD prim now has no `usd-prim-type`, so test for the attribute's absence.
8. **`ovalign.h`.** Grep `ovalign`. Remove the include and any `ovalign_align` or `ovalign_release` calls; they never linked.
9. **Open-USD merge.** Grep repeated `open_usd` / `ovstage_population_open_usd*` on one instance. Opening no longer clears existing content. If the code relied on replacement, call `reset_usd` (`ovstage_population_reset_usd`) then `apply_usd_changes` at an ordinal below the open. If merge is acceptable, make no change.
10. **Error codes and waits.** Grep `ovstage_population_wait_op` and `ovstage_population_export_wait_op`. A stale or foreign op id now returns `OVSTAGE_ERROR_INVALID_HANDLE`, not `OVSTAGE_ERROR_INVALID_ARGUMENT`; update any branch on the old code. In Python, `wait()` now returns the handle; code that used it as a statement needs no change.
11. **Data-contract checks, only if directly consumed.** Reading `UsdRenderVar.sourceName` from a selector-only stage now yields raw UTF-8 bytes with `OVSTAGE_SEMANTIC_STRING`. `HAS` on `usd-schemas` now matches every populated prim; use `CONTAINS`. A stage without the `RENDERING` domain counts two fewer prims in an unfiltered query. Never compare a time with `==` against `OVSTAGE_POPULATION_DEFAULT_TIME` or `population.DEFAULT_TIME`; use `ovstage_population_is_default_time` / `population.is_default_time`.
12. **Configuration keys.** If the code passes custom keys to `ovstage_initialize`, remove unknown keys; 0.3 rejects them.
13. **Validate.** Build, run the existing tests, and exercise open, query, read, write, and teardown. Do not add new tests unless asked.

## Audit Searches

| Pattern | Step |
|---|---|
| `PathDictionary()`, `PathDictionary(None)`, `PathDictionary(stage=None` | 3 |
| `time_code` | 4 |
| `ovstage_get_path_dictionary`, path lists shared across instances | 5 |
| connection attribute writes to more than one prim | 6 |
| `untyped_type_name`, `__ovstage_population_untyped__` | 7 |
| `ovalign` | 8 |
| repeated `open_usd` on one instance | 9 |
| `ovstage_population_wait_op`, `ovstage_population_export_wait_op` | 10 |
| `sourceName`, `HAS` on `usd-schemas`, unfiltered prim counts | 11 |
| `ovstage_initialize` with custom config keys | 12 |

## Output Format

Report, per step, the files changed and the search that justified each change, then the steps that did not apply. List any behavior change you accepted without an edit (for example, merge-on-open) so the user can confirm it is intended.

## Scripts

None. The audit searches above use Grep.

## Limitations

- Covers only changes that can require source, build, or data-handling edits. It does not cover bug fixes, performance changes, or optional 0.3 features such as `get_build_info`, `Stage.config`, clone results, or identifier-addressed export.
- Selector-only `sourceName` layout and connection write counts come from the changelog and are not exercised by a runnable check here.
- Applications on 0.1.x need the earlier migration first.

## Troubleshooting

- `TypeError` from `PathDictionary(...)`: pass a `Stage` (step 3).
- `TypeError` mentioning `time_code`: rename the keyword to `time` (step 4).
- A connection write rejected with `INVALID_ARGUMENT` during submission or `OP_FAILED` from Python `wait()`: check the diagnostic and supply one pair per target prim (step 6). Python reports failures discovered during execution as `OP_FAILED`; always wait after a successful enqueue.
- Content from an earlier open still present after a second open: opens merge (step 9).
- Link error on `ovalign_*` or a missing `ovalign.h`: remove it (step 8).
- Unexpected `OVSTAGE_ERROR_INVALID_HANDLE` from a wait: the op id is stale, already consumed, or from another instance (step 10).

## References

- [`../../CHANGELOG.md`](../../CHANGELOG.md), section `[0.3]`
- [`../../docs/guides/upgrading_from_0_2.rst`](../../docs/guides/upgrading_from_0_2.rst)
- [`../path-dictionary/SKILL.md`](../path-dictionary/SKILL.md)
- [`../loading-usd/SKILL.md`](../loading-usd/SKILL.md)
- [`../error-handling/SKILL.md`](../error-handling/SKILL.md)
