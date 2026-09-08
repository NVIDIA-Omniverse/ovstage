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
name: project-setup-c
description: >
  Setting up a C/C++ consumer of ovstage. Use when user asks to create a new ovstage
  consumer (test, benchmark, plugin, or app), declare an ovstage dependency, include the
  headers, or build code that links ovstage.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - c
  - setup
tools:
  - Read
  - Grep
  - Bash
---

# Project Setup (C)

## When to Use

Use this skill when the user asks to create a new C/C++ consumer of ovstage, declare an
ovstage dependency, include the ovstage headers, or build/link code against ovstage.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- The consuming target type: unit test, benchmark, plugin, or application.
- Whether the consumer also interns tokens / builds path lists (the path dictionary,
  obtained from the instance via `ovstage_get_path_dictionary`) and/or exchanges tensor
  data (DLPack, `<dlpack/...>`). Both come with `<ovstage/ovstage.h>`.
- How the consumer's build system adds an include directory and a link dependency.
- Repository source snippets referenced below. Treat these snippets as the API source of truth.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced example.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Be able to add an include directory and a link dependency in the consumer's build system.

## Instructions

1. Include `<ovstage/ovstage.h>` for the data-plane API, `ovstage_get_path_dictionary`, and
   the path-dictionary/DLPack **types**. To *call* the path-dictionary functions, also include
   `<ovx/path_dictionary/path_dictionary.h>` and `<ovx/path_dictionary/path_dictionary_utils.h>`
   (the inline wrappers) directly, as the minimal example does; `<dlpack/dlpack.h>` covers the
   tensor types.
2. Add ovstage's `include/` to the consumer's include path and link the ovstage library.
3. Follow the minimal example's lifecycle (see Minimal Usage) for the create → query →
   write → advance → read → release ordering.
4. Build and run the narrow target; see the `error-handling` skill for status checking.


## Output Format

- For explanations, cite the relevant API names, source snippets, and caveats.
- For code changes, summarize the files changed, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before documenting new API usage.
- **Pre-release.** The ovstage package and its CMake config ship pre-release; pins and
  packaging details may change between releases.
- **C only.** For Python project setup see `project-setup-python`.
- **⚠️ Draft — API in flux.** The ovstage C API is still changing materially; treat this
  setup (especially exact symbol/usage in the minimal example) as provisional.
- **Snippets** are sourced from the shipping example `examples/c/minimal/main.cpp`.

## Overview

ovstage is a pure **C API**:

- **Public headers:** `include/` — `<ovstage/ovstage.h>` (data plane +
  path-dictionary/DLPack *types*), plus `<ovx/path_dictionary/path_dictionary.h>`
  (+ `path_dictionary_utils.h`) to call the path-dictionary functions, and
  `<dlpack/dlpack.h>` for tensor types.
- **Library:** consumers link a public ovstage loader and include the public headers.
  The released package ships a CMake config (`lib/cmake/ovstage/`), so a standalone
  consumer uses `find_package(ovstage REQUIRED)` + `target_link_libraries(<target>
  PRIVATE ovstage::ovstage)`.

## Dependency Declaration (CMake, standalone)

Model a new standalone consumer on the shipping examples: each example's
`CMakeLists.txt` (e.g. `examples/c/minimal/CMakeLists.txt`) includes the shared
module `examples/c/cmake/ovstage.cmake` and calls `ovstage_fetch()` — which tries
`find_package(ovstage QUIET)` first and otherwise downloads the pinned released
package zip — then:

```cmake
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE ovstage::ovstage)
ovstage_setup_runtime(myapp)
```

`ovstage_setup_runtime()` sets an rpath onto the package `bin/` on Linux; on
Windows put the package `bin/` on `PATH` at runtime. `ovstage::ovstage` is the
shared forwarding loader; it opens the colocated runtime on the first
initialize/create call. Keep the package `bin/` layout intact so the runtime
remains beside its `plugins/` closure.

To consume a manually downloaded package without the examples' fetch module, see
"Standalone build and run (published package)" below.


## Headers

| Header | Purpose |
|--------|---------|
| `<ovstage/ovstage.h>` | **Include this.** Declares instance lifecycle (`ovstage_create_instance` / `ovstage_destroy_instance`) and transitively pulls the whole `ovstage_api/` surface below plus the path-dictionary + DLPack **types**. |
| ↳ `ovstage_api/ovstage_api_utils.h` | The flat inline `ovstage_*` **wrappers you call** — `ovstage_get_path_dictionary`, `ovstage_query_from_path_list`, `ovstage_write_attribute`, `ovstage_advance_write_floor`, `ovstage_read_attributes`, `ovstage_wait_op`, `ovstage_release_op`, … (reached via `ovstage.h`; no need to include directly). |
| ↳ `ovstage_api/ovstage_api.h` | The **vtable** those wrappers dispatch through, plus the execution-model reference. Reached via `ovstage.h`. |
| ↳ `ovstage_api/ovstage_api_types.h` | Structs / enums — `ovstage_enqueue_result_t`, `ovstage_op_wait_result_t`, status codes, and the DLPack + path-dictionary **types**. |
| `<ovx/path_dictionary/path_dictionary.h>` (+ `path_dictionary_utils.h`) | The path-dictionary **functions** — `path_dictionary_*` interning, path lists, and the inline wrappers. **Not** pulled in transitively by `<ovstage/ovstage.h>`; include directly to call them (as the minimal example does). |
| `<dlpack/dlpack.h>` | `DLTensor` types for attribute data exchange (available transitively; the example includes it explicitly). |

## Minimal Usage

Create an instance, get its (instance-owned) path dictionary, intern tokens / build path
lists, then write/advance/read. The path dictionary is owned by the instance — there is no
app-side create/destroy; you obtain it with `ovstage_get_path_dictionary(instance)` (check
for a NULL return before use). The minimal example shows the end-to-end shape:

> **Source:** `examples/c/minimal/main.cpp` snippet `minimal-write-read`

The essential ordering is: `ovstage_create_instance` → `ovstage_get_path_dictionary` →
`path_dictionary_create_path_list_from_strings` / intern tokens →
`ovstage_query_from_path_list` → `ovstage_write_attribute` → `ovstage_advance_write_floor` →
`ovstage_read_attributes` / `ovstage_fetch_read_next` → release (release path-list
references via `path_dictionary_release_path_list_reference`, then destroy the instance).
See the `error-handling` skill for status checking and the `minimal-write-read` snippet
above for the full lifecycle.

The write declares attribute kind explicitly: the fixed-size write in `minimal-write-read`
sets `write.is_array = false`; array attributes set `write.is_array = true`. Attribute kind
is never inferred from tensor shape or count.


## Standalone build and run (published package)

An external C/C++ consumer builds against the **published ovstage package** — public headers,
the shared loader, runtime, and a CMake config — with no ovstage source checkout.
The config exports `ovstage::ovstage` (shared loader) and
`ovstage::ovstage_static` (static loader).

### Get the package

Download the ovstage package archive for your platform — `manylinux_2_35_x86_64`,
`manylinux_2_35_aarch64`, or `windows-x86_64` — from the repository releases page
(each release attaches the per-platform package zips), and unzip it (e.g. to
`ovstage-pkg/`).


The unzipped tree has `include/` (ovstage + `ovx` + `dlpack` headers), `bin/` (the shared
loader, runtime, and runtime closure), and `lib/cmake/ovstage/` (the CMake config). Pin the exact package version your
code targets — it tracks the build the API matches — the same way the Python example pins
its wheel.

### Build against it

Point `CMAKE_PREFIX_PATH` at the unzipped package and `find_package(ovstage)`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(ovstage_consumer CXX)
find_package(ovstage REQUIRED)                       # from lib/cmake/ovstage/ovstageConfig.cmake
add_executable(app main.cpp)
target_link_libraries(app PRIVATE ovstage::ovstage)  # include dir + library come with the target
```

```bash
cmake -B build -DCMAKE_PREFIX_PATH="$PWD/ovstage-pkg"
cmake --build build --config Release
# The shared library ships under the package's bin/; put it on the loader path at runtime:
LD_LIBRARY_PATH="$PWD/ovstage-pkg/bin" ./build/app   # Linux
# Windows: add ovstage-pkg\bin to PATH before running app.exe
```

No manual `-I`/`-l` is needed — the `ovstage::ovstage` target carries the include directory and
links the shared loader. Note the path-dictionary split: `<ovstage/ovstage.h>` pulls the
path-dictionary *types* but not the `path_dictionary_*` *functions* — include
`<ovx/path_dictionary/path_dictionary.h>` directly to call them (see Headers above).

## Troubleshooting

- **Headers not found** (`ovstage/ovstage.h` missing): ensure the consumer's include path
  contains ovstage's `include/`; the data-plane API is reached transitively via
  `<ovstage/ovstage.h>` → `ovstage_api/ovstage_api.h`.
- **Link errors for `ovstage_*` symbols**: link `ovstage::ovstage` or
  `ovstage::ovstage_static`; do not link runtime implementation libraries directly.
- **`ovstage_get_error_string` / diagnostics need an instance**: these are vtable-dispatched
  and take the `ovstage_instance_t*`; you cannot stringify an error before
  `ovstage_create_instance` returns — print the numeric code in that window.
- **Linux link fails with `libX11.so.6/libGL.so.1 ... not found (try using -rpath or
  -rpath-link)` or undefined `XOpenDisplay`/`glX*` references against
  `libov_*usd_ms.so`**: the linker cannot resolve the pre-packaged runtime's system
  dependencies. Either the host is missing `libx11-6`/`libgl1`/`libgomp1`
  (install them), or a Homebrew/Linuxbrew `ld` on `PATH` replaced the distribution
  linker (see next bullet).
- **Linux build fails with `GLIBC_2.xx' not found` (possibly already at compile
  time, from `as`) or X11/GL link errors although the system libraries are
  installed, and Homebrew/Linuxbrew exists on the host**: GCC finds `as` and `ld`
  via `PATH`, and Homebrew links its own `binutils` by default (since June 2026),
  so the system compiler silently uses Homebrew's tools and resolves
  glibc/libgomp against Homebrew's copies. Fix: `brew unlink binutils`, or strip
  the Homebrew `bin` dir from `PATH` for the build, or add `-B/usr/bin` to
  compiler flags. `find_package(ovstage)` warns at configure time when it
  detects a Homebrew linker; pass `-DOVSTAGE_SKIP_LINKER_CHECK=ON` to CMake (or
  set that environment variable) to silence the check.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `error-handling`, `string-handling` skills — API usage detail.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
