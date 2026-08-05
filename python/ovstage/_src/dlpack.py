# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""DLPack tensor structures for zero-copy data interchange.

ctypes wrappers for the DLPack 1.3 structs declared in
``ovstage/public/include/dlpack/dlpack.h`` (which forwards to the shared
``ovx/dlpack/dlpack.h``). ovstage carries all attribute tensor data as
``DLTensor``, so the binding both *consumes* DLTensors (read/map results) and
*produces* them (the copy-in write path).

Two interchange paths are provided:

* **NumPy convenience** — :func:`make_dltensor` wraps a numpy array (CPU) and
  :func:`dltensor_to_numpy` returns a zero-copy ``np.ctypeslib`` view over a CPU
  buffer.
* **Standard DLPack protocol** — :meth:`DLTensor.from_dlpack` ingests *any*
  producer exposing ``__dlpack__`` (numpy / warp / torch / cupy / jax) on CPU
  **or** CUDA, and :class:`ManagedDLTensor` (via ``__dlpack__`` /
  ``__dlpack_device__``) exports an ovstage tensor so those same libraries can
  consume it zero-copy (``np.from_dlpack(managed)`` / ``wp.from_dlpack(...)``).
  This is what lets a GPU producer's device buffer cross into/out of ovstage
  without a host round-trip.
"""

import ctypes
import operator
from typing import Any, Callable, Optional

__all__ = [
    "DLDeviceType",
    "DLDataTypeCode",
    "DLDevice",
    "DLDataType",
    "DLTensor",
    "DLManagedTensor",
    "DLPackVersion",
    "DLManagedTensorVersioned",
    "ManagedDLTensor",
    "DLPACK_MAJOR_VERSION",
    "DLPACK_MINOR_VERSION",
    "DLPACK_FLAG_BITMASK_READ_ONLY",
    "make_dltensor",
    "dltensor_to_numpy",
    "numpy_to_dldatatype",
]

# DLPack version numbers (aligned with C header dlpack.h)
DLPACK_MAJOR_VERSION = 1
DLPACK_MINOR_VERSION = 3

# DLPack capsule name strings (protocol export/import).
_c_str_dltensor = b"dltensor"
_c_str_used_dltensor = b"used_dltensor"
_c_str_dltensor_versioned = b"dltensor_versioned"
_c_str_used_dltensor_versioned = b"used_dltensor_versioned"

# DLPack 1.0+ flag bitmasks
DLPACK_FLAG_BITMASK_READ_ONLY = 1 << 0
DLPACK_FLAG_BITMASK_IS_COPIED = 1 << 1
DLPACK_FLAG_BITMASK_IS_SUBBYTE_TYPE_PADDED = 1 << 2


class DLDeviceType(ctypes.c_int):
    """The enum encoding the type of device where DLTensor memory lives."""

    kDLCPU = 1
    kDLCUDA = 2
    kDLCUDAHost = 3
    kDLOpenCL = 4
    kDLVulkan = 7
    kDLMetal = 8
    kDLVPI = 9
    kDLROCM = 10
    kDLROCMHost = 11
    kDLExtDev = 12
    kDLCUDAManaged = 13
    kDLOneAPI = 14
    kDLWebGPU = 15
    kDLHexagon = 16
    kDLMAIA = 17
    kDLTrn = 18

    def __str__(self):
        return {
            self.kDLCPU: "CPU",
            self.kDLCUDA: "CUDA",
            self.kDLCUDAHost: "CUDAHost",
            self.kDLCUDAManaged: "CUDAManaged",
        }.get(self.value, f"Device{self.value}")


class DLDataTypeCode(ctypes.c_uint8):
    """An integer encoding the category of a DLTensor element's data type."""

    kDLInt = 0
    kDLUInt = 1
    kDLFloat = 2
    kDLOpaqueHandle = 3
    kDLBfloat = 4
    kDLComplex = 5
    kDLBool = 6


class DLDevice(ctypes.Structure):
    """Device where DLTensor memory is allocated."""

    _fields_ = [
        ("device_type", DLDeviceType),
        ("device_id", ctypes.c_int32),
    ]


class DLDataType(ctypes.Structure):
    """Descriptor of the element data type of a DLTensor."""

    _fields_ = [
        ("code", ctypes.c_uint8),
        ("bits", ctypes.c_uint8),
        ("lanes", ctypes.c_uint16),
    ]

    def __repr__(self) -> str:
        return f"DLDataType(code={self.code}, bits={self.bits}, lanes={self.lanes})"


class DLTensor(ctypes.Structure):
    """Plain C tensor object; does not manage memory."""

    _fields_ = [
        ("data", ctypes.c_void_p),
        ("device", DLDevice),
        ("ndim", ctypes.c_int32),
        ("dtype", DLDataType),
        ("shape", ctypes.POINTER(ctypes.c_int64)),
        ("strides", ctypes.POINTER(ctypes.c_int64)),
        ("byte_offset", ctypes.c_uint64),
    ]

    @property
    def shape_tuple(self) -> tuple:
        return tuple(self.shape[i] for i in range(self.ndim)) if self.shape else ()

    @classmethod
    def from_dlpack(cls, obj: Any, stream: Optional[int] = None) -> "DLTensor":
        """Build a :class:`DLTensor` viewing any object implementing the DLPack protocol.

        Accepts numpy / warp / torch / cupy / jax tensors (anything exposing
        ``__dlpack__``) resident on **CPU or CUDA** and aliases their memory
        zero-copy, so a GPU producer's device buffer can be handed straight to
        ovstage without a host round-trip. Shape and strides are deep-copied,
        while the consumed producer descriptor/deleter and source object are
        retained on the returned tensor so the aliased buffer remains valid until
        the returned tensor is destroyed. The caller must keep the returned tensor
        alive until the consuming op completes (``Stage.write_attribute`` does
        this via the :class:`~ovstage.Operation` keepalive).

        ``stream`` is forwarded to a CUDA producer so it can synchronize the
        exporting stream against the consumer; it is ignored for CPU tensors and
        for producers that do not advertise ``__dlpack_device__``.
        """
        if not hasattr(obj, "__dlpack__"):
            raise TypeError(f"object of type {type(obj).__name__} does not support the DLPack protocol")

        # Only forward the consumer stream for CUDA tensors — CPU __dlpack__ rejects a stream arg.
        pass_stream = False
        if stream is not None and hasattr(obj, "__dlpack_device__"):
            device_type, _ = obj.__dlpack_device__()
            pass_stream = device_type in (DLDeviceType.kDLCUDA, DLDeviceType.kDLCUDAManaged)
        capsule = obj.__dlpack__(stream=stream) if pass_stream else obj.__dlpack__()

        ptr = PyCapsule_GetPointer(capsule, _c_str_dltensor)
        if not ptr:
            raise RuntimeError("failed to read the DLManagedTensor pointer from the DLPack capsule")
        managed = ctypes.cast(ptr, ctypes.POINTER(DLManagedTensor)).contents
        src = managed.dl_tensor

        result = cls()
        result.data = src.data
        result.device = src.device
        result.ndim = src.ndim
        result.dtype = src.dtype
        result.byte_offset = src.byte_offset
        # Keepalive: the producer owns the backing buffer; tie its lifetime to ours.
        result._source_obj = obj
        if src.ndim > 0 and src.shape:
            result._shape_storage = (ctypes.c_int64 * src.ndim)(*(src.shape[i] for i in range(src.ndim)))
            result.shape = ctypes.cast(result._shape_storage, ctypes.POINTER(ctypes.c_int64))
        else:
            result.shape = None
        if src.ndim > 0 and src.strides:
            result._strides_storage = (ctypes.c_int64 * src.ndim)(*(src.strides[i] for i in range(src.ndim)))
            result.strides = ctypes.cast(result._strides_storage, ctypes.POINTER(ctypes.c_int64))
        else:
            result.strides = None

        # Consume the capsule per the DLPack protocol: rename it so the capsule
        # destructor no-ops, then keep the producer's managed-tensor descriptor
        # alive until this returned DLTensor dies. Some producers tie export
        # resources to that deleter, so calling it here would release memory that
        # an asynchronous ovstage write may still read.
        if PyCapsule_SetName(capsule, _c_str_used_dltensor) != 0:
            raise RuntimeError("failed to mark the DLPack capsule as consumed")
        if managed.deleter:
            result._dlpack_owner = _ConsumedDLManagedTensor(ptr, managed.deleter)
        return result


class DLPackVersion(ctypes.Structure):
    _fields_ = [
        ("major", ctypes.c_uint32),
        ("minor", ctypes.c_uint32),
    ]


_DLPACK_DELETER = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class DLManagedTensor(ctypes.Structure):
    """Legacy (pre-1.0) managed tensor. dl_tensor is at offset 0."""

    _fields_ = [
        ("dl_tensor", DLTensor),
        ("manager_ctx", ctypes.c_void_p),
        ("deleter", _DLPACK_DELETER),
    ]


class DLManagedTensorVersioned(ctypes.Structure):
    """DLPack 1.x versioned managed tensor (ovstage write managed-tensor path)."""

    _fields_ = [
        ("version", DLPackVersion),
        ("manager_ctx", ctypes.c_void_p),
        ("deleter", _DLPACK_DELETER),
        ("flags", ctypes.c_uint64),
        ("dl_tensor", DLTensor),
    ]


class _ConsumedDLManagedTensor:
    """Own a consumed producer DLManagedTensor until the returned DLTensor dies."""

    __slots__ = ("_ptr", "_deleter", "_released")

    def __init__(self, ptr, deleter) -> None:
        self._ptr = ptr
        self._deleter = deleter
        self._released = False

    def release(self) -> None:
        if self._released:
            return
        self._released = True
        ptr = self._ptr
        deleter = self._deleter
        self._ptr = None
        self._deleter = None
        if ptr and deleter:
            deleter(ptr)

    def __del__(self) -> None:
        try:
            self.release()
        except Exception:
            pass


# ── DLPack protocol export (capsule creation) ───────────────────────────────
# Python C-API bindings used to build/consume DLPack PyCapsules.
# AUTOREMOVE: BEGIN
# Derived from the sibling ovrtx implementation (ovrtx/_src/dlpack.py), but
# deliberately *diverged* as of OMPE-102796: ovrtx still allocates with PyMem_Malloc
# and installs a Python ctypes callback as the DLPack deleter, which reproduces the
# bug fixed here (writing into a read-only np.from_dlpack view raises "SystemError:
# error return without exception set" and strands the capsule context). Do not
# resync this file to ovrtx until the same fix lands there.
# AUTOREMOVE: END
# The DLManagedTensor block is allocated and freed in the *raw* allocator domain.
# PyMem_Malloc/PyMem_Free would require the GIL, and the DLPack deleter below is a
# bare C function pointer that cannot acquire it the way a ctypes callback does, so
# a consumer releasing the tensor off-GIL would be calling a GIL-requiring free.
# The two must stay a matched pair: allocate raw, free raw.
PyMem_RawMalloc = ctypes.pythonapi.PyMem_RawMalloc
PyMem_RawMalloc.argtypes = [ctypes.c_size_t]
PyMem_RawMalloc.restype = ctypes.c_void_p

PyMem_RawFree = ctypes.pythonapi.PyMem_RawFree
PyMem_RawFree.argtypes = [ctypes.c_void_p]
PyMem_RawFree.restype = None

Py_IncRef = ctypes.pythonapi.Py_IncRef
Py_IncRef.argtypes = [ctypes.py_object]
Py_IncRef.restype = None

Py_DecRef = ctypes.pythonapi.Py_DecRef
Py_DecRef.argtypes = [ctypes.py_object]
Py_DecRef.restype = None

PyCapsule_Destructor = ctypes.CFUNCTYPE(None, ctypes.c_void_p)

PyCapsule_New = ctypes.pythonapi.PyCapsule_New
PyCapsule_New.argtypes = [ctypes.c_void_p, ctypes.c_char_p, PyCapsule_Destructor]
PyCapsule_New.restype = ctypes.py_object

PyCapsule_GetPointer = ctypes.pythonapi.PyCapsule_GetPointer
PyCapsule_GetPointer.argtypes = [ctypes.py_object, ctypes.c_char_p]
PyCapsule_GetPointer.restype = ctypes.c_void_p

PyCapsule_SetName = ctypes.pythonapi.PyCapsule_SetName
PyCapsule_SetName.argtypes = [ctypes.py_object, ctypes.c_char_p]
PyCapsule_SetName.restype = ctypes.c_int

# Raw c_void_p variants for use inside the capsule destructor, where the capsule
# refcount is already zero and py_object would try to re-incref it.
_PyCapsule_IsValid_raw = ctypes.pythonapi["PyCapsule_IsValid"]
_PyCapsule_IsValid_raw.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_PyCapsule_IsValid_raw.restype = ctypes.c_int

_PyCapsule_GetPointer_raw = ctypes.pythonapi["PyCapsule_GetPointer"]
_PyCapsule_GetPointer_raw.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_PyCapsule_GetPointer_raw.restype = ctypes.c_void_p

PyCapsule_SetContext = ctypes.pythonapi.PyCapsule_SetContext
PyCapsule_SetContext.argtypes = [ctypes.py_object, ctypes.c_void_p]
PyCapsule_SetContext.restype = ctypes.c_int

_PyCapsule_GetContext_raw = ctypes.pythonapi["PyCapsule_GetContext"]
_PyCapsule_GetContext_raw.argtypes = [ctypes.c_void_p]
_PyCapsule_GetContext_raw.restype = ctypes.c_void_p

# Bound here, at import time, on purpose: see _drain_pending_exception.
_PyErr_Clear = ctypes.pythonapi.PyErr_Clear
_PyErr_Clear.argtypes = []
_PyErr_Clear.restype = None


# A DLPack deleter must not execute Python: the consumer can invoke it while a Python
# exception is propagating (e.g. numpy rejecting an in-place write into a read-only
# view). ctypes cannot enter a Python callback with the error indicator set, so the
# callback aborts at its first call, its cleanup never runs, and the pending exception
# is reported unraisable and cleared -- surfacing to the caller as "SystemError: error
# return without exception set". PyMem_RawFree has a compatible void(*)(void*)
# signature and frees the block with no Python involved.
_C_FREE_DELETER = ctypes.cast(PyMem_RawFree, _DLPACK_DELETER)


def _drain_pending_exception() -> None:
    """Clear any exception pending on entry to a ctypes callback.

    The capsule destructor is itself a ctypes callback, so it hits the same wall the
    DLPack deleter did: with an exception already set, CPython aborts the callback at
    its first C call and the cleanup below never runs, stranding the capsule context
    and the tensor block. Clearing the error indicator up front is what makes that
    cleanup deterministic.

    This does not preserve the caller's exception, and cannot: ctypes reports and
    clears whatever is set when a callback returns, so nothing can be handed back
    across that boundary.

    ``PyErr_Clear`` is bound at *import* time deliberately. Resolving it through
    ``ctypes.pythonapi`` here would itself be a C call and would trip on the very
    exception being drained. Detecting the pending exception first — by provoking the
    ``SystemError`` that ``_Py_CheckFunctionResult`` raises for a call that returns a
    result with the error set — is not viable either: CPython 3.11+ specializes a
    warmed-up builtin call site to an opcode whose equivalent check is an ``assert``
    compiled out of release builds, so that guard silently stops firing after the
    first few calls.
    """
    _PyErr_Clear()


class _CapsuleCtx:
    """Keepalive for a capsule's callbacks and the caller's ``manager_ctx``."""

    __slots__ = ("manager_ctx", "deleter_callback", "capsule_destructor")

    def __init__(self, manager_ctx: Any, deleter_callback: Optional[Callable]) -> None:
        self.manager_ctx = manager_ctx
        self.deleter_callback = deleter_callback
        self.capsule_destructor = None


def _to_dlpack_capsule(
    dl_tensor: DLTensor,
    manager_ctx: Any,
    deleter_callback: Optional[Callable] = None,
    *,
    versioned: bool = False,
    readonly: bool = True,
) -> Any:
    """Create a DLPack ``PyCapsule`` wrapping ``dl_tensor``.

    ``manager_ctx`` is a Python object kept alive for the lifetime of the capsule;
    ``deleter_callback(manager_ctx)`` — if given — runs when the capsule is
    destroyed. Note that a *consumed* capsule is destroyed as soon as the consumer
    has taken ownership of the managed tensor, which is generally before that
    consumer releases the tensor: the DLPack deleter itself must not run Python
    (see ``_C_FREE_DELETER``), so it cannot drive a Python callback at release
    time. Callers needing cleanup tied to the consumer's lifetime must arrange it
    themselves. ``versioned`` selects the DLPack 1.0
    ``DLManagedTensorVersioned`` layout (with a read-only flag) over the legacy
    ``DLManagedTensor``. Vector dtypes (``lanes > 1``) are expanded by adding
    exactly one trailing shape dimension and setting ``lanes = 1``, which is how
    array libraries consume them. This is lane expansion, not reconstruction of
    a convenience write shape: a fixed matrix stored as ``shape=(N,)``,
    ``lanes=16`` exports as ``(N, 16)``, not ``(N, 4, 4)``.
    """
    actual_ndim = dl_tensor.ndim + (1 if dl_tensor.dtype.lanes > 1 else 0)

    if versioned:
        ManagedTensor = DLManagedTensorVersioned
        capsule_name = _c_str_dltensor_versioned
    else:
        ManagedTensor = DLManagedTensor
        capsule_name = _c_str_dltensor

    managed_size = ctypes.sizeof(ManagedTensor)
    shape_size = actual_ndim * ctypes.sizeof(ctypes.c_int64)
    mem_ptr = PyMem_RawMalloc(managed_size + shape_size)
    if not mem_ptr:
        raise MemoryError("failed to allocate DLManagedTensor")

    managed_tensor = ManagedTensor.from_address(mem_ptr)
    if versioned:
        managed_tensor.version.major = DLPACK_MAJOR_VERSION
        managed_tensor.version.minor = DLPACK_MINOR_VERSION
        managed_tensor.flags = DLPACK_FLAG_BITMASK_READ_ONLY if readonly else 0

    managed_tensor.dl_tensor.data = dl_tensor.data
    managed_tensor.dl_tensor.device = dl_tensor.device
    managed_tensor.dl_tensor.ndim = actual_ndim
    managed_tensor.dl_tensor.byte_offset = dl_tensor.byte_offset
    managed_tensor.dl_tensor.dtype.code = dl_tensor.dtype.code
    managed_tensor.dl_tensor.dtype.bits = dl_tensor.dtype.bits
    managed_tensor.dl_tensor.dtype.lanes = 1 if dl_tensor.dtype.lanes > 1 else dl_tensor.dtype.lanes

    shape_ptr = ctypes.cast(mem_ptr + managed_size, ctypes.POINTER(ctypes.c_int64))
    for i in range(dl_tensor.ndim):
        shape_ptr[i] = dl_tensor.shape[i]
    if dl_tensor.dtype.lanes > 1:
        shape_ptr[dl_tensor.ndim] = dl_tensor.dtype.lanes
    managed_tensor.dl_tensor.shape = shape_ptr
    managed_tensor.dl_tensor.strides = None

    # One manual ref keeps _capsule_ctx (and its CFUNCTYPE) alive: held by the capsule
    # destructor stored in the capsule context.
    _capsule_ctx = _CapsuleCtx(manager_ctx, deleter_callback)

    # manager_ctx stays NULL. It exists for a deleter that needs producer state, and
    # this deleter is a bare C free that needs none — it is never read back. Storing
    # id(_capsule_ctx) here would outlive what it points at: the capsule destructor
    # drops the context's only reference when the capsule dies, which for a consumed
    # capsule is while the consumer still owns this block. The destructor reaches the
    # context through the capsule's own context pointer instead, which it releases in
    # the same call.
    managed_tensor.manager_ctx = None

    @PyCapsule_Destructor
    def capsule_destructor(capsule_ptr):
        # Runs before anything else: an unconsumed capsule can be dropped while an
        # exception propagates, which would otherwise abort this callback and leak
        # both the context and the tensor block. This makes the cleanup below run; it
        # does not rescue the caller's exception, which ctypes discards at the callback
        # boundary either way.
        _drain_pending_exception()
        # Both cleanup steps must run even if the caller's deleter_callback raises,
        # otherwise a failing callback strands the context and the tensor block. The
        # exception still propagates (as it did when the deleter owned this call), so
        # a broken callback is reported rather than silently swallowed.
        try:
            ctx_id = _PyCapsule_GetContext_raw(capsule_ptr)
            if ctx_id:
                ctx = ctypes.cast(ctx_id, ctypes.py_object).value
                try:
                    if ctx.deleter_callback is not None:
                        ctx.deleter_callback(ctx.manager_ctx)
                finally:
                    Py_DecRef(ctx)
        finally:
            # Skip the deleter when the capsule was already consumed (renamed by the consumer).
            if _PyCapsule_IsValid_raw(capsule_ptr, capsule_name):
                managed_ptr = _PyCapsule_GetPointer_raw(capsule_ptr, capsule_name)
                if managed_ptr:
                    mt = ManagedTensor.from_address(managed_ptr)
                    if mt.deleter:
                        mt.deleter(managed_ptr)

    _capsule_ctx.capsule_destructor = capsule_destructor
    managed_tensor.deleter = _C_FREE_DELETER

    capsule = PyCapsule_New(mem_ptr, capsule_name, capsule_destructor)
    Py_IncRef(_capsule_ctx)  # held by capsule_destructor
    PyCapsule_SetContext(capsule, id(_capsule_ctx))
    return capsule


class ManagedDLTensor:
    """A DLPack-exportable view of an ovstage tensor.

    Obtained from :meth:`ovstage.ReadGroup.dlpack` / :meth:`ovstage.MapGroup.dlpack`.
    Pass the instance to ``np.from_dlpack()`` / ``wp.from_dlpack()`` /
    ``torch.from_dlpack()`` for zero-copy access (CPU or CUDA), or call
    :meth:`numpy` for a CPU numpy view.

    The tensor data is **borrowed** from ovstage and valid only until the owning
    read/map group is released — copy it if it must outlive the read. A read
    group is exported ``readonly=True``; a (writable) map group ``readonly=False``.
    DLPack consumers see one trailing dimension added for a multi-lane dtype;
    for example, a raw fixed matrix ``shape=(N,)``, ``lanes=16`` is exported as
    ``(N, 16)`` rather than as any convenience input shape.
    ``ManagedDLTensor.shape`` and its representation report the raw, unexpanded
    ``(N,)`` shape; the trailing lane axis materializes only in a DLPack consumer
    such as ``np.from_dlpack()``.

    ``manager_ctx`` is retained by this object and by any capsule it exports, but a
    capsule is destroyed as soon as a consumer takes ownership of it — which is
    *before* that consumer releases the tensor. The DLPack deleter runs no Python
    (see ``_C_FREE_DELETER``), so it cannot release a Python reference at that later
    point. If ``manager_ctx`` is the sole owner of the backing memory, keep this
    ``ManagedDLTensor`` alive for as long as the consumer's view is used::

        managed = ManagedDLTensor(tensor, manager_ctx=owner)
        view = np.from_dlpack(managed)  # `managed` must outlive `view`

    This does not invalidate ``np.from_dlpack(group.dlpack(0))`` for ovstage
    read/map groups: their ``manager_ctx`` does not own the backing allocation,
    and the view remains valid while the owning read/map operation is alive.
    """

    def __init__(
        self,
        dl_tensor: DLTensor,
        manager_ctx: Any,
        deleter_callback: Optional[Callable] = None,
        readonly: bool = True,
    ):
        self._dl_tensor = dl_tensor
        self._manager_ctx = manager_ctx
        self._deleter_callback = deleter_callback
        self._cleanup_done = False
        self._readonly = readonly

    @property
    def shape(self) -> tuple:
        return tuple(self._dl_tensor.shape[i] for i in range(self._dl_tensor.ndim))

    @property
    def ndim(self) -> int:
        return self._dl_tensor.ndim

    @property
    def dtype(self) -> DLDataType:
        return self._dl_tensor.dtype

    @property
    def data(self) -> int:
        return self._dl_tensor.data

    @property
    def device(self) -> DLDevice:
        return self._dl_tensor.device

    @property
    def raw_dltensor(self) -> DLTensor:
        return self._dl_tensor

    def numpy(self):
        """Zero-copy numpy view (CPU tensors only)."""
        import numpy as np

        return np.from_dlpack(self)

    def __dlpack_device__(self) -> tuple:
        return (self._dl_tensor.device.device_type.value, self._dl_tensor.device.device_id)

    def __dlpack__(
        self,
        *,
        stream: Optional[int] = None,
        max_version: Optional[tuple] = None,
        dl_device: Optional[tuple] = None,
        copy: Optional[bool] = None,
    ) -> Any:
        """Return a DLPack capsule. ``stream`` is accepted but ignored — GPU
        synchronization is the caller's responsibility (see the dlpack skill)."""
        _ = stream
        if copy is True:
            raise BufferError("copy=True is not supported")
        if dl_device is not None:
            try:
                requested_device = (int(dl_device[0]), int(dl_device[1]))
            except (TypeError, IndexError, ValueError) as exc:
                raise TypeError("dl_device must be a (device_type, device_id) tuple") from exc
            current_device = self.__dlpack_device__()
            if requested_device != current_device:
                raise BufferError(
                    f"dl_device={requested_device!r} does not match tensor device "
                    f"{current_device!r}; cross-device copy is not supported"
                )
        # Versioned capsule only when the consumer supports the same major and >= (1, 0);
        # minor bumps are ABI-compatible. NumPy 2.1+ requests (1, 0) then honors the RO flag.
        use_versioned = (
            max_version is not None and max_version[0] == DLPACK_MAJOR_VERSION and max_version >= (1, 0)
        )

        if self._deleter_callback is not None:
            original_cb = self._deleter_callback

            def wrapped_cb(ctx, _self=self, _cb=original_cb):
                _cb(ctx)
                _self._cleanup_done = True

        else:
            wrapped_cb = None

        return _to_dlpack_capsule(
            self._dl_tensor, self._manager_ctx, wrapped_cb,
            versioned=use_versioned, readonly=self._readonly,
        )

    def __del__(self):
        if not self._cleanup_done and self._deleter_callback is not None:
            try:
                self._deleter_callback(self._manager_ctx)
                self._cleanup_done = True
            except Exception:
                pass

    def __repr__(self) -> str:
        return f"ManagedDLTensor(shape={self.shape}, dtype={self.dtype!r}, device={self.device})"


# ── numpy <-> DLPack helpers ───────────────────────────────────────────────
#
# (code, bits) -> (numpy dtype name, ctypes scalar). lanes are folded into the
# flat element count rather than the numpy dtype, matching how the C++ tests
# index the raw storage (e.g. ``out[row * componentCount + component]``).
_DL_TO_CTYPE = {
    (DLDataTypeCode.kDLFloat, 16): ctypes.c_uint16,  # float16: view as raw bits
    (DLDataTypeCode.kDLFloat, 32): ctypes.c_float,
    (DLDataTypeCode.kDLFloat, 64): ctypes.c_double,
    (DLDataTypeCode.kDLInt, 8): ctypes.c_int8,
    (DLDataTypeCode.kDLInt, 16): ctypes.c_int16,
    (DLDataTypeCode.kDLInt, 32): ctypes.c_int32,
    (DLDataTypeCode.kDLInt, 64): ctypes.c_int64,
    (DLDataTypeCode.kDLUInt, 8): ctypes.c_uint8,
    (DLDataTypeCode.kDLUInt, 16): ctypes.c_uint16,
    (DLDataTypeCode.kDLUInt, 32): ctypes.c_uint32,
    (DLDataTypeCode.kDLUInt, 64): ctypes.c_uint64,
    (DLDataTypeCode.kDLBool, 8): ctypes.c_bool,
}

_NUMPY_TO_DL = {
    "float16": (DLDataTypeCode.kDLFloat, 16),
    "float32": (DLDataTypeCode.kDLFloat, 32),
    "float64": (DLDataTypeCode.kDLFloat, 64),
    "int8": (DLDataTypeCode.kDLInt, 8),
    "int16": (DLDataTypeCode.kDLInt, 16),
    "int32": (DLDataTypeCode.kDLInt, 32),
    "int64": (DLDataTypeCode.kDLInt, 64),
    "uint8": (DLDataTypeCode.kDLUInt, 8),
    "uint16": (DLDataTypeCode.kDLUInt, 16),
    "uint32": (DLDataTypeCode.kDLUInt, 32),
    "uint64": (DLDataTypeCode.kDLUInt, 64),
    "bool": (DLDataTypeCode.kDLBool, 8),
}


def numpy_to_dldatatype(np_dtype, lanes: int = 1) -> DLDataType:
    """Build a :class:`DLDataType` from a numpy dtype (with optional vector lanes).

    ``lanes`` must be an integer in ``[1, 65535]``: the value lands in the DLPack
    ``uint16`` lane field, so anything outside that range raises
    :class:`ValueError` (:class:`TypeError` for non-integers) instead of silently
    wrapping to a bogus lane count (e.g. ``-1`` becoming ``65535``).
    """
    lanes = operator.index(lanes)
    if not 1 <= lanes <= 0xFFFF:
        raise ValueError(f"DLDataType lanes must be in [1, 65535], got {lanes}")
    name = str(np_dtype)
    if name not in _NUMPY_TO_DL and np_dtype is not None:
        # A numpy scalar *type* (np.float32) stringifies as "<class 'numpy.float32'>",
        # not "float32". Normalize type objects, char codes ("f4") and builtins (float)
        # through np.dtype; the fast path above keeps numpy optional for callers that
        # already pass a dtype name. None is excluded: np.dtype(None) is float64 ("the
        # default dtype"), and this factory must describe a buffer exactly, so an unset
        # dtype has to stay an error rather than silently become f8.
        try:
            import numpy as np

            name = str(np.dtype(np_dtype))
        except Exception:
            pass
    if name not in _NUMPY_TO_DL:
        raise ValueError(f"Unsupported numpy dtype for DLPack: {np_dtype!r}")
    code, bits = _NUMPY_TO_DL[name]
    return DLDataType(code=code, bits=bits, lanes=lanes)


def _compact_row_major_strides(shape: tuple) -> tuple:
    result = [0] * len(shape)
    stride = 1
    for dim in range(len(shape) - 1, -1, -1):
        result[dim] = stride
        stride *= shape[dim]
    return tuple(result)


def _fold_dlpack_producer_layout(
    tensor: DLTensor,
    *,
    dtype: Optional[DLDataType],
    shape: Optional[list],
    ndim: Optional[int],
    strides: Optional[list],
) -> DLTensor:
    """Apply a validated trailing-shape-to-lanes fold to a consumed producer."""
    if dtype is None:
        raise ValueError(
            "DLPack producer layout overrides require dtype; only validated "
            "trailing-dimension-to-lanes folds are supported"
        )
    if not isinstance(dtype, DLDataType):
        raise TypeError("dtype must be a DLDataType")

    source_shape = tensor.shape_tuple
    if tensor.ndim < 0 or (tensor.ndim > 0 and not tensor.shape) or any(dim < 0 for dim in source_shape):
        raise ValueError("cannot override an invalid DLPack producer shape")
    if tensor.strides:
        source_strides = tuple(tensor.strides[i] for i in range(tensor.ndim))
        if source_strides != _compact_row_major_strides(source_shape):
            raise ValueError("DLPack producer layout overrides require compact row-major source strides")

    source_dtype = tensor.dtype
    if source_dtype.bits == 0 or dtype.bits == 0:
        raise ValueError(
            "DLPack producer dtype override requires positive source and requested bit widths "
            f"(source={source_dtype.bits}, requested={dtype.bits})"
        )
    if dtype.code != source_dtype.code or dtype.bits != source_dtype.bits:
        raise ValueError(
            "DLPack producer dtype override must preserve the source base type and bit width "
            f"(source=({source_dtype.code}, {source_dtype.bits}), "
            f"requested=({dtype.code}, {dtype.bits}))"
        )
    if source_dtype.lanes == 0 or dtype.lanes == 0 or dtype.lanes < source_dtype.lanes:
        raise ValueError("DLPack producer dtype override must preserve or increase a positive lane count")
    if dtype.lanes % source_dtype.lanes != 0:
        raise ValueError("requested lanes must be an integer multiple of the source lanes")

    lane_factor = dtype.lanes // source_dtype.lanes
    if lane_factor > 1 and (source_dtype.bits * source_dtype.lanes) % 8 != 0:
        raise ValueError(
            "DLPack producer lane folds require a byte-aligned source element; "
            "sub-byte element padding cannot be reinterpreted safely"
        )
    folded_factor = 1
    fold_start = len(source_shape)
    while folded_factor < lane_factor and fold_start > 0:
        trailing_dim = source_shape[fold_start - 1]
        next_factor = folded_factor * trailing_dim
        if trailing_dim <= 0 or lane_factor % next_factor != 0:
            break
        folded_factor = next_factor
        fold_start -= 1
    if folded_factor != lane_factor:
        raise ValueError(
            f"requested lanes={dtype.lanes} cannot be formed by folding complete trailing "
            f"dimensions of source shape {source_shape} with lanes={source_dtype.lanes}"
        )

    folded_shape = source_shape[:fold_start]
    if lane_factor > 1 and fold_start == 0:
        # ovstage transports logical elements along a leading tensor dimension.
        # A fold that consumes every source axis therefore keeps a size-one
        # dimension instead of forwarding the mathematically rank-zero view.
        folded_shape = (1,)
    if shape is None:
        target_shape = folded_shape
    else:
        try:
            target_shape = tuple(operator.index(dim) for dim in shape)
        except TypeError as exc:
            raise TypeError("shape entries must be integers") from exc
        if any(dim < 0 for dim in target_shape):
            raise ValueError("shape entries must be non-negative")
        if target_shape != folded_shape:
            raise ValueError(
                f"DLPack producer shape override must equal the canonical folded shape {folded_shape} "
                "after folding complete trailing dimensions"
            )

    try:
        target_ndim = len(target_shape) if ndim is None else operator.index(ndim)
    except TypeError as exc:
        raise TypeError("ndim must be an integer") from exc
    if target_ndim != len(target_shape):
        raise ValueError(
            f"DLPack producer ndim ({target_ndim}) must equal the folded shape rank ({len(target_shape)})"
        )

    target_strides = None
    if strides is not None:
        try:
            target_strides = tuple(operator.index(stride) for stride in strides)
        except TypeError as exc:
            raise TypeError("stride entries must be integers") from exc
        expected_strides = _compact_row_major_strides(target_shape)
        if target_strides != expected_strides:
            raise ValueError(
                f"DLPack producer stride override must be compact row-major {expected_strides}, "
                f"got {target_strides}"
            )

    source_element_count = 1
    for dim in source_shape:
        source_element_count *= dim
    target_element_count = 1
    for dim in target_shape:
        target_element_count *= dim
    source_byte_extent = source_element_count * ((source_dtype.bits * source_dtype.lanes + 7) // 8)
    target_byte_extent = target_element_count * ((dtype.bits * dtype.lanes + 7) // 8)
    if source_byte_extent != target_byte_extent:
        raise ValueError("DLPack producer layout override must preserve the payload byte extent")

    tensor.ndim = target_ndim
    tensor.dtype = dtype
    if target_ndim > 0:
        tensor._shape_storage = (ctypes.c_int64 * target_ndim)(*target_shape)
        tensor.shape = ctypes.cast(tensor._shape_storage, ctypes.POINTER(ctypes.c_int64))
    else:
        tensor._shape_storage = None
        tensor.shape = None
    if target_strides is not None:
        tensor._strides_storage = (ctypes.c_int64 * target_ndim)(*target_strides)
        tensor.strides = ctypes.cast(tensor._strides_storage, ctypes.POINTER(ctypes.c_int64))
    else:
        tensor._strides_storage = None
        tensor.strides = None
    return tensor


def make_dltensor(
    array,
    *,
    dtype: Optional[DLDataType] = None,
    shape: Optional[list] = None,
    ndim: Optional[int] = None,
    strides: Optional[list] = None,
    device_type: int = DLDeviceType.kDLCPU,
    device_id: int = 0,
) -> DLTensor:
    """Build a :class:`DLTensor` viewing the memory of an array.

    A numpy array (CPU) is wrapped directly; the array, the shape storage, and the
    tensor are linked by reference so the C-visible pointers stay valid for as long
    as the returned tensor is alive. ``shape``/``ndim``/``dtype``/``strides`` may be
    overridden to describe vector (multi-lane) layouts a plain numpy dtype cannot
    express, matching an explicitly authored multi-lane ``DLTensor`` descriptor.
    A fixed-size ovstage write may also accept the numpy shape directly as a
    convenience layout (for example, ``(N, 4, 4)`` with ``lanes=1``); subsequent
    raw reads/maps normalize it to ``shape=(N,)``, ``lanes=16``.

    Any *non-numpy* object exposing the DLPack protocol (warp / torch / cupy / jax,
    CPU **or** CUDA) is ingested zero-copy via :meth:`DLTensor.from_dlpack` instead,
    so a GPU device buffer can be written without a host round-trip. Such a
    producer may be re-described with a vector ``dtype`` only through a validated
    fold of complete trailing dimensions into ``dtype.lanes``. The source must be
    compact row-major with byte-aligned elements, its base type and positive bit
    width are unchanged, and any explicit ``shape``/``ndim``/``strides`` must match the
    folded compact view. For example, a Warp ``vec3f`` export shaped ``(N, 3)``
    can be viewed as ``shape=(N,)`` with ``lanes=3`` without copying its CPU or
    CUDA allocation. A lane fold that consumes every source axis is normalized
    to ``shape=(1,)``, ``ndim=1``.

    The caller owns the data: it must keep the returned tensor (and thus the
    backing array) alive until the consuming op completes.
    """
    import numpy as np

    # DLPack producers (other than numpy, which keeps the override-capable fast path
    # below) are ingested through the protocol rather than force-copied via numpy.
    if not isinstance(array, np.ndarray) and hasattr(array, "__dlpack__"):
        tensor = DLTensor.from_dlpack(array)
        if any(x is not None for x in (dtype, shape, ndim, strides)):
            return _fold_dlpack_producer_layout(
                tensor,
                dtype=dtype,
                shape=shape,
                ndim=ndim,
                strides=strides,
            )
        return tensor

    array = np.ascontiguousarray(array)
    tensor = DLTensor()
    tensor._array = array  # keepalive: backing storage
    tensor.data = array.ctypes.data
    tensor.device = DLDevice(device_type, device_id)

    if shape is None:
        shape = list(array.shape)
    if ndim is None:
        ndim = len(shape)
    # ndim drives how many shape/strides slots a native consumer reads. Over-
    # allocating the buffers is fine (and used intentionally to describe folded
    # layouts); only reject ndim outrunning the slots, which is a genuine OOB.
    if ndim > len(shape):
        raise ValueError(f"ndim ({ndim}) exceeds len(shape) ({len(shape)})")
    if strides is not None and len(strides) < ndim:
        raise ValueError(f"len(strides) ({len(strides)}) is shorter than ndim ({ndim})")
    tensor.ndim = ndim
    tensor._shape_storage = (ctypes.c_int64 * len(shape))(*shape)  # keepalive
    tensor.shape = ctypes.cast(tensor._shape_storage, ctypes.POINTER(ctypes.c_int64))
    if strides is not None:
        tensor._strides_storage = (ctypes.c_int64 * len(strides))(*strides)  # keepalive
        tensor.strides = ctypes.cast(tensor._strides_storage, ctypes.POINTER(ctypes.c_int64))
    else:
        tensor.strides = None
    tensor.byte_offset = 0
    tensor.dtype = dtype if dtype is not None else numpy_to_dldatatype(array.dtype)
    return tensor


def dltensor_to_numpy(tensor: DLTensor):
    """Return a zero-copy numpy view of a CPU :class:`DLTensor`.

    The view is flat: its length is ``prod(shape) * dtype.lanes`` base elements,
    matching the raw-buffer indexing the C++ tests perform. Vector lanes are
    folded into the element count rather than the dtype. The data is owned by
    ovstage and only valid until the owning group/result is released.

    An invalid dtype (``lanes == 0``) is rejected with :class:`ValueError`. A
    tensor wrapping a numpy array (built by :func:`make_dltensor`) is also
    checked against the array's real size, so the returned view can never extend
    past that backing buffer; tensors from other producers carry no buffer size
    to check against and are trusted to describe themselves.
    """
    import numpy as np

    if tensor.device.device_type.value != DLDeviceType.kDLCPU:
        raise NotImplementedError(
            "dltensor_to_numpy only supports CPU tensors; GPU read is deferred to the CUDA phase"
        )

    key = (tensor.dtype.code, tensor.dtype.bits)
    ctype = _DL_TO_CTYPE.get(key)
    if ctype is None:
        raise ValueError(f"Unsupported DLDataType for numpy conversion: {tensor.dtype!r}")

    lanes = int(tensor.dtype.lanes)
    if lanes < 1:
        raise ValueError(f"Invalid DLDataType lanes for numpy conversion: {tensor.dtype!r}")

    count = 1
    for i in range(tensor.ndim):
        count *= tensor.shape[i]
    count *= lanes

    np_dtype = np.dtype(ctype)
    if count == 0 or not tensor.data:
        return np.empty(0, dtype=np_dtype)

    # When the backing buffer is known (a numpy array wrapped by make_dltensor),
    # refuse to build a view extending past it: a mis-described dtype/shape would
    # otherwise become an out-of-bounds read at first element access.
    backing = getattr(tensor, "_array", None)
    if backing is not None:
        described_bytes = int(tensor.byte_offset) + count * ctypes.sizeof(ctype)
        if described_bytes > backing.nbytes:
            raise ValueError(
                f"DLTensor describes {described_bytes} bytes (shape {tensor.shape_tuple}, "
                f"dtype {tensor.dtype!r}) but its backing buffer holds {backing.nbytes} bytes"
            )

    base_addr = int(tensor.data) + int(tensor.byte_offset)
    buffer = (ctype * count).from_address(base_addr)
    return np.ctypeslib.as_array(buffer)
