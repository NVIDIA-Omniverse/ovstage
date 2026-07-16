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
name: path-dictionary
description: >
  Using the OVX path dictionary — the shared interning layer for tokens, prim paths, and
  prim-path lists across OV libraries. Use when user asks about interning strings/paths,
  ovx_token_t / ovx_primpath_t / ovx_primpath_list_t, or sharing paths with ovstage.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - ovx
  - path-dictionary
tools:
  - Read
  - Grep
---

# Path Dictionary

## When to Use

Use this skill when the user asks about interning strings or prim paths, working with
`ovx_token_t` / `ovx_primpath_t` / `ovx_primpath_list_t`, building a path list to query
ovstage, or sharing interned paths/tokens across OV libraries.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- Whether the caller needs a token (attribute/name interning), a prim path, or a prim-path
  list (to query/read a set of prims).
- The dictionary is owned by a producing subsystem; for ovstage, obtain it via
  `ovstage_get_path_dictionary` for zero-conversion sharing and never free it (there is no
  app-side create/destroy in the public API).
- Whether a path list was created by the caller (`path_dictionary_create_path_list_from_*`
  returns it with refcount=1 — pair it with one `path_dictionary_release_path_list_reference`)
  or borrowed from an ovstage read result (release only references you added, never ovstage's).
- Repository source snippets referenced below. Treat these snippets as the API source of truth.

## Prerequisites

- Use an ovstage checkout with the path-dictionary headers under `include/ovx/path_dictionary/`
  — `path_dictionary.h` (which pulls in `path_dictionary_types.h` and the
  `path_dictionary_utils.h` inline wrappers).
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Know the refcount rule for any `ovx_primpath_list_t` before releasing a reference to it.

## Instructions

1. Identify whether the task needs a token, a path, or a path list.
2. Obtain a dictionary from its owner: for ovstage, `ovstage_get_path_dictionary(instance)`
   returns a `path_dictionary_instance_t*` (do not free its `vtable`/`context`). All calls
   below are the inline wrappers from `path_dictionary_utils.h` and take that instance.
3. Intern strings with `path_dictionary_create_tokens_from_strings` (tokens) and
   `path_dictionary_create_paths_from_strings` / `..._create_paths_from_tokens` (prim paths).
   Resolve tokens back with `path_dictionary_get_strings_from_tokens`; decompose a prim path
   into its tokens with `path_dictionary_get_tokens_from_paths`, then resolve those tokens.
4. Build path lists with `path_dictionary_create_path_list_from_paths` (from interned paths) or
   `..._create_path_list_from_strings`; read them back with
   `path_dictionary_get_paths_from_path_list` / `..._get_num_paths_from_path_list`.
5. Path lists are refcounted: `path_dictionary_create_path_list_from_*` returns refcount=1
   owned by you, so pair it with exactly one `path_dictionary_release_path_list_reference`
   (which frees the list when the count reaches zero). To keep a list borrowed from an ovstage
   read result, call `path_dictionary_add_path_list_reference` first and release that added
   reference when done; never release ovstage's own reference.
6. Check every `ovx_api_result_t::status` against `OVX_API_SUCCESS`; on `OVX_API_ERROR` the
   `result.error` (an `ovx_string_t`) describes the failure and must be freed with
   `path_dictionary_release_error`.
7. When changing code, run the path-dictionary unit test whenever practical.

## Output Format

- For explanations, cite the relevant API names, source snippets, and caveats.
- For code changes, summarize the files changed, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before documenting new API usage.
- **Python:** covered in the Python section (`ovstage.PathDictionary`; `int` handles,
  `str` names, `OvxError`). Inline Python pending a shipping Python example to snippet-source.
- **Snippets** are sourced from the shipping example `examples/c/minimal/main.cpp`.

## Overview

The OVX path dictionary (`<ovx/path_dictionary/path_dictionary.h>`) is the **shared,
zero-cost interning layer** for OV libraries (ovstage, ovrtx, ovphysx, …). The public
surface is a vtable (`path_dictionary_vtable_t`) plus inline wrappers in
`path_dictionary_utils.h`; an instance (`path_dictionary_instance_t`) is produced by an
owner subsystem (e.g. ovstage) and maps strings to stable, trivially comparable handles:

- `ovx_token_t` — interned string (e.g. an attribute name). `OVX_INVALID_TOKEN` is `0`.
- `ovx_primpath_t` — interned prim path. `OVX_INVALID_PRIMPATH` is `0`.
- `ovx_primpath_list_t` — an immutable set/ordering of prim paths. `OVX_INVALID_PRIMPATH_LIST` is `0`.

Design guarantees:

- **Handle identity:** same string → same handle, so consumers use handle equality for
  O(1) identity checks (no string compare).
- **Immutable lists:** once created, an `ovx_primpath_list_t` never changes; equal handles
  imply identical prim set + ordering (usable as a cache key).
- **Thread-safe interning:** concurrent interning is supported; returned handles and
  string pointers are stable for the dictionary's lifetime.

This is the type backbone for `ovx_string_or_token_t` dual-mode arguments throughout
ovstage — see the `string-handling` skill.

## Ownership & lifetime

- The dictionary is **owned by its producing subsystem** (for ovstage, obtained via
  `ovstage_get_path_dictionary`). Callers MUST NOT free its `vtable`/`context`; when the
  owner reports it gone, every handle minted through it is invalidated simultaneously.
- Tokens and prim paths are **dict-lifetime** (interned, never freed individually); they stay
  valid for as long as the dictionary lives.
- String pointers from `path_dictionary_get_strings_from_tokens` point into dictionary-owned
  storage and are valid for the dictionary's lifetime — do not free them; copy if you need
  them longer. (Error strings inside `ovx_api_result_t` are different — free those with
  `path_dictionary_release_error`.)
- `ovx_primpath_list_t` handles are **explicitly refcounted**.
  `path_dictionary_create_path_list_from_*` returns a list with **refcount=1** owned by the
  caller; `path_dictionary_add_path_list_reference` increments and
  `path_dictionary_release_path_list_reference` decrements, freeing the list at zero. Every
  `create`/`add` must be paired with exactly one release.
- A list **returned by an ovstage read result** is a **borrow** held by the producing op. Do
  not release ovstage's reference; to keep the list beyond the producing handle, call
  `path_dictionary_add_path_list_reference` first and release that added reference when finished.

## C — intern and resolve

Obtain a dictionary, intern an attribute token and prim paths, build a path list, and
resolve back to strings:

> **Source:** `examples/c/minimal/main.cpp` snippet `intern-and-resolve`

## C — build a path list and query ovstage

The basic stage test interns paths into a list and passes it to
`ovstage_query_from_path_list`, then uses the interned attribute token as the
`ovx_string_or_token_t` argument:

> **Source:** `examples/c/minimal/main.cpp` snippet `path-list-query`

## Sharing with ovstage

For zero-conversion sharing, obtain ovstage's own dictionary instead of creating a
separate one, so tokens/paths interned by the application and by ovstage are directly
comparable:

```c
/* path_dictionary_instance_t* dict = ovstage_get_path_dictionary(instance);
   returns ovstage's dictionary (NULL if instance is NULL); valid for the
   instance lifetime. Do not free dict->vtable / dict->context. */
```

## Python

The path dictionary is `ovstage.PathDictionary` — a context manager wrapping the same
interning service. Tokens/paths/lists are Python `int` handles; strings are `str`. Errors
raise `ovstage.OvxError`.

> **Source:** `examples/python/minimal/main.py` snippet `intern-and-resolve`
>
> Followed by: `examples/python/minimal/main.py` snippet `path-list-query`

Resolve a path list back to strings with `paths.get_path_strings(list)`, and release a
caller-created list with `paths.destroy_path_list(list)` (path lists are refcounted).

Construct standalone (`PathDictionary()`) or bind to a stage's shared dictionary via
`PathDictionary(stage)` / `stage.get_path_dictionary()` for zero-conversion sharing. Same
ownership rule as C: destroy **caller-created** lists; do not destroy lists handed back by
an ovstage read result.

## Key Types / Functions

All operations are inline wrappers from `path_dictionary_utils.h` taking a
`path_dictionary_instance_t*` (except `ovstage_get_path_dictionary`, which produces one).

| Symbol | Role |
|--------|------|
| `ovstage_get_path_dictionary` | obtain ovstage's `path_dictionary_instance_t*` (owner-owned; do not free) |
| `path_dictionary_create_tokens_from_strings` / `..._get_strings_from_tokens` | string ↔ `ovx_token_t` |
| `path_dictionary_create_paths_from_strings` / `..._create_paths_from_tokens` | build `ovx_primpath_t` (dict-lifetime) |
| `path_dictionary_get_tokens_from_paths` | decompose a `ovx_primpath_t` into its tokens |
| `path_dictionary_create_path_list_from_paths` / `..._create_path_list_from_strings` | build an `ovx_primpath_list_t`, returned with **refcount=1** owned by the caller |
| `path_dictionary_get_paths_from_path_list` / `..._get_num_paths_from_path_list` | read a list back |
| `path_dictionary_add_path_list_reference` / `..._release_path_list_reference` | increment / decrement a list's refcount; release frees the list at zero — pair every `create`/`add` with exactly one release (replaces the legacy `destroy_path_list` slot) |
| `path_dictionary_release_error` | free the `ovx_string_t` error attached to an `ovx_api_result_t` |

Every slot returns `ovx_api_result_t { ovx_api_status_t status; ovx_string_t error; }`, where
`ovx_api_status_t` is `OVX_API_SUCCESS` (0) or `OVX_API_ERROR` (1). Handle sentinels
`OVX_INVALID_TOKEN` / `OVX_INVALID_PRIMPATH` / `OVX_INVALID_PRIMPATH_LIST` are all `0` and are
never returned on success.

## Troubleshooting

- A successful mint never returns `0` — treat `OVX_INVALID_TOKEN` /
  `OVX_INVALID_PRIMPATH` / `OVX_INVALID_PRIMPATH_LIST` (all `0`) as "unset/invalid".
- `create_*` slots intern on miss (create-on-miss), so there is no separate lookup call;
  empty token/path strings are rejected. Both path-list constructors accept zero paths and mint
  a valid empty list; their input array may be null when the count is zero.
  `get_*` slots require a live
  handle — calling one on a released/unknown path list returns `OVX_API_ERROR`.
- Pair every `path_dictionary_create_path_list_from_*` / `..._add_path_list_reference` with
  exactly one `..._release_path_list_reference`; the list is freed when its refcount reaches
  zero. `OVX_INVALID_PRIMPATH_LIST` is a no-op on add/release. Do not release ovstage's
  reference on a list that came from a read result (it is a borrow), and do not release an
  unknown or already-freed handle — those calls return `OVX_API_ERROR`.
- String pointers from the dictionary are borrowed; copy before the dictionary is
  destroyed.
- For passing a name as either a token or a raw string, see `ovx_string_or_token_t` in the
  `string-handling` skill (set `token` for hot paths, else `string`).

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `string-handling` skill — `ovx_string_t` / `ovx_string_or_token_t` dual-mode usage.
- `error-handling` skill — ovstage-level error handling (distinct `ovstage_api_status_t` codes).
- Keep related skills, docs, and snippets synchronized when changing the workflow.
