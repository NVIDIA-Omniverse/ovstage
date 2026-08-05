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
name: project-setup-python
description: >
  Setting up a Python project that uses the ovstage Python bindings. Use when user asks
  to create a Python ovstage app, import ovstage, configure the library path, or scaffold
  Python code that reads/writes stage data.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - python
  - setup
tools:
  - Read
  - Grep
  - Bash
---

# Project Setup (Python)

## When to Use

Use this skill when the user asks to create a Python project that uses ovstage, import the
`ovstage` package, configure the library path so the bindings load, or scaffold a minimal
Python program that reads/writes stage data.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- Whether the consumer uses a built/installed `ovstage` wheel or the in-tree `python/`
  package.
- Whether the build output dir (holding the ovstage shared library) is on the loader path
  (`PATH` on Windows, `LD_LIBRARY_PATH` on Linux).
- Whether the workflow needs `numpy` (tensor I/O) and/or the path dictionary.
- Repository source snippets referenced below. Treat these snippets as the API source of truth.

## Prerequisites

- Python **3.10–3.13** (`requires-python = ">=3.10,<3.14"`).
- A built ovstage shared library reachable by the loader (see Library Path below).
- `numpy` for tensor attribute I/O.

## Instructions

1. Make the `ovstage` package importable (installed/pinned wheel — see Installing
   below — or the in-tree `python/` package on `PYTHONPATH`).
2. Ensure the ovstage shared library loads. The wheel bundles it at
   `<package>/bin` (found automatically); for the in-tree package, put the build
   output dir (holding it) on `PATH` (Windows) or `LD_LIBRARY_PATH` (Linux).
3. Add `numpy` for tensor I/O.
4. Write the minimal program: `Stage` + `PathDictionary`, then write → advance write floor
   → read.
5. Run it and confirm the read-back values.

## Output Format

- For explanations, cite the relevant API names, source snippets, and caveats.
- For code changes, summarize the files changed, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before documenting new API usage.
- **Consumption model:** ovstage is consumed as a **published `ovstage` wheel** (see Installing),
  distributed on public PyPI (`pip install ovstage` / `uv add ovstage`). The import + library-path
  mechanics here also cover consuming a **local build**. For C/C++ setup, see
  `project-setup-c`.

## Overview

ovstage ships a Python package, **`ovstage`**, built with hatchling
(`pyproject.toml`, `requires-python = ">=3.10,<3.14"`). It binds the ovstage C
data-plane via ctypes, so the **ovstage shared library must be loadable** at
import time. The released wheel bundles that library at `<package>/bin`, which
the bindings find automatically — a wheel consumer needs no loader-path setup.

## Installing

Install the published `ovstage` wheel with your Python tool of choice. The platform wheels
bundle the native library under `<package>/bin`, so a **published install needs no loader-path
setup** (skip the Library Path section below — that is only for local builds):

```bash
uv add ovstage        # or: pip install ovstage
```

The minimal example wires this up as a ready-to-copy starting point — pin `ovstage` in its
`pyproject.toml` and run with `uv run main.py`:

> **Setup:** `examples/python/minimal/pyproject.toml` (pins `ovstage`; `uv run main.py` resolves,
> installs, and runs it)


## Library Path

For a **local build** (not a published wheel), the loader finds the ovstage shared library on
the **loader path**: add the build output
dir (the one holding `ovstage.dll` / `libovstage.so`) to `PATH` (Windows) or
`LD_LIBRARY_PATH` (Linux). The wheel layout (`<package>/bin`) is also searched
automatically.

```bash
# Linux
export LD_LIBRARY_PATH=/path/to/ovstage/build/output:$LD_LIBRARY_PATH
# Windows (PowerShell)
#   $env:PATH = "C:\path\to\ovstage\build\output;$env:PATH"
```

> You can also point the loader at the build dir with `OVSTAGE_LIBRARY_PATH_HINT` — as an
> environment variable, or as the `ovstage._src.bindings.OVSTAGE_LIBRARY_PATH_HINT` module
> global for an in-process override (the module global takes precedence when both are set).

## Key Dependencies

| Package | Purpose |
|---------|---------|
| `ovstage` | the bindings (Stage, PathDictionary, OrdinalRange, …) |
| `numpy` | tensor attribute I/O (`write_attribute(tensors=...)`, `group.array(...)`) |

## Minimal Program

`Stage` + `PathDictionary` (both context managers), then intern a token, build a path list,
open a query, and write → advance write floor → read. Async ops return an `Operation`; call
`.wait()`. The verified example composes the full flow:

> **Source:** `examples/python/minimal/main.py` snippet `setup`
>
> Followed by: `examples/python/minimal/main.py` snippet `intern-and-resolve`
>
> Followed by: `examples/python/minimal/main.py` snippet `path-list-query`
>
> Followed by: `examples/python/minimal/main.py` snippet `minimal-write-read`

The context managers wrap the whole sequence —
`with ovstage.Stage("demo") as stage, ovstage.PathDictionary(stage) as paths:` — and the
path dictionary is the stage's (shared; no separate teardown).

The fixed-size write in `minimal-write-read` passes `is_array=False`; array writes must pass
`is_array=True`. Attribute kind is explicit and is never inferred from tensor shape or count.

See `error-handling` (Python) for `OvstageError` / `OvxError`, `path-dictionary` (Python)
for `PathDictionary`, and `string-handling` (Python) for `str` vs. interned-token args.

## Troubleshooting

- **Import fails / shared library not found:** make the build output dir (holding
  `ovstage.dll` / `libovstage.so`) reachable — set `OVSTAGE_LIBRARY_PATH_HINT` to it, or add
  it to `PATH` (Windows) / `LD_LIBRARY_PATH` (Linux). See Library Path.
- **Unsupported Python version:** the package supports 3.10–3.13 only; recreate the env
  with a supported interpreter rather than editing the constraint.
- **`OvstageError` / `OvxError` on calls:** these are `RuntimeError` subclasses carrying a
  numeric `code`; see the `error-handling` skill.
- **Forgot to `.wait()`:** async ops (`write_attribute`, `advance_write_floor`,
  `read_attributes`) return handle objects; effects/errors are observed on `.wait()`.

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `project-setup-c` — C/C++ consumer setup.
- `error-handling`, `string-handling`, `path-dictionary` skills — Python API detail.
- Keep related skills, docs, and snippets synchronized when changing the workflow.
