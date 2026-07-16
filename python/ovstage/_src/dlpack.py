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
# Python C-API bindings used to build/consume DLPack PyCapsules. Mirrors the
# proven ovrtx implementation (ovrtx/_src/dlpack.py) so the two sibling packages
# stay behaviourally identical.
PyMem_Malloc = ctypes.pythonapi.PyMem_Malloc
PyMem_Malloc.argtypes = [ctypes.c_size_t]
PyMem_Malloc.restype = ctypes.c_void_p

PyMem_Free = ctypes.pythonapi.PyMem_Free
PyMem_Free.argtypes = [ctypes.c_void_p]
PyMem_Free.restype = None

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


class _CapsuleCtx:
    """Keepalive for a capsule's callbacks and the caller's ``manager_ctx``."""

    __slots__ = ("manager_ctx", "deleter_callback", "c_deleter", "capsule_destructor")

    def __init__(self, manager_ctx: Any, deleter_callback: Optional[Callable]) -> None:
        self.manager_ctx = manager_ctx
        self.deleter_callback = deleter_callback
        self.c_deleter = None
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

    ``manager_ctx`` is a Python object kept alive for the lifetime of the capsule
    (prevents GC of the underlying data); ``deleter_callback(manager_ctx)`` — if
    given — runs when the tensor is released. ``versioned`` selects the DLPack 1.0
    ``DLManagedTensorVersioned`` layout (with a read-only flag) over the legacy
    ``DLManagedTensor``. Vector dtypes (``lanes > 1``) are expanded to a trailing
    shape dimension with ``lanes = 1``, which is how array libraries consume them.
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
    mem_ptr = PyMem_Malloc(managed_size + shape_size)
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

    # Two manual refs keep _capsule_ctx (and its CFUNCTYPEs) alive: one for the
    # C deleter, one for the capsule destructor stored in the capsule context.
    _capsule_ctx = _CapsuleCtx(manager_ctx, deleter_callback)
    Py_IncRef(_capsule_ctx)  # held by c_deleter
    managed_tensor.manager_ctx = id(_capsule_ctx)

    @_DLPACK_DELETER
    def c_deleter(managed_ptr):
        mt = ManagedTensor.from_address(managed_ptr)
        ctx = ctypes.cast(mt.manager_ctx, ctypes.py_object).value
        try:
            if ctx.deleter_callback is not None:
                ctx.deleter_callback(ctx.manager_ctx)
        finally:
            Py_DecRef(ctx)
            PyMem_Free(managed_ptr)

    @PyCapsule_Destructor
    def capsule_destructor(capsule_ptr):
        ctx_id = _PyCapsule_GetContext_raw(capsule_ptr)
        if ctx_id:
            ctx = ctypes.cast(ctx_id, ctypes.py_object).value
            Py_DecRef(ctx)
        # Skip the deleter when the capsule was already consumed (renamed by the consumer).
        if not _PyCapsule_IsValid_raw(capsule_ptr, capsule_name):
            return
        managed_ptr = _PyCapsule_GetPointer_raw(capsule_ptr, capsule_name)
        if managed_ptr:
            mt = ManagedTensor.from_address(managed_ptr)
            if mt.deleter:
                mt.deleter(managed_ptr)

    _capsule_ctx.c_deleter = c_deleter
    _capsule_ctx.capsule_destructor = capsule_destructor
    managed_tensor.deleter = c_deleter

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
    """Build a :class:`DLDataType` from a numpy dtype (with optional vector lanes)."""
    name = str(np_dtype)
    if name not in _NUMPY_TO_DL:
        raise ValueError(f"Unsupported numpy dtype for DLPack: {name}")
    code, bits = _NUMPY_TO_DL[name]
    return DLDataType(code=code, bits=bits, lanes=lanes)


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
    express — mirroring how ``stage_api_test_utils.h`` authors its write tensors.

    Any *non-numpy* object exposing the DLPack protocol (warp / torch / cupy / jax,
    CPU **or** CUDA) is ingested zero-copy via :meth:`DLTensor.from_dlpack` instead,
    so a GPU device buffer can be written without a host round-trip. The
    numpy-specific overrides are not accepted on that path.

    The caller owns the data: it must keep the returned tensor (and thus the
    backing array) alive until the consuming op completes.
    """
    import numpy as np

    # DLPack producers (other than numpy, which keeps the override-capable fast path
    # below) are ingested through the protocol rather than force-copied via numpy.
    if not isinstance(array, np.ndarray) and hasattr(array, "__dlpack__"):
        if any(x is not None for x in (dtype, shape, ndim, strides)):
            raise ValueError(
                "shape/dtype/ndim/strides overrides are not supported for DLPack producers; "
                "pass a numpy array to use them"
            )
        return DLTensor.from_dlpack(array)

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

    count = 1
    for i in range(tensor.ndim):
        count *= tensor.shape[i]
    count *= max(1, tensor.dtype.lanes)

    np_dtype = np.dtype(ctype)
    if count == 0 or not tensor.data:
        return np.empty(0, dtype=np_dtype)

    base_addr = int(tensor.data) + int(tensor.byte_offset)
    buffer = (ctype * count).from_address(base_addr)
    return np.ctypeslib.as_array(buffer)
