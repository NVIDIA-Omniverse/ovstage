# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""ovstage population: populate an ovstage instance from USD.

Thin wrappers over the ``ovstage_population_*`` C entry points (the USD ->
ovstage bridge in ``ovstage_population.h``). The application owns the ordinal
lifecycle and passes the current ordinal to the calls that carry one.

The surface follows the ovrtx Python conventions: every mutating entry point
comes as a blocking ``foo(...)`` that returns once the work has completed, plus
an asynchronous ``foo_async(...)`` that returns an :class:`Operation` to wait on
explicitly. Per-stage population state is created lazily on first use and
released automatically when the stage is destroyed (no manual detach).
"""

import ctypes
import math

from . import bindings as _b
from .types import OvstageError, PopulationDomain, check_ordinal, check_timeout

__all__ = [
    "Operation",
    "open_usd",
    "open_usd_async",
    "open_usd_from_string",
    "open_usd_from_string_async",
    "add_usd_reference",
    "add_usd_reference_async",
    "add_usd_reference_from_string",
    "add_usd_reference_from_string_async",
    "remove_usd",
    "remove_usd_async",
    "reset_usd",
    "reset_usd_async",
    "update_from_usd_time",
    "update_from_usd_time_async",
    "apply_usd_changes",
    "apply_usd_changes_async",
    "last_error",
    "available",
]


def available() -> bool:
    """True when the loaded ``libovstage`` exports the ovstage population bridge."""
    try:
        return hasattr(_b.load(), "ovstage_population_open_usd_from_file")
    except Exception:  # noqa: BLE001 — library not loadable in this environment
        return False


def last_error() -> str:
    """Thread-local detail for the latest population call on this thread."""
    return str(_b.load().ovstage_population_get_last_error())


def _require(stage):
    if not available():
        raise OvstageError(
            _b.OVSTAGE_ERROR_NOT_SUPPORTED, "libovstage was built without the ovstage population bridge"
        )
    return stage._lib, stage._inst


class Operation:
    """A handle to an enqueued (asynchronous) population operation.

    Mirrors the ovrtx ``Operation`` ergonomics: :meth:`wait` blocks until the op
    (and the ordinal-keyed ops before it) completes, returning the op's payload
    on success — a USD reference handle for an add-reference op, ``True`` for a
    void op — or ``None`` on timeout, and raising :class:`OvstageError` on
    failure. ``status`` is the enqueue status (``OVSTAGE_OK`` = accepted).
    """

    def __init__(self, stage, status: int, op_id: int, *, value=True, keepalive=None, name: str = ""):
        self._stage = stage
        self.status = int(status)
        self.op_id = int(op_id)
        self._value = value
        self._keepalive = keepalive  # holds ovx_string_t input buffers alive until waited
        self._name = name

    @property
    def ok(self) -> bool:
        return self.status == _b.OVSTAGE_OK

    def error_message(self) -> str:
        lib = self._stage._lib
        return str(lib.ovstage_population_get_last_op_error(self.op_id))

    def wait(self, timeout: int = _b.OVSTAGE_TIMEOUT_INFINITE):
        """Wait for completion. Returns the payload, ``None`` on timeout; raises on failure.

        :param timeout: max nanoseconds to wait; ``OVSTAGE_TIMEOUT_INFINITE``
            (default) blocks, ``0`` polls.
        :raises TypeError: if ``timeout`` is not an integer (e.g. ``None``).
        :raises ValueError: if ``timeout`` is negative or does not fit in uint64.
        """
        timeout = check_timeout(timeout)
        if self.status != _b.OVSTAGE_OK:
            raise OvstageError(self.status, last_error())
        lib, inst = self._stage._lib, self._stage._inst
        wait_result = _b.ovstage_population_op_wait_result_t()
        code = lib.ovstage_population_wait_op(inst, self.op_id, timeout, ctypes.byref(wait_result))
        if code == _b.OVSTAGE_ERROR_TIMEOUT:
            return None  # op still pending — keep the input buffers alive for the worker
        self._keepalive = None  # op resolved (success or failure); inputs no longer read
        # Per the wait contract, error_op_ids lists ops observed to have failed
        # even when the wait itself returns OK -- treat that as failure too
        # (the C waitPop helper checks exactly this).
        if code == _b.OVSTAGE_OK and wait_result.error_op_id_count == 0:
            return self._value
        if wait_result.error_op_id_count:
            failed_id = wait_result.error_op_ids[0]
            message = str(lib.ovstage_population_get_last_op_error(failed_id)) or last_error()
            raise OvstageError(code if code != _b.OVSTAGE_OK else _b.OVSTAGE_ERROR_OP_FAILED, message)
        raise OvstageError(code, self.error_message() or last_error())


def _enqueue(stage, name: str, result, *, value=True, keepalive=None) -> Operation:
    """Turn a population enqueue result into an :class:`Operation`, raising on reject."""
    if result.status != _b.OVSTAGE_OK:
        raise OvstageError(result.status, last_error())
    return Operation(stage, result.status, result.op_index, value=value, keepalive=keepalive, name=name)


# ── open (load + populate) ───────────────────────────────────────────────────
def open_usd(stage, path: str, ordinal: int = 1, time_code: float = math.nan,
             domains: int = PopulationDomain.RENDERING) -> None:
    """Open a USD file and populate the stage (blocking).

    ``time_code`` is in **seconds** (converted via the stage's
    ``timeCodesPerSecond``, like the C ``time`` parameter); ``math.nan`` (the
    default) evaluates at USD's Default time code. Wraps the C entry point
    ``ovstage_population_open_usd_from_file``.
    """
    open_usd_async(stage, path, ordinal, time_code, domains).wait()


def open_usd_async(stage, path: str, ordinal: int = 1, time_code: float = math.nan,
                   domains: int = PopulationDomain.RENDERING) -> Operation:
    """Open a USD file and populate the stage (asynchronous). See :func:`open_usd`."""
    lib, inst = _require(stage)
    path_s = _b.ovx_string_t(path)
    res = lib.ovstage_population_open_usd_from_file(inst, path_s, check_ordinal(ordinal), float(time_code), int(domains))
    return _enqueue(stage, "open_usd", res, keepalive=[path_s])


def open_usd_from_string(stage, usda: str, ordinal: int = 1, time_code: float = math.nan,
                         domains: int = PopulationDomain.RENDERING) -> None:
    """Open inline USDA content and populate the stage (blocking).

    ``time_code`` follows the same contract as :func:`open_usd`: seconds, with
    ``math.nan`` (the default) evaluating at USD's Default time code.
    """
    open_usd_from_string_async(stage, usda, ordinal, time_code, domains).wait()


def open_usd_from_string_async(stage, usda: str, ordinal: int = 1, time_code: float = math.nan,
                               domains: int = PopulationDomain.RENDERING) -> Operation:
    """Open inline USDA content and populate the stage (asynchronous). See :func:`open_usd_from_string`."""
    lib, inst = _require(stage)
    usda_s = _b.ovx_string_t(usda)
    res = lib.ovstage_population_open_usd_from_string(inst, usda_s, check_ordinal(ordinal), float(time_code), int(domains))
    return _enqueue(stage, "open_usd_from_string", res, keepalive=[usda_s])


# ── add / remove USD references (USD-source edits; no ordinal) ────────────────
def add_usd_reference(stage, ref_file_path: str, target_path: str) -> int:
    """Add a USD file as a reference at ``target_path`` (blocking). Returns the handle.

    The merge is additive in every case; ``target_path`` selects its shape: the root
    ``"/"`` merges the layer's top-level prims into the stage, an existing prim path
    adds the reference onto that prim (leaving its prior content), and a not-yet-existing
    prim path defines a new prim there. Call :func:`apply_usd_changes` afterwards to
    reflect it into the stage.
    """
    return add_usd_reference_async(stage, ref_file_path, target_path).wait()


def add_usd_reference_async(stage, ref_file_path: str, target_path: str) -> Operation:
    """Add a USD file as a reference at ``target_path`` (asynchronous)."""
    lib, inst = _require(stage)
    path_s = _b.ovx_string_t(ref_file_path)
    target_s = _b.ovx_string_t(target_path)
    handle = _b.ovstage_population_usd_reference_handle_t()
    res = lib.ovstage_population_add_usd_reference_from_file(inst, path_s, target_s, ctypes.byref(handle))
    return _enqueue(stage, "add_usd_reference", res, value=int(handle.value), keepalive=[path_s, target_s])


def add_usd_reference_from_string(stage, ref_str: str, target_path: str) -> int:
    """Add inline USDA content as a reference at ``target_path`` (blocking). Returns the handle.

    Same additive ``target_path`` semantics as :func:`add_usd_reference` (root ``"/"``
    merge, overlay onto an existing prim, or a new prim), but the layer is provided as
    inline USDA text instead of a file.
    """
    return add_usd_reference_from_string_async(stage, ref_str, target_path).wait()


def add_usd_reference_from_string_async(stage, ref_str: str, target_path: str) -> Operation:
    """Add inline USDA content as a reference at ``target_path`` (asynchronous)."""
    lib, inst = _require(stage)
    usda_s = _b.ovx_string_t(ref_str)
    target_s = _b.ovx_string_t(target_path)
    handle = _b.ovstage_population_usd_reference_handle_t()
    res = lib.ovstage_population_add_usd_reference_from_string(inst, usda_s, target_s, ctypes.byref(handle))
    return _enqueue(stage, "add_usd_reference_from_string", res, value=int(handle.value),
                    keepalive=[usda_s, target_s])


def remove_usd(stage, handle: int) -> None:
    """Remove a USD reference previously added by ``add_usd_reference*`` (blocking)."""
    remove_usd_async(stage, handle).wait()


def remove_usd_async(stage, handle: int) -> Operation:
    """Remove a USD reference previously added by ``add_usd_reference*`` (asynchronous)."""
    lib, inst = _require(stage)
    res = lib.ovstage_population_remove_usd_reference(inst, int(handle))
    return _enqueue(stage, "remove_usd", res)


def reset_usd(stage) -> None:
    """Clear all USD source content from the stage (blocking)."""
    reset_usd_async(stage).wait()


def reset_usd_async(stage) -> Operation:
    """Clear all USD source content from the stage (asynchronous)."""
    lib, inst = _require(stage)
    res = lib.ovstage_population_reset_usd(inst)
    return _enqueue(stage, "reset_usd", res)


# ── propagate USD edits / time into the stage (carry an ordinal) ──────────────
def update_from_usd_time(stage, ordinal: int, time_code: float) -> None:
    """Advance time and propagate time-sampled attribute changes (blocking).

    ``time_code`` is in **seconds** (converted via the stage's
    ``timeCodesPerSecond``, like the C ``time`` parameter). Wraps the C entry
    point ``ovstage_population_apply_usd_time``.
    """
    update_from_usd_time_async(stage, ordinal, time_code).wait()


def update_from_usd_time_async(stage, ordinal: int, time_code: float) -> Operation:
    """Advance time and propagate time-sampled changes (asynchronous). See :func:`update_from_usd_time`."""
    lib, inst = _require(stage)
    res = lib.ovstage_population_apply_usd_time(inst, check_ordinal(ordinal), float(time_code))
    return _enqueue(stage, "update_from_usd_time", res)


def apply_usd_changes(stage, ordinal: int = 1) -> None:
    """Propagate USD edits accumulated since the last call into the stage (blocking)."""
    apply_usd_changes_async(stage, ordinal).wait()


def apply_usd_changes_async(stage, ordinal: int = 1) -> Operation:
    """Propagate USD edits accumulated since the last call into the stage (asynchronous)."""
    lib, inst = _require(stage)
    res = lib.ovstage_population_apply_usd_changes(inst, check_ordinal(ordinal))
    return _enqueue(stage, "apply_usd_changes", res)
