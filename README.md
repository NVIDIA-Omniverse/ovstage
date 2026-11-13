# ovstage — shared scene data for simulation and visualization

[![Documentation](https://img.shields.io/badge/docs-ovstage-blue)](https://nvidia-omniverse.github.io/ovstage) [![License](https://img.shields.io/badge/license-NVIDIA%20Proprietary-red)](LICENSE) [![GitHub last commit](https://img.shields.io/github/last-commit/NVIDIA-Omniverse/ovstage)](https://github.com/NVIDIA-Omniverse/ovstage) [![Docs build](https://github.com/NVIDIA-Omniverse/ovstage/actions/workflows/docs.yml/badge.svg)](https://github.com/NVIDIA-Omniverse/ovstage/actions/workflows/docs.yml)

ovstage lets simulation and visualization libraries share scene data through C and Python APIs. Applications load OpenUSD scenes or write arrays directly, coordinate completed updates, and read what changed.

> **`pip install ovstage`** · Query, read, and update scene data from Python, or use the C SDK for a native application.
>
> *Pre-release: APIs, runtime behavior, and packaging may change.*

## 1. What is ovstage?

**ovstage** is a standalone library for sharing scene and simulation data within an application. Load selected values from [OpenUSD](https://openusd.org/), or author data directly through the C or Python API. Producers write arrays and mark completed updates; consumers query prims, read attributes, and track changes through the same API.

[ovphysx](https://github.com/NVIDIA-Omniverse/PhysX/tree/main/ovphysx) uses scene data for physics simulation, and [ovrtx](https://github.com/NVIDIA-Omniverse/ovrtx) uses it for rendering and sensor simulation. Both are optional consumers. The application decides when each producer runs and when each consumer reads; see the [overview](docs/concepts/overview.rst) for the update sequence and history limits.

## 2. What functionalities are available, and who are the target users?

**What you can do with it:**

- **Load and update OpenUSD data** — populate from a file or USDA text, add references, and apply source changes or time samples.
- **Read and write arrays** — use bulk C calls or Python arrays and DLPack tensors. CUDA write sources are supported. Reads and maps use CPU memory by default; the experimental [device-placement extension](docs/concepts/dlpack_tensors.rst#experimental-device-placement) also supports CUDA buffers and avoids migration when the requested copy is current.
- **Find scene data** — select prims by path or supported metadata filters, and inspect hierarchy and instancing relationships.
- **Track updates** — assign update sequence numbers and read changes within the available history window. By default history is off, and reads return the latest committed values only. An instance of `history_selective` (`ovstage_instance_desc_t.implementation`, Python `StageConfig(implementation="history_selective")`) turns history on for selected pairs. `ovstage_enable_history()` selects the exact `(prim, attribute)` pairs that retain earlier sealed values (`ovstage_history.h`). History support (the `history_selective` implementation and `ovstage_history.h`) is experimental and can change between releases without a deprecation period.
- **Clone scene content** — copy subtrees for repeated scene elements or environments.
- **Export selected data to USD** — write files or use the supported in-memory layer and stage destinations.

**Who benefits:**

- **Simulation developers** — exchange transforms and other attributes between producers and consumers.
- **Rendering and sensor application developers** — read completed scene updates for the next frame or sample.
- **C/C++ and Python integrators** — embed scene-data access in a standalone application and use the supplied examples.
- **AI coding agents and their developers** — follow the public [agent guide](AGENTS.md) and [task skills](skills/).

## 3. Documentation and reference links

The full documentation site is available at **<https://nvidia-omniverse.github.io/ovstage>**:

- [Python getting started](https://nvidia-omniverse.github.io/ovstage/python_api/getting_started.html)
- [C getting started](https://nvidia-omniverse.github.io/ovstage/c_api/getting_started.html)
- [Overview](https://nvidia-omniverse.github.io/ovstage/concepts/overview.html) — the data model, ordinals, watermarks, and reads
- [Benchmarking](https://nvidia-omniverse.github.io/ovstage/guides/benchmarking.html) with the bundled `benchmark.ovstage`
- [Choosing an API](docs/concepts/choosing_api.rst)
- [Limitations and current behavior](docs/concepts/limitations.rst)
- [Upgrading from ovstage 0.3](docs/guides/upgrading_from_0_3.rst)
- [Upgrading from ovstage 0.2](docs/guides/upgrading_from_0_2.rst)

Reference links in this source tree:

- **Overview:** [`OVERVIEW.md`](OVERVIEW.md)
- **Public C API:** [`include/ovstage/ovstage.h`](include/ovstage/ovstage.h) (data plane), [`include/ovstage/ovstage_types.h`](include/ovstage/ovstage_types.h) (public types), [`include/ovstage/ovstage_api/`](include/ovstage/ovstage_api/) (API types and utilities; experimental extensions under `experimental/`), [`include/ovstage/ovstage_instancing.h`](include/ovstage/ovstage_instancing.h) (high-level instancing queries; included from `ovstage.h`), [`include/ovstage/ovstage_population.h`](include/ovstage/ovstage_population.h) (USD-to-ovstage population; included from `ovstage.h`), [`include/ovstage/ovstage_population_types.h`](include/ovstage/ovstage_population_types.h) (the types of the population family, the export types among them), [`include/ovstage/ovstage_population_export.h`](include/ovstage/ovstage_population_export.h) (selected ovstage-to-USD authoring), [`include/ovstage/ovstage_ext_vtables.h`](include/ovstage/ovstage_ext_vtables.h) (the family vtables of the lifecycle, hierarchy, population, usd and instancing families, and the rules of the extension instances of `ovstage_instance_t`)
- **Path dictionary API:** [`include/ovx/path_dictionary/`](include/ovx/path_dictionary/) (shared OVX path-dictionary C API headers), [`include/ovx/types.h`](include/ovx/types.h) and [`include/ovx/string_types.h`](include/ovx/string_types.h) (shared OVX handle and string types)
- **DLPack header:** [`include/dlpack/dlpack.h`](include/dlpack/dlpack.h)
- **Examples index:** [`examples/README.md`](examples/README.md)
- **Minimal C example:** [`examples/c/minimal`](examples/c/minimal)
- **Minimal Python example:** [`examples/python/minimal`](examples/python/minimal)
- **Runtime-loop examples (USD populate + live updates):** [`examples/c/runtime-loop`](examples/c/runtime-loop), [`examples/python/runtime-loop`](examples/python/runtime-loop)
- **USD-export examples:** [`examples/c/usd-export`](examples/c/usd-export), [`examples/python/usd-export`](examples/python/usd-export)
- **Public contract tests:** [`tests/`](tests/)
- **AI coding agent skills:** [`skills`](skills)
- **Source:** [NVIDIA-Omniverse/ovstage](https://github.com/NVIDIA-Omniverse/ovstage)

## 4. System requirements

- A CUDA-capable GPU is required to write from CUDA tensors. CPU payload paths are also part of the API surface and do not require a GPU.
- **C/C++**:
    - The ovstage library has a C11-compatible interface. It can be loaded dynamically or by statically linking to the `ovstage-static` loader library, which requires linking to the C++ stdlib (on Linux, the system `libstdc++` — the loader's link interface names it by `-lstdc++`, so it is resolved by whichever linker runs the consumer link).
    - The example code requires a C++17 compiler and CMake 3.18+. The examples use cmake to fetch the prebuilt ovstage package from the GitHub.com release page.
    - On Linux, linking against the SDK additionally requires glibc 2.35+, a small set of system libraries, and the distribution's binutils linker — a Homebrew/Linuxbrew `ld` on `PATH` is not supported; see [Packaging and Dependencies](#packaging-and-dependencies).
- **Python**:
    - Python 3.10–3.14 versions are supported. On Windows, Python 3.11+ is recommended (Python 3.10 is not guaranteed to work because older distributions bundle an older vcruntime140.dll; see [Packaging and Dependencies](#packaging-and-dependencies)).
    - The examples use [uv](https://docs.astral.sh/uv/) to resolve the `ovstage` wheel.

## 5. Licensing

Use of the software and materials is governed by the [NVIDIA Software License Agreement](LICENSE) and its applicable product-specific terms. Review the license terms of additional third-party components before use. This project is not currently open to external contributions.

For security reporting, see [SECURITY.md](SECURITY.md). Do not report security vulnerabilities through public issue trackers.

---

## Getting Started in Python

[ovstage](https://pypi.org/project/ovstage/) Python wheels are distributed on [PyPI](https://pypi.org/).

The fastest way to get running with ovstage is the minimal Python example. It runs against the released `ovstage` wheel via [uv](https://docs.astral.sh/uv/):

```bash
cd examples/python/minimal
uv run main.py
```

## Getting Started in C/C++

The minimal C/C++ example is the fastest way to get started with ovstage in a C/C++ environment. The example builds standalone with CMake and fetches the released ovstage package on first configure:

```bash
cd examples/c/minimal

# Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/minimal
```

On Windows, configure and build the same way (`cmake -B build`, then `cmake --build build --config Release`) and put the ovstage package `bin/` directory on `PATH` before running — see [`examples/c/minimal/README.md`](examples/c/minimal/README.md).

Expected output:

```text
attribute token <N> = 'temperature'
read back ordinal 1: 1.0 2.0 3.0
```


## Packaging and Dependencies

ovstage is distributed as a C package (.zip file) available in the Releases page in this repo, and as Python wheels available via pypi.org. These packages include a number of pre-packaged dependencies.

`ovstage.__version__` reports the installed Python package version without loading the native runtime. For the exact native build, including its build number and Git revision, use `ovstage.get_build_info()` in Python or `ovstage_get_build_info()` in C. The C package and Python wheel also include the same details in `ovstage-build-info.json` for inspection without running code. The shared and static loaders report their loader build, while a directly linked runtime reports its runtime build. If a loader client selects a separately built runtime, that runtime reports its own values through its callback log and crash metadata. The callback record is emitted at `INFO`, so Python clients must request at least `LogSeverity.INFO` (or enable `omni.ovstage=info`) to receive it. The JSON copy is schema-versioned diagnostic metadata, not a stable API or a deployment validation contract.

In addition to these pre-packaged dependencies, on Windows, ovstage depends on [Microsoft's VC runtime redistributable libraries](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170), with a minimum version of 14.38 (as included in Visual Studio 2022 17.8). These libraries can be installed by an end user (using the linked Microsoft resources) or can be included by an application. The use of this version of the MSVC runtime libraries makes our binaries compatible with the vcruntime140.dll pre-packaged in Python distributions for Windows as old as Python 3.11. Older Python versions include older vcruntime140.dll, and are therefore not guaranteed to work.

On Linux, the packages are built against the `manylinux_2_35` baseline: they require glibc 2.35 or newer (Ubuntu 22.04+, or an equivalent distribution). A few host libraries are deliberately not bundled and must be resolvable on the system:

- Required to link a C/C++ application against the SDK, and to load it: `libX11.so.6`, `libGL.so.1`, and `libgomp.so.1` (Debian/Ubuntu packages `libx11-6`, `libgl1`, `libgomp1`). These are transitive dependencies of the pre-packaged runtime, so the linker must be able to find them even though your code never references them directly.
- Additionally used at runtime by optional components: `libXt.so.6`, `libGLX.so.0`, `libOpenGL.so.0` (`libxt6`, `libglx0`, `libopengl0`), and the NVIDIA driver's `libcuda.so.1` for CUDA tensor writes.

Building must use the distribution's binutils (`as`, `ld`). On x86_64 hosts with Homebrew/Linuxbrew installed, note that Homebrew has linked its own `binutils` into `PATH` by default since June 2026: GCC locates `as` and `ld` through `PATH`, so it silently uses Homebrew's tools even when CMake selects the system compiler. Depending on the Homebrew and host glibc versions, the build then fails at compile time (`as: ... version 'GLIBC_2.xx' not found`) or at link time with unresolved X11/GL symbols or `GLIBC_2.xx` version errors reported against the pre-packaged runtime. If you hit this, either run `brew unlink binutils`, or remove the Homebrew `bin` directory from `PATH` for the build, or add `-B/usr/bin` to your compiler flags. The SDK's CMake package config detects a Homebrew linker and warns at configure time (pass `-DOVSTAGE_SKIP_LINKER_CHECK=ON` to CMake, or set that environment variable, to silence it).

## Building the Documentation

**Prerequisites:** [uv](https://docs.astral.sh/uv/) and [Doxygen](https://www.doxygen.nl/) on Linux and Windows; see [`docs/README.md`](docs/README.md) for installation and build details.

```bash
cd docs
make html
```

Then serve the output locally and open <http://localhost:8000/>:

```bash
uv run python -m http.server 8000 -d _build/html
```

## Support

Report documentation issues, installation problems, and runtime issues through the [NVIDIA Omniverse developer forum](https://forums.developer.nvidia.com/c/omniverse/300).

## AI Coding Agents

AI coding agents: start with [AGENTS.md](AGENTS.md) and [llms.txt](llms.txt).

---

*ovstage · Copyright (c) 2026 NVIDIA Corporation.*
