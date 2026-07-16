# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Shared fixtures for the ovstage public Python tests.

`ovstage` loads libovstage via ctypes. In current-mode CI, install the produced
wheel and keep its bundled shared library loadable. For direct local runs, put the
in-repo `python/` package on PYTHONPATH and the built shared library on the
loader path (LD_LIBRARY_PATH, or OVSTAGE_LIBRARY_PATH_HINT). Import/load gaps skip
individual tests with a clear reason, but an all-skipped run is failed at session
finish so a missing runtime cannot masquerade as contract coverage.
"""

import pytest


@pytest.fixture(scope="session")
def ovstage_mod():
    # Skip (not fail) if the bindings package isn't importable.
    mod = pytest.importorskip("ovstage", reason="ovstage bindings not on PYTHONPATH")
    # Probe ONLY that the native library loads (library_version forces the lazy
    # dlopen). A genuine load failure — the loader can't find libovstage in this
    # environment — is an environment gap, so skip. Anything else must FAIL the
    # suite, not be masked as a skip (an all-skipped run exits green and hides real
    # regressions): OvstageError and any other runtime error are re-raised, and a
    # non-load RuntimeError (e.g. a create-instance probe failure) is too.
    from ovstage import OvstageError  # RuntimeError subclass — must not be swallowed

    try:
        mod.library_version()
    except OvstageError:
        raise
    except (OSError, RuntimeError) as exc:
        if "Failed to load" not in str(exc):
            raise
        pytest.skip(f"libovstage not loadable (set LD_LIBRARY_PATH / OVSTAGE_LIBRARY_PATH_HINT): {exc}")
    return mod


@pytest.fixture
def stage(ovstage_mod):
    """A fresh ovstage Stage per test, closed on teardown."""
    with ovstage_mod.Stage("test.ovstage.minimal") as s:
        yield s


def pytest_sessionfinish(session, exitstatus):
    """Do not let a collected-but-all-skipped public contract run exit green."""
    if session.config.option.collectonly or exitstatus != pytest.ExitCode.OK:
        return

    terminal = session.config.pluginmanager.get_plugin("terminalreporter")
    passed = len(terminal.stats.get("passed", [])) if terminal else 0
    if session.testscollected and passed == 0:
        if terminal:
            terminal.write_line(
                "ERROR: all ovstage public Python tests were skipped; "
                "check PYTHONPATH and the native library loader path.",
                red=True,
            )
        session.exitstatus = pytest.ExitCode.TESTS_FAILED
