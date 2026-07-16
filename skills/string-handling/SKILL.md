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
name: string-handling
description: >
  Working with ovx_string_t and ovx_string_or_token_t in ovstage (C / C++). Use when
  user asks about passing or comparing strings, attribute names, or prim paths, or
  about choosing between a raw string and a pre-resolved path-dictionary token.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - c
  - strings
tools:
  - Read
  - Grep
---

# String Handling

## When to Use

Use this skill when the user asks about passing, printing, or comparing ovstage strings,
or about the dual-mode `ovx_string_or_token_t` arguments (attribute names, filter
attributes, prim paths) that accept either a raw string or a pre-resolved path-dictionary
token. Covers both the **C/C++** surface (`ovx_string_t` / `ovx_string_or_token_t`) and the
**Python** bindings (plain `str` vs. interned `int` token) — see the Python section.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- Target API surface: C/C++ (`ovx_string_t` / `ovx_string_or_token_t`) or Python (the
  `ovstage` bindings, where strings are plain `str` and tokens are interned `int`s).
- Source of the `ovx_string_t`: a literal/attribute name the caller is passing in, a
  prim path, or a string view returned from the path dictionary.
- Whether an argument is plain `ovx_string_t` or dual-mode `ovx_string_or_token_t`, and
  whether the caller already holds an interned `ovx_token_t`.
- Required operation: construct, print, compare, convert to `std::string_view`, or copy
  to owning storage.
- The shipped headers under `include/` (the ovstage public API and the bundled `ovx`
  headers it pulls in, e.g. `<ovx/string_types.h>`) are the authoritative contract — treat
  them as the source of truth over any snippet or this doc.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced example/snippets.
- Read the relevant shipped header (e.g. `<ovx/string_types.h>`) before writing or explaining
  API usage; the `> **Source:**` snippets are illustrations, not the contract.
- Treat `ovx_string_t` as a **non-owning** view: the caller owns the pointed-to memory,
  and the bytes are not required to be null-terminated.
- For dual-mode arguments, know whether the caller has a token already; if not, leave
  `token = 0` and the string is resolved through the dictionary at call time.

## Instructions

1. Identify whether the task is constructing, printing, comparing, or converting an
   `ovx_string_t`, or whether it concerns the dual-mode `ovx_string_or_token_t`.
2. Always use both the pointer and length (`ptr`/`length`); never
   assume a null terminator. `length` is bytes, not code points.
3. In C, print with a precision-limited format (`%.*s` with `(int)length, ptr`) and compare
   by checking `length` first, then `memcmp` over exactly `length` bytes (`strncmp` is
   unsafe — a shorter string prefix-matches).
4. In C++, prefer `std::string_view{ s.ptr, s.length }` for borrowed access, and copy
   (`std::string`) if the value must outlive the source buffer.
5. For `ovx_string_or_token_t`, set `token` (non-zero) for repeated/hot-path identity
   use to skip per-call string hashing; otherwise leave `token = 0` and populate
   `string`. See the `path-dictionary` skill for interning and the cost model.
6. When changing code, run the narrow ovstage unit test that exercises the referenced
   string pattern whenever practical.

## Output Format

- For explanations, cite the relevant API names, source snippets, and caveats.
- For code changes, summarize the files changed, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The shipped `include/` headers are the source of truth; keep snippets and this doc in sync
  with them (and add tested snippets) before documenting new API usage.
- **Most error strings are `ovx_string_t`.** `ovstage_get_last_op_error(instance, op_id)`
  and `ovstage_get_last_error()` return `ovx_string_t`, so this skill's view/print/lifetime
  guidance applies to both. The exception is `ovstage_get_error_string(instance, code)`,
  which returns a static `const char*` (never NULL). See the `error-handling` skill.
- **Python:** covered in the Python section (plain `str` ↔ interned `int` token; no
  `ovx_string_t` in Python). Inline Python pending a shipping Python example to snippet-source.

## Overview

ovstage strings use `ovx_string_t` — a non-owning `(ptr, length)` view defined in the
shipped header `<ovx/string_types.h>` (reachable transitively from `<ovstage/ovstage.h>`):

```c
typedef struct ovx_string_t {
    const char* ptr;  /* UTF-8 data; not owned, not necessarily null-terminated */
    size_t      length;  /* length in bytes */
} ovx_string_t;
```

The view is a flat `{ ptr, length }` struct. Null-termination is asymmetric: a
view you pass **in** is a length-prefixed view (`ptr[length]` need not be `'\0'`),
but a non-empty view the library **returns** (e.g. `get_last_error`,
`get_last_op_error`, path-dictionary lookups) is null-terminated, so its `ptr`
can go straight to libc C-string functions. A `{ NULL, 0 }` return means "no value".

Many ovstage entry points (attribute names, filter attributes, prim paths) take
`ovx_string_or_token_t` — a dual-mode value carrying either a pre-resolved
`ovx_token_t` or an `ovx_string_t`. If `token != 0` the token is used directly; if
`token == 0` the `string` is resolved through the path dictionary at call time.
Pre-resolving the token avoids per-call string hashing in hot loops.

## C

Construct from a C literal with the `literal_to_ovx_string(...)` helper macro from
`<ovx/types.h>` (the length is computed at compile time via `sizeof`, excluding the
terminating NUL):

```c
ovx_string_t name = literal_to_ovx_string("points");
```

The macro expands to C++ brace-init syntax (`ovx_string_t{ (str), sizeof(str) - 1 }`),
and the shipped examples compile as C++. In a pure-C translation unit, or for
non-literal bounded storage, use an explicit `{ ptr, length }` construction instead:

```c
ovx_string_t name = { "points", sizeof("points") - 1 };  /* literal, plain C */
ovx_string_t view = { buf, buf_len };                    /* non-literal bounded storage */
```

Do not wrap arbitrary `const char*` values in a `strlen`-based helper — pass
`ovx_string_t` through helper layers instead (repo convention; see `AGENTS.md`).

Print with the `%.*s` precision pattern so `length` controls how many bytes are read:

```c
printf("%.*s\n", (int)name.length, name.ptr);
```

Compare by checking `length` first, then `memcmp` over exactly `length` bytes — do
not rely on a null terminator, and avoid `strncmp` (a shorter string prefix-matches).

Build a dual-mode argument from a string (token left `0` so the string is resolved); set
`token` instead when you already hold an interned handle:

> **Source:** `examples/c/minimal/main.cpp` snippet `string-or-token-arg`

## C++

Wrap `ptr`/`length` in `std::string_view` for zero-copy access to the standard string API;
copy into a `std::string` if the value must outlive the source buffer:

```cpp
std::string_view view{ s.ptr, s.length };
```

> **Source:** `examples/c/minimal/main.cpp` snippet `string-view-from-ovx-string`

## Python

In Python there is **no `ovx_string_t`** — the binding handles encoding, so you pass and
receive plain `str`. The string-vs-token duality maps to **`str` vs. interned `int`
token**: API methods accept `Union[int, str]` for names/paths, and the `PathDictionary`
interns/resolves between them.

> **Source:** `examples/python/minimal/main.py` snippet `intern-and-resolve`
>
> Followed by: `examples/python/minimal/main.py` snippet `string-or-token-arg`

Both forms are accepted where the C API takes `ovx_string_or_token_t`: a pre-interned token
(cheaper — `minimal-write-read` writes with the token) or a raw `str`, interned each call
(`string-or-token-arg`).

Prefer the interned `int` token for repeated/hot-path use (skips per-call hashing); use a
raw `str` for one-shot calls. See the `path-dictionary` skill for interning details.

## Key Types / Functions

| Type | Header | Notes |
|------|--------|-------|
| `ovx_string_t` | `<ovx/string_types.h>` | non-owning `(ptr, length)`; `length` is bytes |
| `literal_to_ovx_string(str)` | `<ovx/types.h>` | creates `ovx_string_t` from a string literal (compile-time `sizeof` length) |
| `ovx_string_or_token_t` | `<ovx/string_types.h>` | dual-mode: `token != 0` uses token, else `string` |
| `ovx_token_t` | `<ovx/string_types.h>` | interned handle; `0` is the unresolved sentinel |

## Troubleshooting

- Do not pass an **input** `ovx_string_t::ptr` (a view you built, e.g. a substring)
  to functions that assume a null-terminated C string without accounting for
  `length` — it may not be null-terminated. Use `%.*s`, `memcmp`, or
  `std::string_view`. (Library-**returned** views are null-terminated; see above.)
- `length` is a **byte** count, not a code-point count — slicing at `length` is byte-exact
  but not character-aware.
- For `ovx_string_or_token_t`, a non-zero `token` takes precedence and the `string` field
  is ignored. Leave `token = 0` unless you intend to use an interned handle.
- A string view returned from the path dictionary is valid for the dictionary's lifetime;
  copy it if you need to outlive that.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `path-dictionary` skill — interning, token lifetime, and the dual-mode cost model.
- `error-handling` skill — error reporting (`get_error_string` is static `const char*`;
  `get_last_op_error` and `get_last_error` return `ovx_string_t`).
- Keep related skills, docs, and snippets synchronized when changing the workflow.
