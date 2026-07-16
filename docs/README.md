# Building the Documentation

This directory contains the Sphinx source for the ovstage documentation published at
<https://nvidia-omniverse.github.io/ovstage>.

## Prerequisites

| Tool | Version | Install |
|------|---------|---------|
| [uv](https://docs.astral.sh/uv/getting-started/installation/) | ≥ 0.4 | `curl -LsSf https://astral.sh/uv/install.sh \| sh` (Linux/macOS) or `winget install astral-sh.uv` (Windows) |
| [Doxygen](https://www.doxygen.nl/download.html) | ≥ 1.9 | `sudo apt-get install doxygen` (Ubuntu) or download the Windows installer |

Sphinx and all Python documentation dependencies (Breathe, nvidia-sphinx-theme, etc.) are
declared in `python/pyproject.toml` under the `[docs]` extra and are installed
automatically by `uv` — you do not need to install them separately.

## Building on Linux

```bash
cd docs
make html
```

`make html` runs Doxygen first (to generate the C API XML consumed by Breathe), then
invokes Sphinx via `uv run`.

## Building on Windows

> **Note:** Doxygen is not yet integrated into the Windows build. C API reference pages
> will be absent from the output until Doxygen is wired into `make.bat`. All other pages
> build normally.

With `uv` on your `PATH` (see Prerequisites), `make.bat` automatically provisions
Sphinx and the rest of the `[docs]` extra from `python/pyproject.toml` into a managed
environment — no separate Sphinx install required:

```bat
cd docs
make.bat html
```

`make.bat` picks its Sphinx in this order:

1. `%SPHINXBUILD%` if you've set it explicitly.
2. `uv` on `PATH` — runs `uv run --project ..\python --extra docs sphinx-build`.
3. A bare `sphinx-build` on `PATH` (only useful if you've installed Sphinx
   yourself).

If you'd rather drive Sphinx directly:

```bat
cd python
uv run --extra docs sphinx-build -M html ..\docs ..\docs\_build
```

If `make.bat` reports that `sphinx-build` was not found, your shell does not have
`uv` (or any other Sphinx) on `PATH`. Install it with `winget install astral-sh.uv`,
open a new terminal, and re-run.

## Viewing the output

After a successful build, serve the result locally:

```bash
# Linux / macOS
uv run python -m http.server 8000 -d _build/html
```

```bat
REM Windows
uv run python -m http.server 8000 -d _build/html
```

Then open <http://localhost:8000/> in a browser.

## Cleaning build artifacts

```bash
# Linux
make clean
```

```bat
REM Windows
rmdir /s /q _build _doxygen
```
