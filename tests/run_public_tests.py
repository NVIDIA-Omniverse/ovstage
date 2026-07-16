#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Run the ovstage public tests + shipping examples against a PRODUCED package.

Cross-platform (Linux/Windows), consuming the package the way an external
consumer would ("current mode"):

  1. unpack the package .zip (or use an unpacked dir) and locate
     lib/cmake/ovstage/ovstageConfig.cmake -> the package root;
  2. C tests: cmake configure+build (find_package(ovstage) + GoogleTest) -> ctest;
  3. C examples: one aggregate cmake configure+build, then run every shipped
     example, assert exit 0 (GPU-requiring examples run only when a CUDA device
     is detected; OVSTAGE_PUBLIC_TESTS_REQUIRE_GPU=1 makes their skip a failure);
  4. if --wheel is given: create/reuse a venv, force-reinstall the wheel (so a
     rerun always tests this --wheel) + pytest + numpy, run pytest over
     tests/python, then run every shipped Python example (extra public-PyPI
     deps such as usd-core install from the example's own pins; GPU-only deps
     such as warp install only when a CUDA device is present).

The package's bin/ is put on the loader path (PATH on Windows, LD_LIBRARY_PATH on
Linux) so ovstage finds its bundled runtime closure. CPU-only except the
GPU-gated examples above.

Examples
--------
  # Linux, against the produced zip + wheel
  python3 tests/run_public_tests.py --package ovstage@X.zip --wheel ovstage-X.whl
  # C only (already-unpacked package dir, no Python)
  python tests/run_public_tests.py --package /path/to/unpacked-ovstage
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
PUBLIC_DIR = TESTS_DIR.parent
C_TESTS_DIR = TESTS_DIR / "c"
PYTHON_TESTS_DIR = TESTS_DIR / "python"
C_EXAMPLES_DIR = PUBLIC_DIR / "examples" / "c"
PY_EXAMPLES_DIR = PUBLIC_DIR / "examples" / "python"

# Every shipping example runs, not just minimal. gpu=True marks examples whose
# core flow needs a CUDA device (authoring-hierarchy computes world transforms
# on the GPU hierarchy models); they run when a GPU is detected, are skipped
# otherwise, and OVSTAGE_PUBLIC_TESTS_REQUIRE_GPU=1 turns that skip into a
# failure so GPU CI runners cannot silently lose the coverage.
C_EXAMPLES = [
    ("minimal", False),
    ("runtime-loop", False),
    ("time-and-ordinals", False),
    ("write-flavors", False),
    ("authoring-hierarchy", True),
    ("producer-consumer", False),
    ("queries", False),
]
# Python examples; extra_pins names PyPI deps beyond numpy the example needs
# (pins read from the example's own pyproject.toml so they stay in one place).
PY_EXAMPLES = [
    ("minimal", False, ()),
    ("runtime-loop", False, ()),
    ("time-and-ordinals", False, ()),
    ("write-flavors", False, ()),
    ("authoring-hierarchy", True, ()),
    ("producer-consumer", False, ()),
    ("queries", False, ()),
    ("usd-to-ovstage", False, ("usd-core",)),
]
# Deps needed only by an example's GPU section (the example self-skips that
# section when they are absent, so they are deliberately NOT hard deps in its
# pyproject). Installed only when a CUDA device is present — otherwise the GPU
# section (e.g. write-flavors section 11, the skill-referenced gpu-warp-ingest
# snippet) would self-skip forever and GPU CI would silently lose the coverage.
# Pins, if any, come from the example's own pyproject via _pinned_spec.
PY_GPU_DEPS = {
    "write-flavors": ("warp-lang",),
}
# Examples whose pyproject deliberately excludes linux-aarch64 (usd-core
# publishes no aarch64 wheels); skipped there instead of hard-failing the
# dep install. x86_64/Windows keep the hard fail.
PY_EXAMPLES_NO_AARCH64 = {"usd-to-ovstage"}

IS_WINDOWS = sys.platform == "win32"
IS_AARCH64 = platform.machine().lower() in ("aarch64", "arm64")


def _check_example_inventory() -> None:
    """Hard-fail if an example exists on disk but is not in the registry above.

    C_EXAMPLES/PY_EXAMPLES is the coverage registry: a present-but-unlisted
    example would silently get zero CI coverage. (The reverse — listed but
    absent — is reported by the per-example exists-skip at run time.)"""
    for label, registry, base, marker in (
        ("C", "C_EXAMPLES", C_EXAMPLES_DIR, "main.cpp"),
        ("Python", "PY_EXAMPLES", PY_EXAMPLES_DIR, "main.py"),
    ):
        if not base.is_dir():
            continue
        listed = {entry[0] for entry in (C_EXAMPLES if label == "C" else PY_EXAMPLES)}
        on_disk = {d.name for d in base.iterdir() if d.is_dir() and (d / marker).is_file()}
        unlisted = sorted(on_disk - listed)
        if unlisted:
            raise SystemExit(
                f"{label} example(s) present under {base} but missing from {registry} in "
                f"{Path(__file__).name}: {', '.join(unlisted)} — register them so they "
                "get CI coverage")


def _check_absent_examples(label: str, absent: list[str]) -> None:
    """Escalate listed-but-absent examples to a hard failure in the canonical tree.

    A published/filtered tree may legitimately omit individual examples (the
    aggregate examples/c/CMakeLists.txt skips absent dirs with the same note),
    so absence there stays a skip. In the canonical source tree every registered
    example must exist — an absent one is an accidental deletion that would
    otherwise silently lose CI coverage while the job still reports success.

    The canonical source tree includes its CI-only skill validator, while the
    published distribution omits that tooling. Its presence therefore
    distinguishes a source checkout, where a missing registered example is an
    error, from a filtered distribution, where absence is an allowed skip."""
    if not absent:
        return
    if (PUBLIC_DIR / "tools" / "ci" / "validate_skills.py").is_file():
        raise SystemExit(
            f"{label} example(s) registered but absent from this canonical tree: "
            f"{', '.join(absent)} — restore them (or update the registry in "
            f"{Path(__file__).name}) so CI coverage is not silently lost")


def _log(msg: str) -> None:
    print(f"[ovstage-public-tests] {msg}", flush=True)


def _run(cmd, **kw) -> None:
    """Run a command, echoing it; raise on non-zero (do NOT capture output — a
    piped ovstage child can hang in CI, per examples/smoke/run_smoke_test.py)."""
    _log("$ " + " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def _unpack_and_find_root(package: Path, work: Path) -> Path:
    """Return the package root (dir holding lib/cmake/ovstage/ovstageConfig.cmake)."""
    if package.is_dir():
        search_root = package
    else:
        search_root = work / "package"
        search_root.mkdir(parents=True, exist_ok=True)
        # cmake -E tar preserves Unix symlinks (unlike Python's zipfile).
        _run(["cmake", "-E", "tar", "xf", package], cwd=search_root)
    for cfg in sorted(search_root.rglob("ovstageConfig.cmake")):
        # <root>/lib/cmake/ovstage/ovstageConfig.cmake -> <root>
        return cfg.parents[3]
    raise SystemExit(f"could not locate ovstageConfig.cmake under {search_root}")


def _runtime_env(root: Path) -> dict:
    """Package runtime on the loader path so ovstage finds its bundled closure."""
    env = os.environ.copy()
    bin_dir = root / "bin"
    if IS_WINDOWS:
        # Windows DLLs carry no embedded search path: expose bin/ (ovstage.dll) and
        # bin/plugins/ (ovstage.dll's direct import deps usd_ms/tbb). These suites
        # link the dynamic ovstage::ovstage target, so the loader path is set here
        # (unlike examples/smoke, which links the static loader and self-locates).
        key, search = "PATH", [bin_dir, bin_dir / "plugins"]
    else:
        # libovstage.so self-locates via RUNPATH ($ORIGIN:$ORIGIN/plugins), so bin/
        # alone is enough for the loader to find libovstage.so itself.
        key, search = "LD_LIBRARY_PATH", [bin_dir]
    prefix = os.pathsep.join(str(p) for p in search)
    env[key] = prefix + (os.pathsep + env[key] if env.get(key) else "")
    return env


def _assert_ovstage_dir_under(build: Path, root: Path) -> None:
    """Hard-fail unless configure resolved ovstage inside the package under test.

    find_package() records the directory holding ovstageConfig.cmake as
    ovstage_DIR in the build cache. If that is not under the --package root,
    the build would consume some other ovstage — a package cached by an earlier
    configure, or the pinned release ovstage_fetch() downloads when
    find_package() misses — and the run would validate the wrong artifact.
    """
    cache = build / "CMakeCache.txt"
    ovstage_dir = ""
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith("ovstage_DIR:"):
            ovstage_dir = line.split("=", 1)[1].strip()
            break
    if not ovstage_dir or ovstage_dir.endswith("-NOTFOUND"):
        raise SystemExit(f"configure did not resolve ovstage (no ovstage_DIR in {cache})")
    try:
        Path(ovstage_dir).resolve().relative_to(root.resolve())
    except ValueError:
        raise SystemExit(
            f"ovstage_DIR is {ovstage_dir}, which is outside the package under test "
            f"({root}); refusing to validate a cached or downloaded ovstage instead "
            "of the --package artifact") from None
    _log(f"ovstage_DIR {ovstage_dir} is inside the package under test")


def _cmake_build(src: Path, build: Path, root: Path, config: str) -> None:
    # Always build from scratch: a persistent build dir caches ovstage_DIR (a
    # rerun would silently keep consuming the package found by an earlier run)
    # and can hold orphaned binaries from a previous run that would pass the
    # exe.is_file() check. Clear everything EXCEPT _deps, which keeps
    # FetchContent downloads (googletest for tests/c) warm for offline or
    # flaky-network reruns.
    if build.exists():
        for child in build.iterdir():
            if child.name == "_deps":
                continue
            if child.is_dir():
                shutil.rmtree(child)
            else:
                child.unlink()
    _run(["cmake", "-S", src, "-B", build, f"-DCMAKE_PREFIX_PATH={root}",
          f"-DCMAKE_BUILD_TYPE={config}"])
    _assert_ovstage_dir_under(build, root)
    _run(["cmake", "--build", build, "--config", config])


def _run_c_tests(root: Path, config: str) -> None:
    _log("=== C tests (ctest) ===")
    build = C_TESTS_DIR / "build"
    _cmake_build(C_TESTS_DIR, build, root, config)
    _run(["ctest", "--test-dir", build, "-C", config, "--output-on-failure"],
         env=_runtime_env(root))


def _have_gpu() -> bool:
    """A CUDA device is visible (nvidia-smi lists at least one GPU)."""
    try:
        out = subprocess.run(["nvidia-smi", "-L"], capture_output=True, text=True, timeout=30)
        return out.returncode == 0 and "GPU" in out.stdout
    except (OSError, subprocess.TimeoutExpired):
        return False


def _gpu_gate(name: str) -> bool:
    """True if a GPU-requiring example should run; skip or fail otherwise."""
    if _have_gpu():
        return True
    if os.environ.get("OVSTAGE_PUBLIC_TESTS_REQUIRE_GPU") == "1":
        raise SystemExit(f"{name} requires a GPU and OVSTAGE_PUBLIC_TESTS_REQUIRE_GPU=1 is set, "
                         "but no CUDA device was detected")
    _log(f"skip {name}: needs a CUDA device (none detected)")
    return False


def _run_c_examples(root: Path, config: str) -> None:
    if not (C_EXAMPLES_DIR / "CMakeLists.txt").is_file():
        _log("skip C examples: no CMakeLists.txt")
        return
    _log("=== C examples (configure + build all, then run each) ===")
    build = C_EXAMPLES_DIR / "build"
    # The shipped examples' CMakeLists use ovstage_fetch(), which does
    # find_package(ovstage) first — so CMAKE_PREFIX_PATH (set by _cmake_build)
    # makes them consume the produced package locally instead of downloading.
    # The examples/c CMakeLists aggregates every example into one build.
    _cmake_build(C_EXAMPLES_DIR, build, root, config)
    absent: list[str] = []
    for name, needs_gpu in C_EXAMPLES:
        if not (C_EXAMPLES_DIR / name / "CMakeLists.txt").is_file():
            _log(f"skip C {name}: not in this tree")
            absent.append(name)
            continue
        if needs_gpu and not _gpu_gate(f"C {name}"):
            continue
        _log(f"=== C example: {name} ===")
        # Pick the config-specific artifact, not the first glob hit (multi-config
        # generators — Windows/VS — emit build/<name>/<config>/exe; single-config —
        # Linux — emit build/<name>/exe), so a stale binary from another config is
        # never run. Run from the binary's directory: examples that ship a scene
        # (runtime-loop, queries) have it copied next to the binary by their
        # CMakeLists and load it relative to the working directory.
        exe_name = f"{name}.exe" if IS_WINDOWS else name
        exe = (build / name / config / exe_name) if IS_WINDOWS else (build / name / exe_name)
        if not exe.is_file():
            raise SystemExit(f"built example {exe_name} not found at {exe}")
        _run([exe], env=_runtime_env(root), cwd=exe.parent)
        if name == "producer-consumer":
            # Also cover the concurrent mode (--threads): producer and consumer
            # on one shared instance with coordinated shutdown. The example
            # documents an EXPECTED race under load (a producer write rejected
            # while it overlaps an outstanding consumer read) and exits 1 with
            # a coordinated shutdown in that case, so exit 0 and exit 1 are
            # both healthy; anything else (crash, signal) fails the suite.
            _log(f"=== C example: {name} --threads ===")
            threads_run = subprocess.run([str(exe), "--threads"], env=_runtime_env(root),
                                         cwd=exe.parent)
            if threads_run.returncode not in (0, 1):
                raise SystemExit(
                    f"{name} --threads exited abnormally (code {threads_run.returncode})")
            if threads_run.returncode == 1:
                _log(f"{name} --threads hit its documented expected race (exit 1)")
    _check_absent_examples("C", absent)


def _venv_python(venv: Path) -> Path:
    return venv / ("Scripts/python.exe" if IS_WINDOWS else "bin/python")


def _pinned_spec(example_dir: Path, dep: str) -> str:
    """Return "dep==version" as pinned in the example's own pyproject.toml, or
    the bare name if no pin is found (the pyproject is the single source of
    truth for the pin; this avoids duplicating it here)."""
    pyproject = example_dir / "pyproject.toml"
    if pyproject.is_file():
        for line in pyproject.read_text(encoding="utf-8").splitlines():
            line = line.strip().strip('",')
            if line.startswith(f"{dep}=="):
                return line
    return dep


def _install_example_deps(py: Path, example_dir: Path, name: str, deps: tuple[str, ...]) -> None:
    """pip-install example deps into the venv; failure is a hard failure —
    skipping would silently drop the example from coverage while the job still
    reports success."""
    specs = [_pinned_spec(example_dir, dep) for dep in deps]
    try:
        _run([py, "-m", "pip", "install", *specs])
    except subprocess.CalledProcessError as exc:
        raise SystemExit(
            f"could not install {' '.join(specs)} for Python example {name}"
        ) from exc


def _run_python_suite(wheel: Path) -> None:
    _log("=== Python tests + examples (against the produced wheel) ===")
    # Keep the venv inside the tree this script runs from (PUBLIC_DIR/_build):
    # paths derived from parent directories depend on where the tree is checked
    # out and may not be writable.
    venv = PUBLIC_DIR / "_build" / ".ovstage-public-tests-venv"
    venv.parent.mkdir(parents=True, exist_ok=True)
    _run([sys.executable, "-m", "venv", "--upgrade-deps", venv])
    py = _venv_python(venv)
    # --no-index keeps the install hermetic AND doubles as the contract check
    # that the wheel declares no runtime deps (a dep would fail to resolve).
    # --force-reinstall because the venv persists across runs and
    # pip skips an already-installed same-version wheel: without it a rerun
    # would silently validate the previous run's artifact, not this --wheel.
    _run([py, "-m", "pip", "install", "--no-index", "--force-reinstall", wheel])
    _run([py, "-m", "pip", "install", "pytest", "numpy"])
    _run([py, "-m", "pytest", PYTHON_TESTS_DIR, "-v"])
    absent: list[str] = []
    for name, needs_gpu, extra_deps in PY_EXAMPLES:
        example_dir = PY_EXAMPLES_DIR / name
        main_py = example_dir / "main.py"
        if not main_py.is_file():
            _log(f"skip Python {name}: not in this tree")
            absent.append(name)
            continue
        if needs_gpu and not _gpu_gate(f"Python {name}"):
            continue
        if name in PY_EXAMPLES_NO_AARCH64 and IS_AARCH64:
            _log(f"skip Python {name}: its pyproject deliberately excludes "
                 f"linux-aarch64 (usd-core publishes no aarch64 wheels)")
            continue
        if extra_deps:
            _install_example_deps(py, example_dir, name, extra_deps)
        gpu_deps = PY_GPU_DEPS.get(name, ())
        if gpu_deps and _gpu_gate(f"Python {name} GPU-section deps ({', '.join(gpu_deps)})"):
            _install_example_deps(py, example_dir, name, gpu_deps)
        _log(f"=== Python example: {name} ===")
        _run([py, main_py], cwd=example_dir)
    _check_absent_examples("Python", absent)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", required=True,
                        help="ovstage package .zip or an unpacked package dir")
    parser.add_argument("--wheel", help="ovstage wheel; if omitted, Python tests are skipped")
    parser.add_argument("--config", default="Release", help="CMake build config (default: Release)")
    parser.add_argument("--skip-c", action="store_true", help="skip the C tests + example")
    parser.add_argument("--skip-python", action="store_true", help="skip the Python tests + example")
    args = parser.parse_args(argv)

    _check_example_inventory()

    with tempfile.TemporaryDirectory(prefix="ovstage-public-tests-") as tmp:
        root = _unpack_and_find_root(Path(args.package).resolve(), Path(tmp))
        _log(f"package root: {root}")

        if not args.skip_c:
            _run_c_tests(root, args.config)
            _run_c_examples(root, args.config)

        if not args.skip_python:
            if args.wheel:
                _run_python_suite(Path(args.wheel).resolve())
            else:
                _log("skip Python suite: no --wheel given")

    _log("ALL PUBLIC TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
