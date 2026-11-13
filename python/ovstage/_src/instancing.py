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
native path-list result after converting it. The queries are members of the
instancing family vtable of the instance (``ovstage_ext_vtables.h``). The
facade of the stage serves them under their ``ovstage_instancing_*`` names.
"""

import ctypes
import sys
from collections.abc import Iterable
from enum import IntEnum
from typing import NamedTuple

from . import bindings as _b
from .path_dictionary import PathDictionary
from .types import OvstageError

__all__ = [
    "available",
    "get_instance_roots",
    "get_instance_roots_batch",
    "get_prototype_root",
    "get_prototype_roots",
    "resolve_paths",
    "PathKind",
    "PathResolution",
]


class PathKind(IntEnum):
    """Classification of a populated logical path and its backing prim."""

    NOT_FOUND = 0
    NON_INSTANCE = 1
    INSTANCE = 2


class PathResolution(NamedTuple):
    """Resolved value; missing paths use None for all three path fields."""

    kind: PathKind
    instance_root: str | None
    prototype_root: str | None
    backing_path: str | None


def resolve_paths(stage, logical_paths: Iterable[str]) -> list[PathResolution]:
    """Resolve logical roots and descendants in input order, preserving duplicates.

    Nested instances return the nearest logical instance root, its prototype,
    and the final prototype backing prim. Ordinary populated prims map to
    themselves with None root fields. Missing backing prims return NOT_FOUND.
    Wait for relevant writes/population and exclude concurrent topology changes
    for a coherent latest-committed batch; resolve again after changes.
    No native result releases are needed. Raises OvstageError on call failure.
    """
    if isinstance(logical_paths, (str, bytes)):
        raise TypeError("logical_paths must be an iterable of path strings")
    lib, inst = _require(stage)
    inputs = list(logical_paths)
    with PathDictionary(stage) as paths:
        handles = (_b.ovx_primpath_t * len(inputs))(*(paths.intern_path(path) for path in inputs))
        results = (_b.ovstage_instancing_path_resolution_t * len(inputs))()
        stage._check(lib.ovstage_instancing_resolve_paths(inst, handles, len(inputs), results))

        def path_string(handle):
            return paths.path_to_string(handle) if handle else None

        return [PathResolution(PathKind(result.kind), path_string(result.instance_root),
                               path_string(result.prototype_root), path_string(result.backing_path))
                for result in results]


def available() -> bool:
    """Return whether the ovstage library loads in this environment.

    The instancing queries are members of the instancing family vtable of every
    instance, so a loaded library serves them. An implementation that does not
    serve a query answers ``OVSTAGE_ERROR_NOT_SUPPORTED`` at the call.
    """
    try:
        _b.load()
        return True
    except Exception:  # noqa: BLE001 — library not loadable in this environment
        return False


def _require(stage):
    return stage._api, stage._inst


def _consume_path_list(paths: PathDictionary, path_list: _b.ovx_primpath_list_t) -> list[str]:
    """Convert an owned native path list to strings and release its reference."""
    try:
        return paths.get_path_strings(path_list)
    finally:
        paths.destroy_path_list(path_list)


def get_instance_roots(stage, prototype_root: str) -> list[str]:
    """Return the instance-root paths that reference ``prototype_root``."""
    api, inst = _require(stage)
    with PathDictionary(stage) as paths:
        prototype = paths.intern_path(prototype_root)
        out = _b.ovx_primpath_list_t(0)
        stage._check(api.ovstage_instancing_get_instance_roots(inst, prototype, ctypes.byref(out)))
        return _consume_path_list(paths, out)


def get_instance_roots_batch(stage, prototype_roots: Iterable[str]) -> list[list[str]]:
    """Return one instance-root list per input prototype path, preserving order.

    Duplicate inputs receive independent results. The stage must remain alive
    for the synchronous call, and callers must exclude concurrent topology
    changes.

    All native result-list references are released before this function returns,
    including when native execution or conversion fails.

    Args:
        stage: Live ovstage instance to query.
        prototype_roots: Prototype-root paths to query.

    Returns:
        One list of instance-root path strings per input path.

    Raises:
        OvstageError: If the native query fails.
    """
    if isinstance(prototype_roots, (str, bytes)):
        raise TypeError("prototype_roots must be an iterable of path strings")
    lib, inst = _require(stage)
    if not hasattr(lib, "ovstage_instancing_get_instance_roots_batch"):
        raise OvstageError(_b.OVSTAGE_ERROR_NOT_SUPPORTED, "instancing batch query is unavailable")
    roots = list(prototype_roots)
    with PathDictionary(stage) as paths:
        handles = (_b.ovx_primpath_t * len(roots))(*(paths.intern_path(root) for root in roots))
        results = (_b.ovx_primpath_list_t * len(roots))()
        try:
            stage._check(lib.ovstage_instancing_get_instance_roots_batch(inst, handles, len(roots), results))
            return [paths.get_path_strings(result) for result in results]
        finally:
            active_error = sys.exc_info()[0] is not None
            cleanup_error = None
            for result in results:
                if result != _b.OVX_INVALID_PRIMPATH_LIST:
                    try:
                        paths.destroy_path_list(result)
                    except Exception as error:  # noqa: BLE001 — finish releasing all results
                        if cleanup_error is None:
                            cleanup_error = error
            if cleanup_error is not None and not active_error:
                raise cleanup_error


def get_prototype_root(stage, instance_root: str) -> str:
    """Return the prototype-root path referenced by ``instance_root``.

    Raises:
        OvstageError: If ``instance_root`` is not an instance root or the native
            query otherwise fails.
    """
    api, inst = _require(stage)
    with PathDictionary(stage) as paths:
        instance = paths.intern_path(instance_root)
        out = _b.ovx_primpath_t(0)
        stage._check(api.ovstage_instancing_get_prototype_root(inst, instance, ctypes.byref(out)))
        return paths.path_to_string(out)


def get_prototype_roots(stage) -> list[str]:
    """Return every scene-graph-instancing prototype-root path in ``stage``."""
    api, inst = _require(stage)
    with PathDictionary(stage) as paths:
        out = _b.ovx_primpath_list_t(0)
        stage._check(api.ovstage_instancing_get_prototype_roots(inst, ctypes.byref(out)))
        return _consume_path_list(paths, out)
