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
import operator
import sys
import warnings
from dataclasses import dataclass, field
from enum import IntEnum, IntFlag
from typing import Any, Dict, FrozenSet, List, Optional, Sequence, Tuple, Type, Union

from . import bindings as _b
from .bindings import check_timeout  # defined there to avoid an import cycle with flush_log
from .dlpack import DLTensor, ManagedDLTensor, dltensor_to_numpy

__all__ = [
    "ErrorCode",
    "OvstageError",
    "FilterOp",
    "PrimMode",
    "Scope",
    "PopulationDomain",
    "PrimPredicateKind",
    "PropertyPredicateKind",
    "AttributeSemantic",
    "HierarchyRelation",
    "HierarchyComputationModel",
    "StageConfig",
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


_UINT32_MAX = (1 << 32) - 1
_UINT64_MAX = (1 << 64) - 1
_SIZE_T_MAX = (1 << (8 * ctypes.sizeof(ctypes.c_size_t))) - 1
_INT32_MIN, _INT32_MAX = -(1 << 31), (1 << 31) - 1  # the ovstage C enums are int

_ORDINAL_MAX = _UINT64_MAX  # ovstage_ordinal_t is uint64_t


def check_ordinal(ordinal: int) -> int:
    """Validate and normalize an ordinal to a ``uint64_t``-representable int.

    C++ callers pass ``ovstage_ordinal_t`` (``uint64_t``), so an out-of-range value
    is a compile-time/type error. Python passes through ctypes as ``c_uint64`` and
    would silently wrap (both a negative value and one ``>= 2**64``); reject both
    here instead so the wrap can never reach the C API.

    Delegates to :func:`_check_unsigned`, so a float is rejected rather than
    truncated. This used to be ``int(ordinal)``, which turned ``5.25`` into a real
    write at ordinal ``5``. Truncation toward zero also defeated the sign check
    below: ``-0.5`` normalized to ``0`` and never reached the negative branch.
    """
    return _check_unsigned(ordinal, _ORDINAL_MAX, "ordinal", "uint64")


def _check_unsigned(value: int, maximum: int, name: str, ctype: str) -> int:
    """Validate one caller int against a fixed-width unsigned C field.

    ctypes never raises on an out-of-range int: struct fields, array elements,
    and function arguments all wrap mod ``2**bits`` instead. A guard here is
    what turns a silently wrong value into a rejected one, the same way
    :func:`check_ordinal` and :func:`check_timeout` do for the wider fields.
    """
    try:
        result = operator.index(value)
    except TypeError:
        raise TypeError(f"{name} must be an int, got {type(value).__name__}") from None
    if result < 0:
        raise ValueError(f"{name} must be non-negative, got {value}")
    if result > maximum:
        raise ValueError(f"{name} must fit in {ctype} (<= {maximum}), got {value}")
    return result


def check_write_count(count: int) -> int:
    """Validate a write's logical element count (``ovstage_write_data_t.count``).

    This is the representability guard only: it rejects what the ``uint32_t``
    field cannot hold. Without it ``2**32`` silently became ``0`` (a whole-query
    write) and ``2**32 + n`` aliased ``n``. ``0`` passes here because it *is*
    representable — it is the C contract's "every prim the query covers", and
    :meth:`Stage.write_attribute` rejects it separately, where the sparsity form
    that gives it meaning is known.

    Its own messages rather than :func:`_check_unsigned`'s: ``count`` is a
    single named argument, so the wording names it directly instead of the
    "entries"/"words" phrasing the sequence guards use.
    """
    try:
        value = operator.index(count)
    except TypeError:
        raise TypeError(f"count must be an int, got {type(count).__name__}") from None
    if value < 0:
        raise ValueError(f"count must not be negative; got {count}")
    if value > _UINT32_MAX:
        raise ValueError(f"count must fit in uint32 (<= {_UINT32_MAX}), got {count}")
    return value


def _check_unsigned_sequence(values: Sequence[int], maximum: int, name: str, ctype: str) -> List[int]:
    """Materialize and bound-check a sequence destined for a fixed-width C array.

    ``min`` / ``max`` are C-level passes over the already-materialized list
    rather than a per-element branch: sparsity inputs are per-prim, and this
    path is already O(n) from the materialization itself.
    """
    try:
        result = [operator.index(value) for value in values]
    except TypeError as exc:
        raise TypeError(f"{name} must be ints ({exc})") from None
    if result:
        _check_unsigned(min(result), maximum, name, ctype)
        _check_unsigned(max(result), maximum, name, ctype)
    return result


def check_index_map(index_map: Sequence[int]) -> List[int]:
    """Validate write ``index_map`` entries (``const uint32_t*``) and materialize them.

    Entries are source-row indices. The native side rejects a row beyond the
    transported row count, but only after ctypes has already wrapped, so an
    entry of ``2**32`` silently aliased source row ``0`` and wrote the wrong
    data with no error.
    """
    return _check_unsigned_sequence(index_map, _UINT32_MAX, "index_map entries", "uint32")


def check_mask(mask: Sequence[int]) -> List[int]:
    """Validate write ``mask`` words (``ovstage_mask_t``, ``uint64_t``) and materialize them.

    A negative word silently became an all-bits-set word (write every element)
    and ``2**64 + n`` aliased ``n``.
    """
    return _check_unsigned_sequence(mask, _UINT64_MAX, "mask words", "uint64")


def check_token(token: int, name: str = "attribute") -> int:
    """Validate an attribute token (``ovx_token_t``, ``uint64_t``).

    Tokens are interned ids, so a wrong one names a different live attribute
    rather than failing. ``int()`` here accepted a float and a ``str``; the token
    also wrapped, so ``2**64`` addressed token ``0``.
    """
    return _check_unsigned(token, _UINT64_MAX, name, "uint64")


def check_token_sequence(tokens: Sequence[int], name: str) -> List[int]:
    """Validate a sequence of attribute tokens and materialize it for a C array."""
    return _check_unsigned_sequence(tokens, _UINT64_MAX, name, "uint64")


def check_handle(handle: int, name: str) -> int:
    """Validate an opaque handle or op id (``uint64_t``).

    Handle ids come from one monotonic counter, so a truncated or wrapped handle
    lands on a live neighbour instead of failing as invalid.
    """
    return _check_unsigned(handle, _UINT64_MAX, name, "uint64")


def check_uint32(value: int, name: str) -> int:
    """Validate an integer for a generic ``uint32_t`` field."""
    return _check_unsigned(value, _UINT32_MAX, name, "uint32")


def check_uint64(value: int, name: str) -> int:
    """Validate an integer for a generic ``uint64_t`` field."""
    return _check_unsigned(value, _UINT64_MAX, name, "uint64")


def check_int64(value: int, name: str) -> int:
    """Validate an integer for a generic ``int64_t`` field."""
    try:
        result = operator.index(value)
    except TypeError:
        raise TypeError(f"{name} must be an int, got {type(value).__name__}") from None
    minimum, maximum = -(1 << 63), (1 << 63) - 1
    if not (minimum <= result <= maximum):
        raise ValueError(f"{name} must fit in int64, got {value}")
    return result


def check_element_sizes(element_sizes: Sequence[int]) -> List[int]:
    """Validate ``map_attribute`` per-element byte sizes (``size_t*``)."""
    return _check_unsigned_sequence(element_sizes, _SIZE_T_MAX, "element_sizes entries", "size_t")


def check_domains(domains: int) -> int:
    """Validate a population domain bitmask (``uint32_t domains``).

    The C parameter is ``uint32_t`` -- deliberately not
    ``ovstage_population_domain_t`` -- and the header calls it a bitmask to OR
    together, so the extra width is the contract's headroom for future domain
    bits. The runtime bit-tests the bits it knows and ignores the rest, so
    there is no ``& ~PopulationDomain.ALL`` mask here: masking would re-narrow
    exactly what ``uint32_t`` widened, and a wheel older than the loaded
    library would reject a domain that library implements.

    :func:`check_enum` was the wrong guard. It bounds to *signed* int32, so it
    rejected ``1 << 31`` -- representable in the field, and a legitimate future
    flag -- while accepting ``-1``, which ctypes then wrapped to ``0xFFFFFFFF``:
    every domain plus the 30 undefined bits. That is sticky rather than
    transient, because the domain set chosen at open governs every later
    ``apply_usd_*`` on the population state.

    Note for Python 3.10: ``~PopulationDomain.PHYSICS`` is ``-3`` there (3.11+
    gives ``1``), so it is now rejected rather than wrapped. Spell an exclusion
    as ``PopulationDomain.ALL & ~PopulationDomain.PHYSICS``, which is ``1`` on
    every supported interpreter.
    """
    return _check_unsigned(domains, _UINT32_MAX, "domains", "uint32")


def check_enum(value: int, name: str) -> int:
    """Index-guard a C enum value whose accepted set is *not* closed here.

    ovstage enums are ``c_int`` and :class:`LogSeverity` runs to ``-2``, so this
    is a *signed* guard rather than :func:`_check_unsigned`. Representability
    only. What it stops is ``int()`` truncating a float into a neighbouring
    enumerator -- ``1.9`` became ``PrimMode.INSERT``, and because truncation is
    toward zero ``-1.9`` became ``LogSeverity.INFO`` where the caller meant
    ``VERBOSE``.

    A range guard is not value validation, so this is the *weaker* of the two
    enum guards, kept only where the contract forbids the stronger one:
    ``model`` (ids are advertised at runtime by
    ``ovstage_get_hierarchy_computation_models``) and ``semantic`` (documented
    to accept a raw ``ovstage_attribute_semantic_t``). Every closed C enum uses
    :func:`check_enum_member` instead.
    """
    try:
        result = operator.index(value)
    except TypeError:
        raise TypeError(f"{name} must be an int, got {type(value).__name__}") from None
    if not (_INT32_MIN <= result <= _INT32_MAX):
        raise ValueError(f"{name} must fit in int32, got {value}")
    return result


_ENUM_MEMBER_CACHE: Dict[type, Tuple[FrozenSet[int], str]] = {}


def _enum_members(enum_type: Type[IntEnum]) -> Tuple[FrozenSet[int], str]:
    """Return ``(accepted values, listing for the message)`` for a closed C enum.

    Read off ``__members__`` rather than by iterating the class: iteration is
    not stable across the interpreters this wheel supports, while
    ``__members__`` is the same mapping on all of them. Cached because
    :meth:`Filter.to_c` runs the predicate guard once per predicate.

    :class:`IntFlag` is refused outright. A flag parameter's legal inputs are
    arbitrary ORs, so it has no member set to validate against --
    ``PopulationDomain(1 << 31)`` does not raise -- which is why a flag field
    gets a width guard instead; see :func:`check_domains`.
    """
    cached = _ENUM_MEMBER_CACHE.get(enum_type)
    if cached is None:
        if issubclass(enum_type, IntFlag):
            raise TypeError(
                f"{enum_type.__name__} is an IntFlag; a flag field takes a width guard "
                "(see check_domains), not a membership check"
            )
        values, names, seen = [], [], set()
        for member_name, member in enum_type.__members__.items():
            value = int(member)
            values.append(value)
            if value not in seen:  # aliases stay accepted, but only list them once
                seen.add(value)
                names.append(f"{member_name}={value}")
        cached = (frozenset(values), ", ".join(names))
        _ENUM_MEMBER_CACHE[enum_type] = cached
    return cached


def check_enum_member(value: int, enum_type: Type[IntEnum], name: str) -> int:
    """Validate a caller value against a *closed* C enum's declared members.

    :func:`check_enum` bounds representability only, so an in-range garbage
    value passes as silently as a wrapped one. :class:`LogSeverity` is the case
    that shows why: it has a real gap at ``2``, and the runtime's severity
    switch has no ``default``, so ``severity=2`` installed an *INFO* threshold
    -- a log flood -- for a caller who meant something stricter.

    Use this only where the C enum is closed. Where the header advertises the
    set at runtime, or documents that raw values are accepted, keep
    :func:`check_enum`: otherwise a wheel older than the loaded library rejects
    a value that library accepts, and nothing pins the two versions together.

    No width check, because membership implies representability -- bounding
    first would report ``must fit in int32`` for a value whose real problem is
    that it is not a member.

    Membership is by value, not by type: ``Scope.INCLUDE`` and
    ``PrimMode.INSERT`` are both ``1``, so passing the wrong enum class still
    passes. An ``isinstance`` gate would catch that only by also rejecting the
    plain ``0``/``1`` spelling the write and map docstrings accept.
    """
    try:
        result = operator.index(value)
    except TypeError:
        raise TypeError(f"{name} must be an int, got {type(value).__name__}") from None
    values, listing = _enum_members(enum_type)
    if result not in values:
        raise ValueError(f"{name} must be a valid {enum_type.__name__} ({listing}), got {value}")
    return result


def check_int(value: int, name: str) -> int:
    """Reject a non-integer for a field that never reaches a fixed-width C type.

    Deliberately unbounded, unlike every other guard here. The width checks
    exist because ctypes wraps instead of raising, and that hazard needs a C
    field behind the value to be real. :attr:`Operation.status` has none: it is
    only compared against ``OVSTAGE_OK`` and handed to :class:`OvstageError`,
    which formats a code it does not recognize as ``ERROR_<n>`` so newer
    statuses stay readable. Bounding it to some width would assert a boundary
    that is not there and send the next reader looking for it.

    What this *does* stop is the truncation. ``int(0.5)`` is ``0``, which is
    ``OVSTAGE_OK``, so a fractional status read as success.
    """
    try:
        return operator.index(value)
    except TypeError:
        raise TypeError(f"{name} must be an int, got {type(value).__name__}") from None


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
    OUT_OF_RANGE = _b.OVSTAGE_ERROR_OUT_OF_RANGE
    INTERNAL = _b.OVSTAGE_ERROR_INTERNAL


class OvstageError(RuntimeError):
    """Raised when an ovstage call or enqueued op fails.

    ``code`` is the raw ``ovstage_api_status_t``; ``message`` is the human-readable
    detail from ``ovstage_get_last_op_error`` / ``ovstage_get_last_error`` when
    available.
    """

    def __init__(self, code: int, message: str = ""):
        # Guarded for the reason Operation.status is: a fractional code
        # truncated, and int(0.5) is OVSTAGE_OK, so an error object built
        # around a real failure formatted itself as "OK".
        self.code = check_int(code, "code")
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


class PrimPredicateKind(IntEnum):
    """How an ``ovstage_population_prim_predicate_t`` matches.

    A kind that takes values matches when any one of them matches: the values of
    a single predicate are a disjunction, never a conjunction. Requiring more
    than one condition is what ``AND`` is for.
    """

    NONE = 0
    ALL = 1
    AND = 2
    OR = 3
    NOT = 4
    HAS_PARENT = 5
    HAS_ANCESTOR = 6
    HAS_PROPERTY = 7
    HAS_TYPE = 8
    IS_A_TYPE = 9
    HAS_SCHEMA = 10
    HAS_APPLIED_SCHEMA = 11
    HAS_APPLIED_SCHEMA_IN_NAMESPACE = 12
    HAS_PATH = 13
    IS_UNDER_PATH = 14
    HAS_KIND = 15
    HAS_PURPOSE = 16
    HAS_METADATA = 17


class PropertyPredicateKind(IntEnum):
    """How an ``ovstage_population_property_predicate_t`` matches.

    Values disjoin exactly as they do for :class:`PrimPredicateKind`.
    """

    NONE = 0
    ALL = 1
    AND = 2
    OR = 3
    NOT = 4
    DECLARED_BY_SCHEMA = 5
    HAS_NAME = 6
    IN_NAMESPACE = 7
    HAS_METADATA = 8
    IS_ATTRIBUTE = 9
    IS_RELATIONSHIP = 10
    IS_CUSTOM = 11
    IS_AUTHORED = 12


class AttributeSemantic(IntEnum):
    """Authored USD interpretation of a column's bytes (``ovstage_attribute_semantic_t``).

    Geometric semantics (POINT/VECTOR/NORMAL/COLOR/QUATERNION/MATRIX/FRAME/
    TEXTURE_COORDINATE) record a geometric role on the column; storage
    stays in the requested numeric ``dtype``.

    ``TIME_CODE`` marks a time code -- a unitless time value -- again with
    storage in the requested numeric ``dtype``. Only the plain numeric value is
    carried: sentinel time codes have no portable numeric encoding, so they are
    not representable.

    ID semantics select the corresponding ID storage type and require
    pre-interned id payloads (producers must intern via the path dictionary /
    token dictionary before writing -- ovstage does not stringify or resolve):

    - ``TOKEN_ID`` → ``dtype = (kDLUInt, 64, 1)`` carrying one 64-bit token id
      per row; id 0 is the empty token.
    - ``RELATIONSHIP_PATH_ID`` → ``dtype = (kDLUInt, 64, 1)`` carrying one
      64-bit path id per row.
    - ``CONNECTION_PATH_ID`` → ``dtype = (kDLUInt, 64, 2)`` carrying one
      ``(path_id, token_id)`` pair per row (one 16-byte element per row).
    - ``ASSET_PATH_ID`` → ``dtype = (kDLUInt, 64, 2)`` carrying one
      ``(authored_token, resolved_token)`` pair per row (one 16-byte element
      per row).

    An asset has two paths: the path as authored, and the path after
    resolution. ``ASSET_PATH_ID`` carries both as token ids. A token id of 0
    means no path, so an unresolved asset has a resolved token id of 0.

    ``PATH_EXPRESSION_STRING`` carries the expression text as one interned token
    id, ``(kDLUInt, 64, 1)``. ``is_array = False`` is a scalar
    ``pathExpression``; ``True`` is ``pathExpression[]``, one id per element.
    Writes reject other layouts. A token id of 0 means no expression. ovstage
    does not evaluate the expression; it stores the id of the authored text.

    ``STRING`` carries a plain USD ``string`` as raw UTF-8 bytes in a ragged
    ``(kDLUInt, 8, 1)`` byte array (``is_array = True``), not a token id. It
    records the USD-string role so the column uses the canonical USD-string
    representation.

    The semantic round-trips through the attribute column: writes record it at
    creation, reads recover it by decoding the column.
    """

    NONE = 0
    ASSET_PATH_ID = 1
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
    TIME_CODE = 14
    FRAME = 15


class HierarchyRelation(IntEnum):
    PARENT = _b.OVSTAGE_HIERARCHY_PARENT
    CHILDREN = _b.OVSTAGE_HIERARCHY_CHILDREN
    SIBLINGS = _b.OVSTAGE_HIERARCHY_SIBLINGS


class HierarchyComputationModel(IntEnum):
    INVALID = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_INVALID
    CPU_INCREMENTAL = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL
    GPU_INCREMENTAL = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_INCREMENTAL
    GPU_GLOBAL = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_GLOBAL
    RUNTIME_DEFAULT = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_RUNTIME_DEFAULT
    DEFAULT_CPU = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_DEFAULT_CPU
    DEFAULT_GPU = _b.OVSTAGE_HIERARCHY_COMPUTATION_MODEL_DEFAULT_GPU


@dataclass
class StageConfig:
    """Process configuration applied when creating a :class:`~ovstage.Stage`.

    The configuration is process-scoped. Configured stages may coexist when
    their concrete settings match; creating a stage with a conflicting setting
    while another stage is live raises :class:`OvstageError`.
    """

    runtime_default_hierarchy_computation_model: Optional[HierarchyComputationModel] = None
    """Model used for automatic transform updates and manual
    :attr:`HierarchyComputationModel.RUNTIME_DEFAULT` computations.

    ``None`` and :attr:`HierarchyComputationModel.RUNTIME_DEFAULT` do not
    override the active process default. A fresh process defaults to
    :attr:`HierarchyComputationModel.CPU_INCREMENTAL`; ``RUNTIME_DEFAULT`` is
    sent as an entry that the native runtime intentionally ignores.
    """


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
    prim mode are shared by the batch. Fixed-size convenience shapes are
    normalized to one source-data-row dimension with the tuple width in
    ``dtype.lanes``; their trailing dimensions are not preserved.

    ``count``, ``index_map``, and ``mask`` carry the same bounds as
    :meth:`Stage.write_attribute`.
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

    - ``OrdinalRange.latest(N)`` → latest snapshot request; recorded columns
      return current committed payload rather than historical payload <= N.
    - ``OrdinalRange.between(start, end)`` → select the keys that changed
      in inclusive [start, end]. An unsealed selected change raises
      ``WRITE_FLOOR_VIOLATION``. If a selected key also changed after ``end``,
      latest-only storage raises ``OUT_OF_RANGE`` because the payload for that
      fixed range is no longer available.
    """

    end_ordinal: int
    start_ordinal: Optional[int] = None

    def _validate(self) -> Tuple[int, Optional[int]]:
        """Validate both ends and return them normalized.

        The normalized values are *returned* rather than discarded: this class is
        a plain (unfrozen) dataclass, so ``latest()`` / ``between()`` cannot be
        the only checkpoint -- a caller can build one directly or assign to the
        field afterwards. Validating here and having :meth:`to_c` consume the
        result is what makes every entry point converge on one check.
        """
        # C read_attributes rejects reversed ranges with INVALID_ARGUMENT
        # (ReadInterface.cpp); ordinals are uint64_t on the C side.
        end = check_ordinal(self.end_ordinal)
        start = None
        if self.start_ordinal is not None:
            start = check_ordinal(self.start_ordinal)
            if start > end:
                raise ValueError(
                    f"start_ordinal ({self.start_ordinal}) must be <= end_ordinal ({self.end_ordinal})"
                )
        return end, start

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
        end, start = self._validate()
        raw = _b.ovstage_ordinal_range_t()
        raw.end_ordinal = end
        if start is not None:
            raw.start_ordinal = start
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
            # A truncated op picks a neighbouring FilterOp, so the query resolves
            # a different prim set and every write/read/map on the handle it
            # returns targets the wrong prims.
            # FilterOp is closed; the planner already rejects an unknown op with
            # NOT_SUPPORTED, so this moves the error earlier and makes it a ValueError.
            cpred.op = check_enum_member(pred.op, FilterOp, "predicate op")
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
        # Normally both come straight from an enqueue result, but Operation is
        # public and directly constructible, and a truncated op_id waits on (and
        # releases) whichever live op that integer names.
        self._stage = stage
        self.status = check_int(status, "status")
        self.op_id = check_handle(op_id, "op_id")
        self._keepalive = keepalive  # holds input buffers alive until waited
        self._consumed = False
        # A rejected enqueue records its detail in the thread-local last-error
        # slot, which the next enqueue on this thread clears. Reading it lazily
        # at wait() time can therefore return a later operation's message, or
        # nothing at all. The enqueue has only just returned, so capture it now.
        self._enqueue_error = (
            self._stage._last_op_error(self.op_id) if self.status != _b.OVSTAGE_OK else None
        )

    @property
    def ok(self) -> bool:
        return self.status == _b.OVSTAGE_OK

    def error_message(self) -> str:
        if self._enqueue_error is not None:
            return self._enqueue_error
        return self._stage._last_op_error(self.op_id)

    def wait(self, timeout: int = TIMEOUT_INFINITE) -> None:
        """Wait for completion and release the op. Raises on failure.

        Mirrors the C++ ``waitOk`` helper: if the enqueue was rejected, or the
        op or its dependencies failed, raises :class:`OvstageError`.

        :param timeout: max nanoseconds to wait; ``TIMEOUT_INFINITE`` (default)
            blocks, ``0`` polls.
        :raises TypeError: if ``timeout`` is not an integer (e.g. ``None``).
        :raises ValueError: if ``timeout`` is negative or does not fit in uint64.
        """
        # Validate before the try: a rejected timeout must not consume the op
        # or drop the keepalives of inputs the pending op may still read.
        timeout = check_timeout(timeout)
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


def _warn_dropped_resource(message: str) -> None:
    """Report a resource dropped without release, without escaping ``__del__``.

    The caller must have released the resource *before* calling this:
    :func:`warnings.warn` raises when the caller escalates ``ResourceWarning``
    (``-W error::ResourceWarning``), so warning first would skip the release for
    exactly the users who asked to be strict about resources.

    When it does raise, fall back to stderr rather than swallowing the report —
    opting into strictness must not yield *less* diagnostic output than leaving
    the default filters in place, which ignore ``ResourceWarning`` outright.
    """
    try:
        warnings.warn(message, ResourceWarning, stacklevel=3)
    except Exception:
        try:
            print(f"ResourceWarning: {message}", file=sys.stderr)
        except Exception:
            pass  # stderr gone (interpreter shutdown); nothing left to report through


class _GroupBase:
    """Shared accessors over the ``prims`` / ``data`` sub-structs of a group."""

    def __init__(self, raw):
        self.raw = raw

    def _check_live(self) -> None:
        """Hook for subclasses whose storage can be released out from under them."""

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
        """Resolve the list-relative prim index of the ``local``-th prim.

        ``local`` is index-guarded before the range check, because the range
        check does not reject a fraction: ``0 <= 1.5 < count`` is true. Without
        this, ``int(local)`` truncated, and the shipped row-placement idiom
        ``buffer[g.data_row_index(i)] = value[g.prim_index(i)]`` then wrote the
        wrong row -- on a map that is committed stage state, not just a
        misread.
        """
        self._check_live()
        local = check_int(local, "local")
        p = self.raw.prims
        count = int(p.count)
        if not 0 <= local < count:
            raise IndexError(f"prim index {local} out of range [0, {count})")
        if p.index_map:
            return int(p.index_map[local])
        return int(p.offset) + local

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
        """Resolve the data-tensor row index of the ``local``-th element.

        Index-guarded before the range check, for the reason in
        :meth:`prim_index`.
        """
        self._check_live()
        local = check_int(local, "local")
        d = self.raw.data
        count = int(d.count)
        if not 0 <= local < count:
            raise IndexError(f"data row index {local} out of range [0, {count})")
        if d.index_map:
            return int(d.index_map[local])
        return local

    def tensor(self, index: int) -> DLTensor:
        """Raw :class:`DLTensor` at ``index`` (for shape/dtype/device checks).

        A fixed-size read/map tensor is lane-canonical: ``ndim=1``, the leading
        dimension is the transported data-row count, and ``dtype.lanes`` is the
        complete tuple width. Use :meth:`data_row_index` to resolve a logical
        group element through any data index map. Convenience write dimensions
        are not reconstructed.
        """
        self._check_live()
        index = check_int(index, "index")
        count = int(self.raw.data.tensor_count)
        if not 0 <= index < count:
            raise IndexError(f"tensor index {index} out of range [0, {count})")
        return self.raw.data.tensors[index]

    def array(self, index: int):
        """Zero-copy flat numpy view of tensor ``index`` (CPU only).

        Tuple lanes are folded into this one-dimensional base-element view.
        """
        return dltensor_to_numpy(self.tensor(index))

    def dlpack(self, index: int, *, readonly: bool = True) -> ManagedDLTensor:
        """DLPack-protocol view of tensor ``index`` for zero-copy exchange with
        numpy / warp / torch / cupy: ``np.from_dlpack(group.dlpack(i))``.

        Unlike :meth:`array` (CPU-only numpy), this also works for CUDA-resident
        tensors. The data is borrowed from ovstage and valid only until the owning
        group/result is released — copy it if it must outlive the read. The returned
        :class:`~ovstage.ManagedDLTensor` retains the group, but a consumer view does
        not. A direct ``np.from_dlpack(group.dlpack(i))`` remains valid while the
        owning read/map operation is alive; custom producers whose manager context
        owns the backing allocation have stricter lifetime requirements (see
        :class:`~ovstage.ManagedDLTensor`). A multi-lane raw dtype
        exports with exactly one trailing lane axis, so a fixed matrix is
        ``(N, 16)``, not ``(N, 4, 4)``.
        """
        return ManagedDLTensor(self.tensor(index), manager_ctx=self, deleter_callback=None, readonly=readonly)

    @property
    def meta(self) -> AttributeMeta:
        return AttributeMeta(self.raw.meta)


class ReadGroup(_GroupBase):
    """A read result group (``ovstage_read_group_t``).

    Valid until released via :meth:`Stage.release_group`. Exposes the attribute
    token, ordinal, delete flag, prim grouping, and tensor data.

    A group's pinned storage is an independent resource: it is reclaimed **only**
    by ``release_group``. Releasing the owning :class:`Read` does not reclaim it,
    so a dropped group stays pinned for the life of the :class:`Stage` — and,
    because the pin also holds the group's outstanding-read coverage, later writes
    to the same attribute and prims keep failing with an "overlapping outstanding
    read" error reported at the *write* site, far from the group that caused it.

    Use it as a context manager so the group is released even when an error
    interrupts processing. ``fetch_next()`` returns ``None`` at end of iteration,
    so bind it before entering the block::

        group = read.fetch_next()
        if group is not None:
            with group:
                values = np.array(group.array(0))  # copy out to outlive the group

    or iterate, which only yields real groups::

        for group in read.groups():
            with group:
                ...

    If a group is dropped without release, :meth:`__del__` issues a best-effort
    release and emits a :class:`ResourceWarning`; rely on the context manager (or
    an explicit :meth:`Stage.release_group`) rather than the finalizer. Note the
    zero-copy views handed out by :meth:`array` / :meth:`tensor` point into that
    pinned storage and must not outlive the release.
    """

    def __init__(self, raw, stage=None):
        super().__init__(raw)
        # None only if a caller constructs a group itself; without an owning
        # stage there is no slot to release through, so the finalizer stays off.
        self._stage = stage
        self._released = False

    @property
    def released(self) -> bool:
        """Whether this group's pinned storage has been released."""
        return self._released

    def _check_live(self) -> None:
        """Reject access to storage this group has already released.

        The pointers in ``raw`` dangle after ``release_group``, so reading through
        them would return whatever now occupies that memory rather than failing.
        """
        if self._released:
            raise OvstageError(
                _b.OVSTAGE_ERROR_INVALID_HANDLE,
                "ReadGroup has been released; its pinned storage is no longer valid",
            )

    def _claim_release(self) -> None:
        if self._released:
            raise OvstageError(
                _b.OVSTAGE_ERROR_INVALID_HANDLE,
                "ReadGroup has already been released; its pinned storage is no longer valid",
            )
        self._released = True

    def _rollback_release(self) -> None:
        self._released = False

    def release(self) -> None:
        """Release this group's pinned storage (see :meth:`Stage.release_group`)."""
        if self._stage is None:
            raise OvstageError(
                _b.OVSTAGE_ERROR_INVALID_HANDLE,
                "ReadGroup was constructed without an owning Stage; release it via "
                "Stage.release_group instead",
            )
        self._stage.release_group(self)

    def __enter__(self) -> "ReadGroup":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self._released or self._stage is None:
            return
        if exc_type is None:
            self.release()  # surface release errors on the normal exit path
        else:
            # An exception is already propagating; release best-effort so it does
            # not mask the original error.
            try:
                self.release()
            except Exception:
                pass

    def __del__(self):
        # Safety net only. Skip if already released, if there is no owning stage,
        # or if that stage is gone (e.g. interpreter shutdown), where the pinned
        # storage died with the instance and dispatch is unsafe.
        if self._released or self._stage is None or not getattr(self._stage, "_inst", None):
            return
        # Release BEFORE warning. warnings.warn raises when the caller escalates
        # ResourceWarning (-W error::ResourceWarning, some -X dev setups), so
        # warning first would skip the release for exactly the users who asked to
        # be strict about resources — and here that leaves writes to these prims
        # failing for the life of the Stage.
        try:
            self.release()
        except Exception:
            pass
        _warn_dropped_resource(
            "ReadGroup was garbage-collected without release(); issued a best-effort "
            "release to free its pinned storage. Use a 'with' block or call "
            "Stage.release_group() explicitly — an unreleased group keeps failing later "
            "writes to the same prims with an 'overlapping outstanding read' error."
        )

    def array(self, index: int):
        """Zero-copy flat read-only numpy view of tensor ``index`` (CPU only).

        Tuple lanes are folded into this one-dimensional base-element view.

        The view **borrows** this group's storage and does not keep the group
        alive: it is valid only while the group is, and the group is released by
        :meth:`Stage.release_group`, by ``with`` exit, or by the finalizer once
        the group becomes unreachable. Keep the group bound for as long as you
        read through the view, and copy out (``np.array(...)``) anything that must
        outlive it.
        """
        return dltensor_to_numpy(self.tensor(index), readonly=True)

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
    """Snapshot of a fetched query result (copied out before release).

    Owns no C-side resources: :meth:`Stage.fetch_query_result` copies the scalar
    summary and the attribute tokens out, then releases the payload before
    returning. Nothing here needs to be freed.
    """

    attributes: List[int]
    total_prim_count: int
    all_handle: int
    """The query's own handle, echoed back for convenience — *not* a second resource.

    It is the same value as :attr:`Query.handle`, included so a consumer handed only
    the ``QueryResult`` still has the handle covering all matched prims (e.g. to pass
    to :meth:`Stage.resolve_query_prim_paths`). Releasing the query
    (:meth:`Query.release`, or :meth:`Stage.release_query`) reclaims it; releasing
    ``all_handle`` *as well* would be a double release.
    """
