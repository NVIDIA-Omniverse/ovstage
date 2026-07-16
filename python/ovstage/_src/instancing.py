# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""High-level scene-graph-instancing queries for an ovstage instance.

The native API uses dictionary-lifetime prim-path handles and refcounted path
lists. This module presents paths as ordinary Python strings and releases every
native path-list result after converting it.
"""

import ctypes

from . import bindings as _b
from .path_dictionary import PathDictionary
from .types import OvstageError

__all__ = ["available", "get_instance_roots", "get_prototype_root", "get_prototype_roots"]

_SYMBOLS = (
    "ovstage_instancing_get_instance_roots",
    "ovstage_instancing_get_prototype_root",
    "ovstage_instancing_get_prototype_roots",
)


def available() -> bool:
    """Return whether the loaded ``libovstage`` exports all instancing queries."""
    try:
        lib = _b.load()
        return all(hasattr(lib, name) for name in _SYMBOLS)
    except Exception:  # noqa: BLE001 — library not loadable in this environment
        return False


def _require(stage):
    if not all(hasattr(stage._lib, name) for name in _SYMBOLS):
        raise OvstageError(_b.OVSTAGE_ERROR_NOT_SUPPORTED, "libovstage does not export the instancing query API")
    return stage._lib, stage._inst


def _consume_path_list(paths: PathDictionary, path_list: int) -> list[str]:
    """Convert an owned native path list to strings and release its reference."""
    try:
        return paths.get_path_strings(path_list)
    finally:
        paths.destroy_path_list(path_list)


def get_instance_roots(stage, prototype_root: str) -> list[str]:
    """Return the instance-root paths that reference ``prototype_root``."""
    lib, inst = _require(stage)
    with PathDictionary(stage) as paths:
        prototype = paths.intern_path(prototype_root)
        out = _b.ovx_primpath_list_t(_b.OVX_INVALID_PRIMPATH_LIST)
        stage._check(lib.ovstage_instancing_get_instance_roots(inst, prototype, ctypes.byref(out)))
        return _consume_path_list(paths, int(out.value))


def get_prototype_root(stage, instance_root: str) -> str:
    """Return the prototype-root path referenced by ``instance_root``.

    Raises:
        OvstageError: If ``instance_root`` is not an instance root or the native
            query otherwise fails.
    """
    lib, inst = _require(stage)
    with PathDictionary(stage) as paths:
        instance = paths.intern_path(instance_root)
        out = _b.ovx_primpath_t(_b.OVX_INVALID_PRIMPATH)
        stage._check(lib.ovstage_instancing_get_prototype_root(inst, instance, ctypes.byref(out)))
        return paths.path_to_string(int(out.value))


def get_prototype_roots(stage) -> list[str]:
    """Return every scene-graph-instancing prototype-root path in ``stage``."""
    lib, inst = _require(stage)
    with PathDictionary(stage) as paths:
        out = _b.ovx_primpath_list_t(_b.OVX_INVALID_PRIMPATH_LIST)
        stage._check(lib.ovstage_instancing_get_prototype_roots(inst, ctypes.byref(out)))
        return _consume_path_list(paths, int(out.value))
