# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Test-only gate hooks (``ovstage.test.hooks`` extension).

These deterministic delay/fail injectors are exposed only by test-instrumented
``libovstage`` builds. They let the concurrency tests stall a producer at a
known point to exercise timeout/serialization paths. :func:`available` reports
whether the loaded library exposes the test-hooks extension.
"""

import ctypes
from contextlib import contextmanager
from functools import lru_cache

from . import bindings as _b

__all__ = ["query_gate", "write_gate", "scoped_query_gate", "scoped_write_gate", "available"]

_EXTENSION_NAME = b"ovstage.test.hooks"
_EXTENSION_VERSION = 1


class _ExtensionHeader(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
    ]


_VOID_FN = ctypes.CFUNCTYPE(None)
_COUNT_FN = ctypes.CFUNCTYPE(ctypes.c_uint32)


class _TestHooksExtensionV1(ctypes.Structure):
    _fields_ = [
        ("header", _ExtensionHeader),
        ("query_gate_enable", _VOID_FN),
        ("query_gate_wait_until_entered", _VOID_FN),
        ("query_gate_release", _VOID_FN),
        ("query_gate_reset", _VOID_FN),
        ("query_gate_entered_count", _COUNT_FN),
        ("query_gate_fail_next", _VOID_FN),
        ("write_gate_enable", _VOID_FN),
        ("write_gate_wait_until_entered", _VOID_FN),
        ("write_gate_release", _VOID_FN),
        ("write_gate_reset", _VOID_FN),
        ("write_gate_entered_count", _COUNT_FN),
        ("write_gate_fail_next", _VOID_FN),
    ]


@lru_cache(maxsize=1)
def _hooks() -> _TestHooksExtensionV1:
    lib = _b.load()
    desc = _b.ovstage_instance_desc_t()
    desc.name = b"ovstage-python-test-hooks-extension-probe"
    inst = _b.ovstage_instance_p()
    code = lib.ovstage_create_instance(ctypes.byref(desc), ctypes.byref(inst))
    if code != _b.OVSTAGE_OK:
        raise RuntimeError(f"ovstage_create_instance failed while probing test hooks (code {int(code)})")
    try:
        if not inst or not inst.contents.vtable:
            raise RuntimeError("ovstage_create_instance returned an invalid instance while probing test hooks")
        extension = ctypes.c_void_p()
        code = inst.contents.vtable.contents.query_extension(
            inst.contents.context, _EXTENSION_NAME, ctypes.byref(extension)
        )
        if code != _b.OVSTAGE_OK or not extension.value:
            raise RuntimeError("ovstage.test.hooks is not exposed by the loaded libovstage")
        hooks = ctypes.cast(extension, ctypes.POINTER(_TestHooksExtensionV1)).contents
        if hooks.header.struct_size < ctypes.sizeof(_TestHooksExtensionV1):
            raise RuntimeError("ovstage.test.hooks has an incompatible struct size")
        if hooks.header.version != _EXTENSION_VERSION:
            raise RuntimeError(f"ovstage.test.hooks has unsupported version {hooks.header.version}")
        return hooks
    finally:
        lib.ovstage_destroy_instance(inst)


def available() -> bool:
    try:
        _hooks()
        return True
    except Exception:  # noqa: BLE001 — library not loadable in this environment
        return False


class _Gate:
    """Namespaced accessor for one gate (query or write)."""

    def __init__(self, kind: str):
        self._kind = kind

    def _fn(self, suffix: str):
        name = f"{self._kind}_gate_{suffix}"
        try:
            return getattr(_hooks(), name)
        except AttributeError as exc:
            raise RuntimeError(f"{name} is not exposed by ovstage.test.hooks") from exc

    def enable(self) -> None:
        self._fn("enable")()

    def wait_until_entered(self) -> None:
        self._fn("wait_until_entered")()

    def release(self) -> None:
        self._fn("release")()

    def reset(self) -> None:
        self._fn("reset")()

    def fail_next(self) -> None:
        self._fn("fail_next")()

    def entered_count(self) -> int:
        return int(self._fn("entered_count")())


query_gate = _Gate("query")
write_gate = _Gate("write")


@contextmanager
def scoped_query_gate():
    """Reset+enable the query gate on entry; release+reset on exit (ScopedQueryGate)."""
    query_gate.reset()
    query_gate.enable()
    try:
        yield query_gate
    finally:
        query_gate.release()
        query_gate.reset()


@contextmanager
def scoped_write_gate():
    """Reset+enable the write gate on entry; release+reset on exit (ScopedWriteGate)."""
    write_gate.reset()
    write_gate.enable()
    try:
        yield write_gate
    finally:
        write_gate.release()
        write_gate.reset()
