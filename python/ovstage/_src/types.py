# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Pythonic types layered over the raw ctypes bindings.

Enums mirror the ovstage C enums; :class:`OvstageError` turns error codes into
exceptions; :class:`Operation` models the async enqueue/observe pattern;
:class:`Filter`/:class:`Predicate`/:class:`OrdinalRange` build the C query
structs (with keepalives); and the group views expose read/map results with
zero-copy numpy access.
"""

import ctypes
from dataclasses import dataclass, field
from enum import IntEnum, IntFlag
from typing import Any, List, Optional, Sequence, Tuple, Union

from . import bindings as _b
from .dlpack import DLTensor, ManagedDLTensor, dltensor_to_numpy

__all__ = [
    "ErrorCode",
    "OvstageError",
    "FilterOp",
    "PrimMode",
    "Scope",
    "PopulationDomain",
    "AttributeSemantic",
    "HierarchyRelation",
    "HierarchyComputationModel",
    "OrdinalRange",
    "Predicate",
    "Filter",
    "Operation",
    "AttributeMeta",
    "ReadGroup",
    "MapGroup",
    "QueryResult",
    "HierarchyItem",
    "HierarchyResult",
    "HierarchyComputationModelDesc",
    "WriteDesc",
    "TIMEOUT_INFINITE",
]

TIMEOUT_INFINITE = _b.OVSTAGE_TIMEOUT_INFINITE


_ORDINAL_MAX = (1 << 64) - 1  # ovstage_ordinal_t is uint64_t


def check_ordinal(ordinal: int) -> int:
    """Validate and normalize an ordinal to a ``uint64_t``-representable int.

    C++ callers pass ``ovstage_ordinal_t`` (``uint64_t``), so an out-of-range value
    is a compile-time/type error. Python passes through ctypes as ``c_uint64`` and
    would silently wrap (both a negative value and one ``>= 2**64``); reject both
    here instead so the wrap can never reach the C API.
    """
    value = int(ordinal)
    if value < 0:
        raise ValueError(f"ordinal must be non-negative, got {ordinal}")
    if value > _ORDINAL_MAX:
        raise ValueError(f"ordinal must fit in uint64 (<= {_ORDINAL_MAX}), got {ordinal}")
    return value


class ErrorCode(IntEnum):
    OK = _b.OVSTAGE_OK
    INVALID_ARGUMENT = _b.OVSTAGE_ERROR_INVALID_ARGUMENT
    INVALID_HANDLE = _b.OVSTAGE_ERROR_INVALID_HANDLE
    NOT_FOUND = _b.OVSTAGE_ERROR_NOT_FOUND
    PRIM_NOT_FOUND = _b.OVSTAGE_ERROR_PRIM_NOT_FOUND
    WRITE_FLOOR_VIOLATION = _b.OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION
    NOT_SUPPORTED = _b.OVSTAGE_ERROR_NOT_SUPPORTED
    QUEUE_FULL = _b.OVSTAGE_ERROR_QUEUE_FULL
    END_OF_ITERATION = _b.OVSTAGE_ERROR_END_OF_ITERATION
    OUT_OF_MEMORY = _b.OVSTAGE_ERROR_OUT_OF_MEMORY
    LAYOUT_CHANGED = _b.OVSTAGE_ERROR_LAYOUT_CHANGED
    TIMEOUT = _b.OVSTAGE_ERROR_TIMEOUT
    OP_FAILED = _b.OVSTAGE_ERROR_OP_FAILED
    INTERNAL = _b.OVSTAGE_ERROR_INTERNAL


class OvstageError(RuntimeError):
    """Raised when an ovstage call or enqueued op fails.

    ``code`` is the raw ``ovstage_api_status_t``; ``message`` is the human-readable
    detail from ``ovstage_get_last_op_error`` / ``ovstage_get_last_error`` when
    available.
    """

    def __init__(self, code: int, message: str = ""):
        self.code = int(code)
        self.message = message or ""
        try:
            name = ErrorCode(self.code).name
        except ValueError:
            name = f"ERROR_{self.code}"
        super().__init__(f"{name}: {self.message}" if self.message else name)


class LogSeverity(IntEnum):
    """Log severity levels (mirrors ``ovstage_log_severity_t``).

    Values follow the underlying log-level ordering. ``NONE`` is a threshold
    sentinel: as a filter level it disables all logging and is never delivered
    to a callback.
    """

    VERBOSE = -2
    INFO = -1
    WARNING = 0
    ERROR = 1
    NONE = 3


class FilterOp(IntEnum):
    HAS = 0
    IN = 1
    CONTAINS = 2
    PREFIX = 3
    LT = 4
    LE = 5
    GT = 6
    GE = 7


class PrimMode(IntEnum):
    UPSERT = 0
    INSERT = 1


class Scope(IntEnum):
    """Write-floor advance scope (see :meth:`Stage.advance_write_floor`).

    - ``ALL`` → advance the global write floor and every known attribute.
    - ``INCLUDE`` → advance only the listed attributes.
    - ``EXCLUDE`` → advance every known attribute except the listed ones
      (an empty list behaves like ``ALL``).
    """

    ALL = 0
    INCLUDE = 1
    EXCLUDE = 2


class PopulationDomain(IntFlag):
    NONE = 0
    RENDERING = 1 << 0
    PHYSICS = 1 << 1
    ALL = (1 << 0) | (1 << 1)


class AttributeSemantic(IntEnum):
    """Authored USD interpretation of a column's bytes (``ovstage_attribute_semantic_t``).

    Geometric semantics (POINT/VECTOR/NORMAL/COLOR/QUATERNION/MATRIX/
    TEXTURE_COORDINATE) record a geometric role on the column; storage
    stays in the requested numeric ``dtype``.

    ID semantics select the corresponding ID storage type and require
    pre-interned id payloads (producers must intern via the path dictionary /
    token dictionary before writing -- ovstage does not stringify or resolve):

    - ``TOKEN_ID`` → ``dtype = (kDLUInt, 64, 1)`` carrying one 64-bit token id
      per row.
    - ``RELATIONSHIP_PATH_ID`` → ``dtype = (kDLUInt, 64, 1)`` carrying one
      64-bit path id per row.
    - ``CONNECTION_PATH_ID`` → ``dtype = (kDLUInt, 64, 2)`` carrying one
      ``(path_id, token_id)`` pair per row (one 16-byte element per row).

    Byte-string semantics (``ASSET_STRING``, ``PATH_EXPRESSION_STRING``) keep
    ragged ``(kDLUInt, 8, 1)`` byte-row storage with NUL-separated authored
    sub-values.

    ``STRING`` carries a plain USD ``string`` as raw UTF-8 bytes in a ragged
    ``(kDLUInt, 8, 1)`` byte array (``is_array = True``), not a token id. It
    records the USD-string role so the column uses the canonical USD-string
    representation.

    The semantic round-trips through the attribute column: writes record it at
    creation, reads recover it by decoding the column.
    """

    NONE = 0
    ASSET_STRING = 1
    TOKEN_ID = 2
    PATH_EXPRESSION_STRING = 3
    RELATIONSHIP_PATH_ID = 4
    POINT = 5
    VECTOR = 6
    NORMAL = 7
    COLOR = 8
    QUATERNION = 9
    MATRIX = 10
    TEXTURE_COORDINATE = 11
    CONNECTION_PATH_ID = 12
    STRING = 13


class HierarchyRelation(IntEnum):
    PARENT = _b.OVSTAGE_HIERARCHY_PARENT
    CHILDREN = _b.OVSTAGE_HIERARCHY_CHILDREN
    SIBLINGS = _b.OVSTAGE_HIERARCHY_SIBLINGS


class HierarchyComputationModel(IntEnum):
    INVALID = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_INVALID
    CPU_INCREMENTAL = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL
    GPU_INCREMENTAL = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_INCREMENTAL
    GPU_GLOBAL = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_GLOBAL
    DEFAULT_CPU = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_DEFAULT_CPU
    DEFAULT_GPU = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_DEFAULT_GPU


@dataclass
class HierarchyComputationModelDesc:
    """Runtime-supported hierarchy computation model descriptor."""

    model_id: int
    name: str
    description: str


@dataclass
class HierarchyItem:
    """Per-input hierarchy lookup result."""

    status: int
    paths: Tuple[str, ...]

    @property
    def ok(self) -> bool:
        return self.status == _b.OVSTAGE_OK


@dataclass
class HierarchyResult:
    """Copied hierarchy lookup result.

    ``items`` preserves the input path-list order. Each item carries its own
    status so a missing input prim can be reported without failing the whole
    batch.
    """

    ordinal: int
    items: List[HierarchyItem]

    @property
    def input_count(self) -> int:
        return len(self.items)

    def item_paths(self, index: int) -> Tuple[str, ...]:
        return self.items[index].paths


@dataclass
class WriteDesc:
    """Python-facing form of one native ``ovstage_attribute_write_t``.

    Each instance supplies one named attribute write to
    :meth:`Stage.write_attributes`.

    ``is_array`` is required and explicitly declares the logical attribute
    kind; it is never inferred from ``tensors``. ``tensors`` accepts the same
    numpy/DLTensor forms as :meth:`Stage.write_attribute`. Sparsity and CUDA
    synchronization are write-local, as is ``semantic``; the ordinal and
    prim mode are shared by the batch.
    """

    attribute: Union[int, str]
    tensors: Any
    is_array: bool
    index_map: Optional[Sequence[int]] = None
    mask: Optional[Sequence[int]] = None
    count: Optional[int] = None
    cuda_event: Optional[int] = None
    cuda_stream: Optional[int] = None
    semantic: AttributeSemantic = AttributeSemantic.NONE


@dataclass
class OrdinalRange:
    """Ordinal range for reads.

    - ``OrdinalRange.latest(N)`` → most recent value with ordinal <= N.
    - ``OrdinalRange.between(start, end)`` → all changes in [start, end].
    """

    end_ordinal: int
    start_ordinal: Optional[int] = None

    def _validate(self) -> None:
        # C read_attributes rejects reversed ranges with INVALID_ARGUMENT
        # (ReadInterface.inl); ordinals are uint64_t on the C side.
        check_ordinal(self.end_ordinal)
        if self.start_ordinal is not None:
            check_ordinal(self.start_ordinal)
            if self.start_ordinal > self.end_ordinal:
                raise ValueError(
                    f"start_ordinal ({self.start_ordinal}) must be <= end_ordinal ({self.end_ordinal})"
                )

    @classmethod
    def latest(cls, end_ordinal: int) -> "OrdinalRange":
        check_ordinal(end_ordinal)
        return cls(end_ordinal=end_ordinal)

    @classmethod
    def between(cls, start_ordinal: int, end_ordinal: int) -> "OrdinalRange":
        rng = cls(end_ordinal=end_ordinal, start_ordinal=start_ordinal)
        rng._validate()
        return rng

    def to_c(self) -> _b.ovstage_ordinal_range_t:
        self._validate()
        raw = _b.ovstage_ordinal_range_t()
        raw.end_ordinal = int(self.end_ordinal)
        if self.start_ordinal is not None:
            raw.start_ordinal = int(self.start_ordinal)
            raw.has_start_ordinal = True
        else:
            raw.has_start_ordinal = False
        return raw


@dataclass
class Predicate:
    """A single filter predicate. ``attribute`` may be an int token or a string."""

    attribute: Union[int, str]
    op: FilterOp
    values: Sequence[str] = field(default_factory=tuple)


class Filter:
    """Conjunction (AND) of predicates. ``None`` filter = match all prims."""

    def __init__(self, predicates: Sequence[Predicate]):
        self.predicates: List[Predicate] = list(predicates)

    def to_c(self) -> Tuple[_b.ovstage_filter_t, list]:
        """Return ``(ovstage_filter_t, keepalive)``.

        The keepalive list owns every ctypes buffer the filter struct points
        into (predicate array, attribute string refs, value-string arrays); the
        caller must keep it alive for the duration of the query enqueue call.
        """
        keepalive: list = []
        count = len(self.predicates)
        pred_array = (_b.ovstage_predicate_t * count)()
        keepalive.append(pred_array)
        for i, pred in enumerate(self.predicates):
            cpred = pred_array[i]
            sot = _b.make_string_or_token(pred.attribute)
            cpred.attribute = sot
            keepalive.append(sot)  # keeps sot._string_ref alive
            cpred.op = int(pred.op)
            values = list(pred.values)
            cpred.value_count = len(values)
            if values:
                val_array = (_b.ovx_string_t * len(values))()
                refs = []
                for j, v in enumerate(values):
                    s = _b.ovx_string_t(str(v))
                    val_array[j] = s
                    refs.append(s)  # keeps s._bytes alive
                cpred.values = ctypes.cast(val_array, ctypes.POINTER(_b.ovx_string_t))
                keepalive.append(val_array)
                keepalive.append(refs)
            else:
                cpred.values = None
        filt = _b.ovstage_filter_t()
        filt.predicates = ctypes.cast(pred_array, ctypes.POINTER(_b.ovstage_predicate_t))
        filt.count = count
        return filt, keepalive


class Operation:
    """A handle to an enqueued (asynchronous) ovstage operation.

    ``status`` is the enqueue status (``OVSTAGE_OK`` = accepted); ``op_id`` is
    the per-op identifier. Call :meth:`wait` to block until the op (and its
    ordinal-keyed dependencies) completes, raising :class:`OvstageError` if it
    failed.
    """

    def __init__(self, stage, status: int, op_id: int, keepalive=None):
        self._stage = stage
        self.status = int(status)
        self.op_id = int(op_id)
        self._keepalive = keepalive  # holds input buffers alive until waited
        self._consumed = False

    @property
    def ok(self) -> bool:
        return self.status == _b.OVSTAGE_OK

    def error_message(self) -> str:
        return self._stage._last_op_error(self.op_id)

    def wait(self, timeout: int = TIMEOUT_INFINITE) -> None:
        """Wait for completion and release the op. Raises on failure.

        Mirrors the C++ ``waitOk`` helper: if the enqueue was rejected, or the
        op or its dependencies failed, raises :class:`OvstageError`.
        """
        try:
            if self.status != _b.OVSTAGE_OK:
                raise OvstageError(self.status, self.error_message())
            if self._consumed:
                raise OvstageError(
                    _b.OVSTAGE_ERROR_INVALID_HANDLE,
                    f"operation {self.op_id} has already been waited and released",
                )
            if self.op_id == _b.OVSTAGE_INVALID_OP_ID:
                raise OvstageError(
                    _b.OVSTAGE_ERROR_INVALID_HANDLE,
                    "operation has no valid op id (OVSTAGE_INVALID_OP_ID); it was never enqueued",
                )
            if not getattr(self._stage, "_inst", None):
                raise OvstageError(
                    _b.OVSTAGE_ERROR_INVALID_HANDLE,
                    "operation cannot be waited after its Stage was destroyed",
                )
            self._consumed = True
            self._stage._wait_and_release(self.op_id, timeout)
        finally:
            self._keepalive = None


class AttributeMeta:
    """Read-only view of ``ovstage_attribute_meta_t``."""

    __slots__ = ("attribute_write_floor_ordinal", "layout_generation")

    def __init__(self, raw: _b.ovstage_attribute_meta_t):
        self.attribute_write_floor_ordinal = int(raw.attribute_write_floor_ordinal)
        self.layout_generation = int(raw.layout_generation)


class _GroupBase:
    """Shared accessors over the ``prims`` / ``data`` sub-structs of a group."""

    def __init__(self, raw):
        self.raw = raw

    # prims -----------------------------------------------------------------
    @property
    def prim_list(self) -> int:
        return int(self.raw.prims.list)

    @property
    def prim_offset(self) -> int:
        return int(self.raw.prims.offset)

    @property
    def prim_count(self) -> int:
        return int(self.raw.prims.count)

    @property
    def has_prim_index_map(self) -> bool:
        return bool(self.raw.prims.index_map)

    def prim_index(self, local: int) -> int:
        """Resolve the list-relative prim index of the ``local``-th prim."""
        p = self.raw.prims
        count = int(p.count)
        if not 0 <= local < count:
            raise IndexError(f"prim index {local} out of range [0, {count})")
        if p.index_map:
            return int(p.index_map[local])
        return int(p.offset) + int(local)

    # data ------------------------------------------------------------------
    @property
    def tensor_count(self) -> int:
        return int(self.raw.data.tensor_count)

    @property
    def data_count(self) -> int:
        return int(self.raw.data.count)

    @property
    def has_data_index_map(self) -> bool:
        return bool(self.raw.data.index_map)

    def data_row_index(self, local: int) -> int:
        """Resolve the data-tensor row index of the ``local``-th element."""
        d = self.raw.data
        count = int(d.count)
        if not 0 <= local < count:
            raise IndexError(f"data row index {local} out of range [0, {count})")
        if d.index_map:
            return int(d.index_map[local])
        return int(local)

    def tensor(self, index: int) -> DLTensor:
        """Raw :class:`DLTensor` at ``index`` (for shape/dtype/device checks)."""
        count = int(self.raw.data.tensor_count)
        if not 0 <= index < count:
            raise IndexError(f"tensor index {index} out of range [0, {count})")
        return self.raw.data.tensors[index]

    def array(self, index: int):
        """Zero-copy flat numpy view of tensor ``index`` (CPU only)."""
        return dltensor_to_numpy(self.tensor(index))

    def dlpack(self, index: int, *, readonly: bool = True) -> ManagedDLTensor:
        """DLPack-protocol view of tensor ``index`` for zero-copy exchange with
        numpy / warp / torch / cupy: ``np.from_dlpack(group.dlpack(i))``.

        Unlike :meth:`array` (CPU-only numpy), this also works for CUDA-resident
        tensors. The data is borrowed from ovstage and valid only until the owning
        group/result is released — copy it if it must outlive the read. The group
        is retained for the lifetime of the returned view.
        """
        return ManagedDLTensor(self.tensor(index), manager_ctx=self, deleter_callback=None, readonly=readonly)

    @property
    def meta(self) -> AttributeMeta:
        return AttributeMeta(self.raw.meta)


class ReadGroup(_GroupBase):
    """A read result group (``ovstage_read_group_t``).

    Valid until released via :meth:`Stage.release_group`. Exposes the attribute
    token, ordinal, delete flag, prim grouping, and tensor data.
    """

    @property
    def attribute(self) -> int:
        return int(self.raw.attribute)

    @property
    def ordinal(self) -> int:
        return int(self.raw.ordinal)

    @property
    def is_delete(self) -> bool:
        return bool(self.raw.is_delete)

    @property
    def is_array(self) -> bool:
        """Whether this result group carries array-valued attribute rows."""
        return bool(self.raw.is_array)


class MapGroup(_GroupBase):
    """A writable map group (``ovstage_map_group_t``).

    Fill ``array(i)`` then commit via :meth:`Stage.unmap_group` (or finalize the
    whole session with :meth:`Stage.unmap_attribute`).
    """

    def dlpack(self, index: int, *, readonly: bool = False) -> ManagedDLTensor:
        """Writable DLPack view of tensor ``index`` (a map group is writable).

        Fill it in place — ``wp.from_dlpack(group.dlpack(i))`` for a GPU kernel, or
        ``np.from_dlpack(group.dlpack(i))[:] = ...`` on CPU (numpy >= 2.1 honors the
        writable flag) — then commit via :meth:`Stage.unmap_group` /
        :meth:`Stage.unmap_attribute`.
        """
        return super().dlpack(index, readonly=readonly)


@dataclass
class QueryResult:
    """Snapshot of a fetched query result (copied out before release)."""

    attributes: List[int]
    total_prim_count: int
    all_handle: int
