# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage import-surface contract: every module ovstage publishes in
# ``__all__`` must be importable by its dotted path, not just reachable as an
# attribute of an already-imported ``ovstage``. Re-exporting a submodule with
# ``from ._src import population`` binds a name but does not register a submodule,
# so ``import ovstage.population`` raises ModuleNotFoundError unless ``__init__``
# also registers the alias in ``sys.modules``. The docs publish the dotted path
# (``.. automodule:: ovstage.population``), so the two must not drift apart.
# CPU-only; no Stage required.

import importlib
import subprocess
import sys

import pytest


def _public_submodules(mod):
    """The names in ``mod.__all__`` that resolve to modules."""
    import types

    return sorted(
        name
        for name in getattr(mod, "__all__", ())
        if isinstance(getattr(mod, name, None), types.ModuleType)
    )


def test_public_submodules_are_registered(ovstage_mod):
    # The registration itself: a published submodule must have a sys.modules entry
    # under its public dotted name, which is what makes every import spelling work.
    names = _public_submodules(ovstage_mod)
    assert names, "expected ovstage.__all__ to publish at least one submodule"

    missing = [name for name in names if f"ovstage.{name}" not in sys.modules]
    assert not missing, (
        f"published submodules absent from sys.modules: {missing}. "
        "Register them in ovstage/__init__.py; a bare re-export is not a submodule."
    )


def test_public_submodules_import_by_dotted_path(ovstage_mod):
    # importlib takes the same path as ``import ovstage.<name>`` and as Sphinx
    # autodoc's ``.. automodule::``, and the alias must resolve to the very same
    # module object as the attribute so patching one path affects both.
    for name in _public_submodules(ovstage_mod):
        imported = importlib.import_module(f"ovstage.{name}")
        assert imported is getattr(ovstage_mod, name)


def test_public_submodules_import_in_a_fresh_interpreter(ovstage_mod):
    # The tests above run in an interpreter that already imported ovstage. A user
    # types ``import ovstage.population`` first, with nothing cached, so re-check
    # each spelling out-of-process where the parent import is the only trigger.
    for name in _public_submodules(ovstage_mod):
        for statement in (
            f"import ovstage.{name}",
            f"import ovstage.{name} as _m",
            f"from ovstage.{name} import __name__ as _n",
        ):
            completed = subprocess.run(
                [sys.executable, "-c", statement],
                capture_output=True,
                text=True,
            )
            assert completed.returncode == 0, (
                f"`{statement}` failed with rc={completed.returncode}:\n{completed.stderr}"
            )


def test_gates_is_not_published(ovstage_mod):
    # ``gates`` wraps the test-hooks extension exposed only by test-instrumented
    # libovstage builds. It stays importable as an attribute for the concurrency
    # tests but must not be advertised as public API, or the contract above would
    # promote instrumentation to a supported dotted import.
    assert "gates" not in getattr(ovstage_mod, "__all__", ())


@pytest.mark.parametrize("name", ["population", "instancing"])
def test_documented_submodules_stay_published(ovstage_mod, name):
    # Guards the reverse drift: these two are written as ``ovstage.<name>`` in the
    # shipped docs, so dropping them from ``__all__`` would break that contract
    # without any test above noticing (it only checks what ``__all__`` claims).
    assert name in getattr(ovstage_mod, "__all__", ())
