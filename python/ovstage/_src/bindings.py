# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Raw ctypes layer for the ovstage C ABI.

The ovstage shared loader exports the process functions and
``ovstage_create_instance`` as flat symbols. ``ovstage_create_instance`` hands
back an ``ovstage_instance_t`` struct: ``{ovstage_data_plane_instance_t
data_plane; ovstage_ext_instances_t ext_instances;}``. Every instance of the
API is a vtable with its context. The data-plane instance is ``{const
ovstage_data_plane_vtable_t* vtable; ovstage_data_plane_context_t* context;}``. The public vtable
(``ovstage_api/ovstage_api.h``) holds the data-plane operations
(query/read/write/map/ordinal/diagnostics) and the subtree clone slots
``clone`` and ``fetch_clone_result``. A data-plane call reads
``instance->data_plane.vtable->slot`` and passes
``instance->data_plane.context`` as the first argument. The extension
instances hold one instance per family: lifecycle, hierarchy, population,
usd and instancing. An instance of a family holds its family vtable
(``ovstage_ext_vtables.h``) and its context
(``ovstage_<family>_context_t*``). A function of the families reads its
member of ``instance->ext_instances.<family>.vtable`` and passes
``instance->ext_instances.<family>.context`` as the first argument, as a
data-plane call passes the data-plane context. The lifecycle functions
``ovstage_get_config`` and ``ovstage_destroy_instance`` stay exported
symbols. The members of the lifecycle vtable take the instance.
The library exports no symbol for the functions of the other families. The
path-dictionary interning API is reached
through the ``get_path_dictionary`` slot of the public vtable, which hands back
a ``path_dictionary_instance_t`` with its own vtable. The thread-local error
accessors stay flat symbols. Test gates are
discovered through the ``ovstage.test.hooks`` extension.

This module mirrors the C structs (exact field layout), the scalar typedefs,
the error/enum constants, the public vtable, the family vtables, the
data-plane instance and the extension instances. It binds the prototypes of
the remaining flat exports.
:class:`_InstanceApi` (via :func:`instance_api`) presents the data-plane slots
and the functions of the families under their historical ``ovstage_*`` names.
Each one takes the instance pointer or the pointer to the pair of its family
(``ovstage_data_plane_instance_t`` for a slot, ``ovstage_<family>_instance_t``
for a function of a family) as its first argument. Everything
here is intentionally low-level. The Pythonic surface lives in ``stage.py``,
``path_dictionary.py``, ``instancing.py``, ``population.py`` and
``population_export.py``.
"""

import ctypes
import functools
import operator
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional

from ..handles import INVALID_HIERARCHY_HANDLE as OVSTAGE_INVALID_HIERARCHY_HANDLE
from ..handles import INVALID_HIERARCHY_RESULT_ID as OVSTAGE_INVALID_HIERARCHY_RESULT_ID
from ..handles import INVALID_MAP_HANDLE as OVSTAGE_INVALID_MAP_HANDLE
from ..handles import INVALID_OP_ID as OVSTAGE_INVALID_OP_ID
from ..handles import INVALID_ORDINAL_QUERY_HANDLE as OVSTAGE_INVALID_ORDINAL_QUERY_HANDLE
from ..handles import INVALID_QUERY_HANDLE as OVSTAGE_INVALID_QUERY_HANDLE
from ..handles import INVALID_QUERY_RESULT_ID as OVSTAGE_INVALID_QUERY_RESULT_ID
from ..handles import INVALID_READ_GROUP_ID as OVSTAGE_INVALID_READ_GROUP_ID
from ..handles import INVALID_READ_HANDLE as OVSTAGE_INVALID_READ_HANDLE
from ..handles import (
    POPULATION_EXPORT_INVALID_DESTINATION_HANDLE as OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE,
)
from ..handles import POPULATION_INVALID_OP_ID as OVSTAGE_POPULATION_INVALID_OP_ID
from ..handles import POPULATION_INVALID_USD_REFERENCE_HANDLE as OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE
from ..handles import HierarchyHandle as ovstage_hierarchy_handle_t
from ..handles import HierarchyResultId as ovstage_hierarchy_result_id_t
from ..handles import MapHandle as ovstage_map_handle_t
from ..handles import OpId as ovstage_op_id_t
from ..handles import OrdinalQueryHandle as ovstage_ordinal_query_handle_t
from ..handles import PopulationExportDestinationHandle as ovstage_population_export_destination_handle_t
from ..handles import PopulationOpId as ovstage_population_op_id_t
from ..handles import PopulationUsdReferenceHandle as ovstage_population_usd_reference_handle_t
from ..handles import QueryHandle as ovstage_query_handle_t
from ..handles import QueryResultId as ovstage_query_result_id_t
from ..handles import ReadGroupId as ovstage_read_group_id_t
from ..handles import ReadHandle as ovstage_read_handle_t
from .dlpack import DLDataType, DLDevice, DLManagedTensorVersioned, DLTensor

# ── Scalar typedefs and dictionary identities ────────────────────────────
ovstage_api_status_t = ctypes.c_uint32
ovstage_ordinal_t = ctypes.c_int64
OVSTAGE_ORDINAL_MIN = -(1 << 63)
OVSTAGE_ORDINAL_MAX = (1 << 63) - 1
ovstage_timeout_ns_t = ctypes.c_uint64
ovstage_hierarchy_computation_model_id_t = ctypes.c_int
ovstage_hierarchy_relation_t = ctypes.c_int
from ..handles import PrimPath as ovx_primpath_t
from ..handles import PrimPathList as ovx_primpath_list_t
from ..handles import Token as ovx_token_t


class ovstage_instancing_path_resolution_t(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_int),
        ("instance_root", ovx_primpath_t),
        ("prototype_root", ovx_primpath_t),
        ("backing_path", ovx_primpath_t),
    ]

OVSTAGE_TIMEOUT_INFINITE = 0xFFFFFFFFFFFFFFFF


def check_timeout(timeout: int) -> int:
    """Validate and normalize a timeout to a ``uint64_t`` nanosecond count.

    ovstage timeouts (``ovstage_timeout_ns_t``) are unsigned nanoseconds:
    ``0`` polls without blocking, ``OVSTAGE_TIMEOUT_INFINITE`` blocks until
    completion, and any other value waits at most that many nanoseconds. Python
    passes the value through ctypes as ``c_uint64``, which would silently wrap
    a negative value or one ``>= 2**64`` (``-1`` happened to wrap to
    ``OVSTAGE_TIMEOUT_INFINITE``); reject non-integers and out-of-range values
    here instead. Lives in this module (not ``types.py``, which re-exports it)
    so :func:`flush_log` can use it without an import cycle.
    """
    try:
        value = operator.index(timeout)
    except TypeError:
        raise TypeError(
            "timeout must be an int (nanoseconds; 0 polls, TIMEOUT_INFINITE " f"blocks), got {type(timeout).__name__}"
        ) from None
    if value < 0:
        raise ValueError(f"timeout must be non-negative, got {timeout}; pass TIMEOUT_INFINITE to block")
    if value > OVSTAGE_TIMEOUT_INFINITE:
        raise ValueError(f"timeout must fit in uint64 (<= {OVSTAGE_TIMEOUT_INFINITE}), got {timeout}")
    return value


# ── Error codes (ovstage_api_types.h) ──────────────────────────────────────
OVSTAGE_OK = 0
OVSTAGE_ERROR_INVALID_ARGUMENT = 1
OVSTAGE_ERROR_INVALID_HANDLE = 2
OVSTAGE_ERROR_NOT_FOUND = 3
OVSTAGE_ERROR_PRIM_NOT_FOUND = 4
OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION = 5
OVSTAGE_ERROR_NOT_SUPPORTED = 6
OVSTAGE_ERROR_QUEUE_FULL = 7
OVSTAGE_ERROR_END_OF_ITERATION = 8
OVSTAGE_ERROR_OUT_OF_MEMORY = 9
OVSTAGE_ERROR_LAYOUT_CHANGED = 10
OVSTAGE_ERROR_TIMEOUT = 11
OVSTAGE_ERROR_OP_FAILED = 12
OVSTAGE_ERROR_OUT_OF_RANGE = 13
OVSTAGE_ERROR_INTERNAL = 99


OVSTAGE_HIERARCHY_COMPUTATION_MODEL_INVALID = 0
OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL = 1
OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_INCREMENTAL = 2
OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_GLOBAL = 3
OVSTAGE_HIERARCHY_COMPUTATION_MODEL_RUNTIME_DEFAULT = 4
OVSTAGE_HIERARCHY_COMPUTATION_MODEL_DEFAULT_CPU = OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL
OVSTAGE_HIERARCHY_COMPUTATION_MODEL_DEFAULT_GPU = OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_GLOBAL
OVSTAGE_INVALID_HIERARCHY_COMPUTATION_MODEL_ID = OVSTAGE_HIERARCHY_COMPUTATION_MODEL_INVALID

OVSTAGE_CONFIG_KEY_TYPE_BOOL = 0
OVSTAGE_CONFIG_KEY_TYPE_INT64 = 1
OVSTAGE_CONFIG_KEY_TYPE_UINT64 = 2
OVSTAGE_CONFIG_KEY_TYPE_DOUBLE = 3
OVSTAGE_CONFIG_KEY_TYPE_STRING = 4
OVSTAGE_CONFIG_KEY_TYPE_BLOB = 5

OVSTAGE_CONFIG_RUNTIME_DEFAULT_HIERARCHY_COMPUTATION_MODEL = 0
OVSTAGE_CONFIG_RUNTIME_DEFAULT_ORDINAL_ADMISSION_POLICY = 1
OVSTAGE_CONFIG_POPULATION_THREAD_LIMIT = 2
OVSTAGE_CONFIG_RETENTION_WINDOW = 3

OVSTAGE_POPULATION_THREAD_LIMIT_DEFAULT = 0
OVSTAGE_POPULATION_THREAD_LIMIT_AUTO = (1 << 64) - 1

OVSTAGE_ORDINAL_ADMISSION_DEFAULT = 0
OVSTAGE_ORDINAL_ADMISSION_FLOOR_ONLY = 1
OVSTAGE_ORDINAL_ADMISSION_NONDECREASING = 2
OVSTAGE_ORDINAL_ADMISSION_STRICTLY_INCREASING = 3

OVSTAGE_CONFIG_POPULATION_STRICT_RESOLUTION = 0

OVSTAGE_CONFIG_BINARY_PACKAGE_ROOT_PATH = 0

OVSTAGE_HIERARCHY_PARENT = 0
OVSTAGE_HIERARCHY_CHILDREN = 1
OVSTAGE_HIERARCHY_SIBLINGS = 2

# ovx/types.h — path-dictionary API result status (ovx_api_status_t). The
# refactored path-dictionary API reports only success/failure; a human-readable
# detail rides along in ``ovx_api_result_t.error``.
OVX_API_SUCCESS = 0
OVX_API_ERROR = 1

# Match C's UINT64_MAX; token ID 0 belongs to the empty string.
OVX_INVALID_TOKEN = ovx_token_t(0xFFFFFFFFFFFFFFFF)
OVX_INVALID_PRIMPATH = ovx_primpath_t(0)
OVX_INVALID_PRIMPATH_LIST = ovx_primpath_list_t(0)


# ── ovx string view types (ovx/string_types.h) ─────────────────────────────
class ovx_string_t(ctypes.Structure):
    """Non-owning UTF-8 string view: { const char* ptr; size_t length; }.

    Layout matches the anonymous-union (ptr/str, length/len) form in the C
    header. The optional ``value`` constructor encodes a Python string into an
    owned buffer kept alive on the instance (``_bytes``); pass the instance (or
    keep it referenced) for as long as the C side reads it.
    """

    _fields_ = [
        ("ptr", ctypes.c_char_p),
        ("length", ctypes.c_size_t),
    ]

    def __init__(self, value=None):
        super().__init__()
        if value is None:
            self.ptr = None
            self.length = 0
            return
        encoded = str(value).encode("utf-8")
        # Bytes already provide NUL-terminated storage; retaining them avoids a buffer/cast ownership cycle.
        self._bytes = encoded
        self.ptr = self._bytes
        self.length = len(encoded)

    def __bool__(self) -> bool:
        return self.ptr is not None and self.length > 0

    def __len__(self) -> int:
        return int(self.length)

    def __str__(self) -> str:
        if self.ptr is None:
            return ""
        return ctypes.string_at(self.ptr, self.length).decode("utf-8", errors="replace")

    def __repr__(self) -> str:
        return f"ovx_string_t({str(self)!r})"


@dataclass(frozen=True)
class BuildInfo:
    """Build information embedded in the loaded OVStage native library.

    Future versions may append fields while preserving the meaning and order
    of existing fields. ``build_string`` has the form
    ``version+branch.build_number.git_hash.build_location`` and starts with the
    OVStage semantic version. Native string views are decoded and copied, so
    every string stored in this Python object is owned by Python.

    :ivar int struct_size: Size of the native build-information structure.
    :ivar int build_number: Numeric CI or local build number.
    :ivar str build_string: Full version, build number, and source revision.
    :ivar str git_branch: Source branch used for the build.
    :ivar str git_hash: Source revision used for the build.
    :ivar str build_location: Build-system or location identifier.
    """

    struct_size: int
    build_number: int
    build_string: str
    git_branch: str
    git_hash: str
    build_location: str


class _ovstage_build_info_header_t(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32)]


class _ovstage_build_info_t(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("build_number", ctypes.c_uint32),
        ("build_string", ovx_string_t),
        ("git_branch", ovx_string_t),
        ("git_hash", ovx_string_t),
        ("build_location", ovx_string_t),
    ]


def _read_build_info(lib: ctypes.CDLL) -> BuildInfo:
    info_ptr = lib.ovstage_get_build_info()
    if not info_ptr:
        raise RuntimeError("ovstage_get_build_info returned null")
    info_header = ctypes.cast(info_ptr, ctypes.POINTER(_ovstage_build_info_header_t)).contents
    expected_size = ctypes.sizeof(_ovstage_build_info_t)
    if info_header.struct_size < expected_size:
        raise RuntimeError(
            f"ovstage_get_build_info returned struct_size={info_header.struct_size}, "
            f"smaller than the required {expected_size}"
        )
    info = info_ptr.contents

    def decode(name: str, value: ovx_string_t) -> str:
        if value.ptr is None:
            if value.length:
                raise RuntimeError(f"ovstage_get_build_info returned null {name} with non-zero length {value.length}")
            return ""
        try:
            return ctypes.string_at(value.ptr, value.length).decode("utf-8")
        except UnicodeDecodeError as exc:
            raise RuntimeError(f"ovstage_get_build_info returned invalid UTF-8 in {name}") from exc

    return BuildInfo(
        struct_size=info.struct_size,
        build_number=info.build_number,
        build_string=decode("build_string", info.build_string),
        git_branch=decode("git_branch", info.git_branch),
        git_hash=decode("git_hash", info.git_hash),
        build_location=decode("build_location", info.build_location),
    )


def _validate_build_info_version(build_info: BuildInfo, version: tuple[int, int, int]) -> None:
    runtime_version = ".".join(str(component) for component in version)
    embedded_version = build_info.build_string.partition("+")[0]
    if embedded_version != runtime_version:
        raise RuntimeError(
            f"ovstage build information mismatch: API reports {runtime_version} "
            f"but the embedded build string reports {embedded_version}"
        )


# ovstage_log_callback_t: void(ovstage_log_severity_t severity, double timestamp,
# ovx_string_t message /* by value */, void* user_data). ``severity`` is a signed
# enum (c_int); ``message`` is a non-owning view valid only for the call.
_log_callback_t = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_double, ovx_string_t, ctypes.c_void_p)


class ovx_token_or_string_t(ctypes.Structure):
    """Use string when its pointer is non-null; otherwise use token, including ID 0."""

    _fields_ = [
        ("token", ovx_token_t),
        ("string", ovx_string_t),
    ]


def make_token_or_string(value) -> ovx_token_or_string_t:
    """Build an ``ovx_token_or_string_t`` from a ``Token`` or a Python string.

    When a string is supplied, the backing buffer is kept alive on the returned
    struct (``_string_ref``) so it stays valid for the duration of the call it
    is passed to.

    Only ``Token`` and ``str`` are accepted. Integer storage values must be
    explicitly wrapped in ``Token``; another handle kind is always rejected.
    """
    out = ovx_token_or_string_t()
    if isinstance(value, str):
        s = ovx_string_t(value)
        out.token = ovx_token_t()
        out.string = s
        out._string_ref = s  # keepalive: s._bytes backs out.string.ptr
        return out
    # Imported lazily to avoid a bindings <- types import cycle at module load,
    # matching set_log_callback's LogSeverity import below.
    from .types import check_token

    out.token = check_token(value)
    return out


# ── Process configuration (ovstage_initialize) ─────────────────────────────
class ovstage_config_blob_value_t(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.c_void_p),
        ("size", ctypes.c_size_t),
    ]


class ovstage_config_entry_t(ctypes.Structure):
    """Single typed ovstage process-configuration entry."""

    class _KeyUnion(ctypes.Union):
        _fields_ = [
            ("bool_key", ctypes.c_int),
            ("int64_key", ctypes.c_int),
            ("uint64_key", ctypes.c_int),
            ("double_key", ctypes.c_int),
            ("string_key", ctypes.c_int),
            ("blob_key", ctypes.c_int),
        ]

    class _ValueUnion(ctypes.Union):
        _fields_ = [
            ("bool_value", ctypes.c_bool),
            ("int_value", ctypes.c_int64),
            ("uint_value", ctypes.c_uint64),
            ("double_value", ctypes.c_double),
            ("string_value", ovx_string_t),
            ("blob_value", ovstage_config_blob_value_t),
        ]

    _fields_ = [
        ("key_type", ctypes.c_int),
        ("key", _KeyUnion),
        ("value", _ValueUnion),
    ]


def ovstage_config_entry_uint64(key: int, value: int) -> ovstage_config_entry_t:
    """Build a config entry for an unsigned 64-bit setting."""
    entry = ovstage_config_entry_t()
    entry.key_type = OVSTAGE_CONFIG_KEY_TYPE_UINT64
    entry.key.uint64_key = key
    entry.value.uint_value = value
    return entry


def ovstage_config_entry_bool(key: int, value: bool) -> ovstage_config_entry_t:
    """Build a config entry for a boolean setting."""
    entry = ovstage_config_entry_t()
    entry.key_type = OVSTAGE_CONFIG_KEY_TYPE_BOOL
    entry.key.bool_key = key
    entry.value.bool_value = value
    return entry


def ovstage_config_entry_string(key: int, value: str) -> ovstage_config_entry_t:
    """Build a config entry for a string setting.

    ``value`` is a Python ``str``. The entry keeps the ``ovx_string_t`` view
    alive on itself (``_string``), because the union copy holds only the
    pointer and the length.
    """
    view = ovx_string_t(value)
    entry = ovstage_config_entry_t()
    entry.key_type = OVSTAGE_CONFIG_KEY_TYPE_STRING
    entry.key.string_key = key
    entry.value.string_value = view
    entry._string = view
    return entry


class ovstage_config_t(ctypes.Structure):
    """Config container passed to ``ovstage_initialize``."""

    _fields_ = [
        ("entries", ctypes.POINTER(ovstage_config_entry_t)),
        ("entry_count", ctypes.c_size_t),
    ]

    def __init__(self, entries: List[ovstage_config_entry_t]):
        if entries:
            self._array = (ovstage_config_entry_t * len(entries))(*entries)
            self._entries = entries
            super().__init__(entries=self._array, entry_count=len(entries))
        else:
            self._array = None
            self._entries = []
            super().__init__(entries=None, entry_count=0)


# ── Opaque handles ─────────────────────────────────────────────────────────
class ovstage_instance_t(ctypes.Structure):
    """The ``ovstage_instance_t`` struct (``{data_plane, ext_instances}``).

    Declared field-less here so ``ovstage_instance_p`` (and the prototypes that
    take it) can be defined before the vtable types exist. ``_fields_`` is
    assigned once ``ovstage_data_plane_instance_t`` and
    ``ovstage_ext_instances_t`` are defined below.
    """


ovstage_instance_p = ctypes.POINTER(ovstage_instance_t)


# ── Result / payload structs (ovstage.h) ───────────────────────────────────
class ovstage_enqueue_result_t(ctypes.Structure):
    _fields_ = [
        ("status", ovstage_api_status_t),
        ("op_index", ovstage_op_id_t),
    ]


class ovstage_op_wait_result_t(ctypes.Structure):
    _fields_ = [
        ("error_op_ids", ctypes.POINTER(ovstage_op_id_t)),
        ("error_op_id_count", ctypes.c_size_t),
        ("lowest_pending_op_id", ovstage_op_id_t),
    ]


class ovstage_clone_result_t(ctypes.Structure):
    _fields_ = [
        ("ordinal", ovstage_ordinal_t),
        ("target_count", ctypes.c_size_t),
        ("source_prim_count", ctypes.c_size_t),
        ("prims_cloned", ctypes.c_size_t),
    ]


# ── ovstage_population result structs (ovstage_population.h) ─────────────────
# The population USD bridge has its own async enqueue/wait model, parallel to
# but distinct from the data-plane one above: every mutating call returns an
# ``ovstage_population_enqueue_result_t`` by value and is awaited via the
# ``wait_op`` member of the population family vtable (there is no population
# release_op).
class ovstage_hierarchy_item_t(ctypes.Structure):
    _fields_ = [
        ("status", ovstage_api_status_t),
        ("path_offset", ctypes.c_size_t),
        ("path_count", ctypes.c_size_t),
    ]


class ovstage_hierarchy_result_t(ctypes.Structure):
    _fields_ = [
        ("hierarchy_result_id", ovstage_hierarchy_result_id_t),
        ("ordinal", ovstage_ordinal_t),
        ("items", ctypes.POINTER(ovstage_hierarchy_item_t)),
        ("input_count", ctypes.c_size_t),
        ("paths", ctypes.POINTER(ovx_token_or_string_t)),
        ("path_count", ctypes.c_size_t),
    ]


class ovstage_hierarchy_computation_model_desc_t(ctypes.Structure):
    _fields_ = [
        ("model_id", ovstage_hierarchy_computation_model_id_t),
        ("name", ovx_string_t),
        ("description", ovx_string_t),
    ]


class ovstage_population_enqueue_result_t(ctypes.Structure):
    _fields_ = [
        ("status", ovstage_api_status_t),
        ("op_index", ovstage_population_op_id_t),
    ]


class ovstage_population_prim_predicate_t(ctypes.Structure):
    # Mirrors the C prim predicate: kind, a union selecting how `values` is read, and
    # value_count. A prim predicate's union names a property predicate, so both unions
    # are filled in after both classes exist (see below).
    pass


class ovstage_population_property_predicate_t(ctypes.Structure):
    pass


class _prim_predicate_targeted_by(ctypes.Structure):
    _fields_ = [
        ("prim_predicate", ctypes.POINTER(ovstage_population_prim_predicate_t)),  # the referrers
        ("property_predicate", ctypes.POINTER(ovstage_population_property_predicate_t)),  # their relationships
    ]


class _prim_predicate_values(ctypes.Union):
    _fields_ = [
        ("strings", ctypes.POINTER(ovx_string_t)),
        ("subpredicates", ctypes.POINTER(ovstage_population_prim_predicate_t)),
        ("property_predicate", ctypes.POINTER(ovstage_population_property_predicate_t)),
        ("targeted_by", _prim_predicate_targeted_by),
    ]


class _property_predicate_values(ctypes.Union):
    _fields_ = [
        ("strings", ctypes.POINTER(ovx_string_t)),
        ("subpredicates", ctypes.POINTER(ovstage_population_property_predicate_t)),
        ("prim_predicate", ctypes.POINTER(ovstage_population_prim_predicate_t)),
    ]


ovstage_population_prim_predicate_t._fields_ = [
    ("kind", ctypes.c_int),
    ("values", _prim_predicate_values),
    ("value_count", ctypes.c_size_t),
]

ovstage_population_property_predicate_t._fields_ = [
    ("kind", ctypes.c_int),
    ("values", _property_predicate_values),
    ("value_count", ctypes.c_size_t),
]


class ovstage_population_expression_t(ctypes.Structure):
    # Mirrors the C expression: kind, a union selecting how `values` is read, and value_count.
    pass


class _expression_resolved(ctypes.Structure):
    _fields_ = [
        ("name", ovx_string_t),
        ("prim_predicate", ctypes.POINTER(ovstage_population_prim_predicate_t)),
        ("property_predicate", ctypes.POINTER(ovstage_population_property_predicate_t)),
    ]


class _expression_values(ctypes.Union):
    _fields_ = [
        ("number", ctypes.c_double),
        ("boolean", ctypes.c_bool),
        ("name", ovx_string_t),
        ("operands", ctypes.POINTER(ovstage_population_expression_t) * 2),
        ("resolved", _expression_resolved),
    ]


ovstage_population_expression_t._fields_ = [
    ("kind", ctypes.c_int),
    ("values", _expression_values),
    ("value_count", ctypes.c_size_t),
]


class ovstage_population_derived_property_t(ctypes.Structure):
    _fields_ = [
        ("name", ovx_string_t),
        ("multi_apply_schema_family", ovx_string_t),
        ("prim_predicate", ctypes.POINTER(ovstage_population_prim_predicate_t)),
        ("when", ctypes.c_int),
        ("dtype", DLDataType),
        ("semantic", ctypes.c_int),
        ("is_array", ctypes.c_bool),
        ("value", ctypes.POINTER(ovstage_population_expression_t)),
    ]


class ovstage_population_selector_t(ctypes.Structure):
    _fields_ = [
        ("prim_predicate", ovstage_population_prim_predicate_t),
        ("property_predicate", ovstage_population_property_predicate_t),
        ("prim_metadata_paths", ctypes.POINTER(ovx_string_t)),
        ("prim_metadata_path_count", ctypes.c_size_t),
        ("property_metadata_paths", ctypes.POINTER(ovx_string_t)),
        ("property_metadata_path_count", ctypes.c_size_t),
        ("derived_properties", ctypes.POINTER(ovstage_population_derived_property_t)),
        ("derived_property_count", ctypes.c_size_t),
    ]


class ovstage_population_desc_t(ctypes.Structure):
    _fields_ = [
        ("domains", ctypes.c_uint32),
        ("selectors", ctypes.POINTER(ovstage_population_selector_t)),
        ("selector_count", ctypes.c_size_t),
        ("stage_metadata_paths", ctypes.POINTER(ovx_string_t)),
        ("stage_metadata_path_count", ctypes.c_size_t),
    ]


class ovstage_population_op_wait_result_t(ctypes.Structure):
    _fields_ = [
        ("error_op_ids", ctypes.POINTER(ovstage_population_op_id_t)),
        ("error_op_id_count", ctypes.c_size_t),
        ("lowest_pending_op_id", ovstage_population_op_id_t),
    ]


# ── ovstage_population export types (ovstage_population_types.h) ─────────────
# The types of the export functions of the population family: the destination
# handle, the rules, the descriptor and the report. The export members of the
# population family vtable below name them. ``population_export_bindings.py``
# re-exports them next to the export constants.


class ovstage_population_export_attribute_rule_t(ctypes.Structure):
    _fields_ = [
        ("path", ovx_string_t),
        ("path_match", ctypes.c_int),
        ("source_attribute_name", ovx_string_t),
        ("source_attribute_name_match", ctypes.c_int),
        ("destination_attribute_name", ovx_string_t),
        ("usd_type_name", ovx_string_t),
        ("required_schema_name", ovx_string_t),
        ("flags", ctypes.c_uint32),
    ]


class ovstage_population_export_prim_rule_t(ctypes.Structure):
    _fields_ = [
        ("path", ovx_string_t),
        ("path_match", ctypes.c_int),
        ("usd_type_name", ovx_string_t),
        ("unknown_type_metadata_key", ovx_string_t),
    ]


class ovstage_population_export_api_schema_rule_t(ctypes.Structure):
    _fields_ = [
        ("path", ovx_string_t),
        ("path_match", ctypes.c_int),
        ("schema_name", ovx_string_t),
        ("instance_name", ovx_string_t),
        ("flags", ctypes.c_uint32),
    ]


class ovstage_population_export_property_edge_rule_t(ctypes.Structure):
    _fields_ = [
        ("path", ovx_string_t),
        ("path_match", ctypes.c_int),
        ("source_property_name", ovx_string_t),
        ("destination_property_name", ovx_string_t),
        ("usd_type_name", ovx_string_t),
        ("property_kind", ctypes.c_int),
        ("flags", ctypes.c_uint32),
    ]


class ovstage_population_export_metadata_rule_t(ctypes.Structure):
    _fields_ = [
        ("path", ovx_string_t),
        ("path_match", ctypes.c_int),
        ("source_metadata_name", ovx_string_t),
        ("destination_metadata_name", ovx_string_t),
        ("metadata_kind", ctypes.c_int),
        ("flags", ctypes.c_uint32),
    ]


class ovstage_population_export_desc_t(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("source_api_schema_policy", ctypes.c_int),
        ("projection", ctypes.c_int),
        ("ordinal", ovstage_ordinal_t),
        ("since_ordinal", ovstage_ordinal_t),
        ("selection", ctypes.c_int),
        ("layer_mode", ctypes.c_int),
        ("transform_policy", ctypes.c_int),
        ("unknown_metadata_policy", ctypes.c_int),
        ("prim_predicate", ovstage_population_prim_predicate_t),
        ("property_predicate", ovstage_population_property_predicate_t),
        ("prim_rules", ctypes.POINTER(ovstage_population_export_prim_rule_t)),
        ("prim_rule_count", ctypes.c_size_t),
        ("api_schema_rules", ctypes.POINTER(ovstage_population_export_api_schema_rule_t)),
        ("api_schema_rule_count", ctypes.c_size_t),
        ("attribute_rules", ctypes.POINTER(ovstage_population_export_attribute_rule_t)),
        ("attribute_rule_count", ctypes.c_size_t),
        ("property_rules", ctypes.POINTER(ovstage_population_export_property_edge_rule_t)),
        ("property_rule_count", ctypes.c_size_t),
        ("metadata_rules", ctypes.POINTER(ovstage_population_export_metadata_rule_t)),
        ("metadata_rule_count", ctypes.c_size_t),
    ]


class ovstage_population_export_report_t(ctypes.Structure):
    _fields_ = [
        ("prims_examined", ctypes.c_size_t),
        ("prims_exported", ctypes.c_size_t),
        ("prims_skipped", ctypes.c_size_t),
        ("attributes_examined", ctypes.c_size_t),
        ("attributes_exported", ctypes.c_size_t),
        ("attributes_skipped", ctypes.c_size_t),
        ("attributes_unsupported", ctypes.c_size_t),
        ("api_schemas_examined", ctypes.c_size_t),
        ("api_schemas_applied", ctypes.c_size_t),
        ("api_schemas_skipped", ctypes.c_size_t),
        ("custom_attributes_inferred", ctypes.c_size_t),
        ("unknown_metadata_preserved", ctypes.c_size_t),
        ("transforms_examined", ctypes.c_size_t),
        ("transforms_exported", ctypes.c_size_t),
        ("property_edges_examined", ctypes.c_size_t),
        ("property_edges_exported", ctypes.c_size_t),
        ("property_edges_skipped", ctypes.c_size_t),
        ("property_edges_unsupported", ctypes.c_size_t),
        ("metadata_examined", ctypes.c_size_t),
        ("metadata_exported", ctypes.c_size_t),
        ("metadata_skipped", ctypes.c_size_t),
        ("metadata_unsupported", ctypes.c_size_t),
        ("source_states_unavailable", ctypes.c_size_t),
    ]


class ovstage_cuda_sync_t(ctypes.Structure):
    # Mirrors C ovstage_cuda_sync_t { uintptr_t stream; uintptr_t wait_event; }.
    # stream: 0 = no sync, 1 = default stream, >1 = a specific cudaStream_t.
    # wait_event: cudaEvent_t to wait on before the op (0 = none). {0,0} = no sync.
    _fields_ = [
        ("stream", ctypes.c_size_t),
        ("wait_event", ctypes.c_size_t),
    ]


class ovstage_placement_t(ctypes.Structure):
    # Mirrors C ovstage_placement_t (experimental; in
    # ovstage_api/experimental/ovstage_placement_extension.h)
    # { DLDevice device; uintptr_t stream; uintptr_t availability_event; }.
    # Destination residency for a whole operation, as opposed to ovstage_cuda_sync_t,
    # which describes the source handoff. A NULL pointer means unspecified (follow
    # the source tensor). ``stream`` is the stream the operation's device work is
    # issued on: 0 or 1 = the legacy default stream, 2 = the per-thread default
    # stream, otherwise a specific cudaStream_t.
    # ``availability_event`` is a caller-owned cudaEvent_t ovstage records into once
    # the data the op makes available is safe to dereference (0 = none) -- the one
    # outbound field here, where ovstage_cuda_sync_t's wait_event is inbound. On a
    # read it is deliberately not the op's own completion: the read op completes
    # first, and the event also covers device work the caller never issued.
    #
    # The C struct carries no size or version field, so this mirror must match it
    # field for field: a missing trailing member means the native side reads past
    # the end of what Python allocated. A change to it bumps
    # OVSTAGE_PLACEMENT_EXTENSION_VERSION.
    _fields_ = [
        ("device", DLDevice),
        ("stream", ctypes.c_size_t),
        ("availability_event", ctypes.c_size_t),
    ]


def make_placement(device_type: int, device_id: int, stream: int, availability_event: int) -> ovstage_placement_t:
    """Build an ovstage_placement_t. C callers use OVSTAGE_PLACEMENT_INIT.

    Every field is required rather than defaulted: the struct has no size field,
    so each member this helper forgets to set is a member the native side still
    reads.
    """
    placement = ovstage_placement_t()
    placement.device = DLDevice(device_type, device_id)
    placement.stream = stream
    placement.availability_event = availability_event
    return placement


class ovstage_data_t(ctypes.Structure):
    _fields_ = [
        ("tensors", ctypes.POINTER(DLTensor)),
        ("tensor_count", ctypes.c_uint32),
        ("count", ctypes.c_uint32),
        ("index_map", ctypes.POINTER(ctypes.c_uint32)),
        ("mask", ctypes.POINTER(ctypes.c_uint64)),
        ("cuda_sync", ovstage_cuda_sync_t),
    ]


class ovstage_prim_group_t(ctypes.Structure):
    _fields_ = [
        ("list", ovx_primpath_list_t),
        ("offset", ctypes.c_uint32),
        ("count", ctypes.c_uint32),
        ("index_map", ctypes.POINTER(ctypes.c_uint32)),
    ]


class ovstage_attribute_meta_t(ctypes.Structure):
    _fields_ = [
        ("attribute_write_floor_ordinal", ovstage_ordinal_t),
        ("layout_generation", ctypes.c_uint64),
    ]


class ovstage_ordinal_range_t(ctypes.Structure):
    _fields_ = [
        ("start_ordinal", ovstage_ordinal_t),
        ("end_ordinal", ovstage_ordinal_t),
        ("has_start_ordinal", ctypes.c_bool),
    ]


class ovstage_read_column_t(ctypes.Structure):
    _fields_ = [
        ("attribute", ovx_token_t),
        ("ordinal", ovstage_ordinal_t),
        ("is_delete", ctypes.c_bool),
        ("is_array", ctypes.c_bool),  # ragged/CSR source column (vs fixed scalar)
        ("semantic", ctypes.c_int),  # ovstage_attribute_semantic_t (recovered from column metadata)
        ("data", ovstage_data_t),
        ("meta", ovstage_attribute_meta_t),
    ]


class ovstage_read_group_t(ctypes.Structure):
    _fields_ = [
        ("read_group_id", ovstage_read_group_id_t),
        ("prims", ovstage_prim_group_t),
        ("columns", ctypes.POINTER(ovstage_read_column_t)),
        ("column_count", ctypes.c_size_t),
    ]


class ovstage_map_group_t(ctypes.Structure):
    _fields_ = [
        ("prims", ovstage_prim_group_t),
        ("data", ovstage_data_t),
        ("meta", ovstage_attribute_meta_t),
    ]


class ovstage_predicate_t(ctypes.Structure):
    _fields_ = [
        ("attribute", ovx_token_or_string_t),
        ("op", ctypes.c_int),
        ("values", ctypes.POINTER(ovx_string_t)),
        ("value_count", ctypes.c_size_t),
    ]


class ovstage_filter_t(ctypes.Structure):
    _fields_ = [
        ("predicates", ctypes.POINTER(ovstage_predicate_t)),
        ("count", ctypes.c_size_t),
    ]


class ovstage_query_result_t(ctypes.Structure):
    _fields_ = [
        ("query_result_id", ovstage_query_result_id_t),
        ("attributes", ctypes.POINTER(ovx_token_t)),
        ("attribute_count", ctypes.c_size_t),
        ("all_handle", ovstage_query_handle_t),
        ("total_prim_count", ctypes.c_size_t),
    ]


class ovstage_write_floor_desc_t(ctypes.Structure):
    _fields_ = [
        ("ordinal", ovstage_ordinal_t),
        ("scope", ctypes.c_int),
        ("attributes", ctypes.POINTER(ovx_token_t)),
        ("attribute_count", ctypes.c_size_t),
    ]


class ovstage_write_data_t(ctypes.Structure):
    _fields_ = [
        ("tensors", ctypes.POINTER(DLTensor)),
        ("managed_tensors", ctypes.POINTER(ctypes.POINTER(DLManagedTensorVersioned))),
        ("tensor_count", ctypes.c_uint32),
        ("count", ctypes.c_uint32),
        ("index_map", ctypes.POINTER(ctypes.c_uint32)),
        ("mask", ctypes.POINTER(ctypes.c_uint64)),
        ("cuda_sync", ovstage_cuda_sync_t),
        # ovstage_attribute_semantic_t (NONE=0). Geometric and ID semantics are
        # recorded in column metadata and ID semantics require pre-interned payloads:
        #   TOKEN_ID / RELATIONSHIP_PATH_ID → dtype {kDLUInt, 64, 1}
        #   CONNECTION_PATH_ID              → dtype {kDLUInt, 64, 2}
        # See AttributeSemantic in ovstage.types for details.
        ("semantic", ctypes.c_int),
        ("is_array", ctypes.c_bool),
        ("attribute_mode", ctypes.c_int),  # ovstage_attribute_mode_t (REJECT=0 / FORCE)
    ]


class ovstage_attribute_write_t(ctypes.Structure):
    _fields_ = [
        ("attribute", ovx_token_or_string_t),
        ("data", ovstage_write_data_t),
    ]


class ovstage_map_desc_t(ctypes.Structure):
    _fields_ = [
        ("attribute", ovx_token_or_string_t),
        ("dtype", DLDataType),  # element type used only when creating a new column (else ignored)
        # ovstage_attribute_semantic_t (NONE=0). Records semantic metadata when
        # the map creates a column; see ovstage_write_data_t
        # for the geometric vs. ID semantic split.
        ("semantic", ctypes.c_int),
        ("prim_mode", ctypes.c_int),  # ovstage_prim_mode_t (UPSERT / INSERT)
        ("attribute_mode", ctypes.c_int),  # ovstage_attribute_mode_t (REJECT=0 / FORCE)
    ]


class ovstage_instance_desc_t(ctypes.Structure):
    """Descriptor of ``ovstage_create_instance`` (``ovstage_instance_desc_t``).

    ``implementation`` points to the UTF-8 name of the implementation of the
    library that creates the instance. NULL or an empty string selects the
    default implementation. A name that no implementation registered makes the
    creation fail with ``OVSTAGE_ERROR_INVALID_ARGUMENT``.
    """

    _fields_ = [
        ("name", ctypes.c_char_p),
        ("implementation", ctypes.c_char_p),
    ]


# ── ovx path-dictionary API (ovx/path_dictionary/path_dictionary.h) ─────────
# The dictionary is owned by the ovstage instance and reached through the
# ``get_path_dictionary`` vtable slot below; it is itself a ``{vtable, context}``
# bundle. Every slot returns an ``ovx_api_result_t`` by value and takes the
# dictionary context as its first argument. Slot order must match
# ``path_dictionary_vtable_t`` in path_dictionary.h exactly.
class ovx_api_result_t(ctypes.Structure):
    """Result of a path-dictionary call: ``{ovx_api_status_t status; ovx_string_t error;}``.

    ``status`` is ``OVX_API_SUCCESS`` (0) or ``OVX_API_ERROR`` (1). On error,
    ``error`` carries an owned message that must be handed back to the dictionary
    via the ``release_error`` slot once it has been copied out.
    """

    _fields_ = [
        ("status", ctypes.c_int),
        ("error", ovx_string_t),
    ]


_PD_CTX = ctypes.c_void_p  # path_dictionary_context_t* (opaque)
_PD_RES = ovx_api_result_t

_FN_create_tokens_from_strings = ctypes.CFUNCTYPE(
    _PD_RES, _PD_CTX, ctypes.POINTER(ovx_string_t), ctypes.c_size_t, ctypes.POINTER(ovx_token_t)
)
_FN_create_paths_from_tokens = ctypes.CFUNCTYPE(
    _PD_RES,
    _PD_CTX,
    ctypes.POINTER(ovx_token_t),
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_size_t,
    ctypes.POINTER(ovx_primpath_t),
)
_FN_create_paths_from_strings = ctypes.CFUNCTYPE(
    _PD_RES, _PD_CTX, ctypes.POINTER(ovx_string_t), ctypes.c_size_t, ctypes.POINTER(ovx_primpath_t)
)
_FN_create_path_list_from_paths = ctypes.CFUNCTYPE(
    _PD_RES, _PD_CTX, ctypes.POINTER(ovx_primpath_t), ctypes.c_size_t, ctypes.POINTER(ovx_primpath_list_t)
)
_FN_create_path_list_from_strings = ctypes.CFUNCTYPE(
    _PD_RES, _PD_CTX, ctypes.POINTER(ovx_string_t), ctypes.c_size_t, ctypes.POINTER(ovx_primpath_list_t)
)
_FN_add_path_list_reference = ctypes.CFUNCTYPE(_PD_RES, _PD_CTX, ovx_primpath_list_t)
_FN_release_path_list_reference = ctypes.CFUNCTYPE(_PD_RES, _PD_CTX, ovx_primpath_list_t)
_FN_get_strings_from_tokens = ctypes.CFUNCTYPE(
    _PD_RES, _PD_CTX, ctypes.POINTER(ovx_token_t), ctypes.c_size_t, ctypes.POINTER(ovx_string_t)
)
_FN_get_tokens_from_paths = ctypes.CFUNCTYPE(
    _PD_RES,
    _PD_CTX,
    ctypes.POINTER(ovx_primpath_t),
    ctypes.c_size_t,
    ctypes.POINTER(ovx_token_t),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.POINTER(ovx_token_t)),
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_size_t),
)
_FN_get_num_paths_from_path_list = ctypes.CFUNCTYPE(
    _PD_RES, _PD_CTX, ovx_primpath_list_t, ctypes.POINTER(ctypes.c_size_t)
)
_FN_get_paths_from_path_list = ctypes.CFUNCTYPE(
    _PD_RES,
    _PD_CTX,
    ovx_primpath_list_t,
    ctypes.c_size_t,
    ctypes.c_size_t,
    ctypes.POINTER(ovx_primpath_t),
    ctypes.POINTER(ctypes.c_size_t),
)
_FN_release_error = ctypes.CFUNCTYPE(None, _PD_CTX, ovx_string_t)


class path_dictionary_vtable_t(ctypes.Structure):
    _fields_ = [
        ("create_tokens_from_strings", _FN_create_tokens_from_strings),
        ("create_paths_from_tokens", _FN_create_paths_from_tokens),
        ("create_paths_from_strings", _FN_create_paths_from_strings),
        ("create_path_list_from_paths", _FN_create_path_list_from_paths),
        ("create_path_list_from_strings", _FN_create_path_list_from_strings),
        ("add_path_list_reference", _FN_add_path_list_reference),
        ("release_path_list_reference", _FN_release_path_list_reference),
        ("get_strings_from_tokens", _FN_get_strings_from_tokens),
        ("get_tokens_from_paths", _FN_get_tokens_from_paths),
        ("get_num_paths_from_path_list", _FN_get_num_paths_from_path_list),
        ("get_paths_from_path_list", _FN_get_paths_from_path_list),
        ("release_error", _FN_release_error),
    ]


class path_dictionary_instance_t(ctypes.Structure):
    """``{path_dictionary_vtable_t* vtable; path_dictionary_context_t* context;}``."""

    _fields_ = [
        ("vtable", ctypes.POINTER(path_dictionary_vtable_t)),
        ("context", _PD_CTX),
    ]


path_dictionary_instance_p = ctypes.POINTER(path_dictionary_instance_t)


# ── ovstage_api vtable (ovstage_api.h) ──────────────────────────────────────
# The data plane is reached through an append-only vtable. Every slot takes the implementation
# context (ovstage_data_plane_context_t*) as its first argument — NOT the ovstage_instance_t
# bundle. The CFUNCTYPE argtypes/restype below ARE the data-plane prototypes
# (they replace the flat ovstage_* prototypes that no longer exist as exports).
# Slot order must match ovstage_data_plane_vtable_t in ovstage_api.h exactly.
ovstage_data_plane_context_p = ctypes.c_void_p  # ovstage_data_plane_context_t* (opaque)

# The context of a family (ovstage_api/ovstage_api_types.h). Each type is an
# opaque struct type, as ovstage_data_plane_context_t is. The instance of a family holds
# the context of its family next to its family vtable.
ovstage_lifecycle_context_p = ctypes.c_void_p  # ovstage_lifecycle_context_t* (opaque)
ovstage_hierarchy_context_p = ctypes.c_void_p  # ovstage_hierarchy_context_t* (opaque)
ovstage_population_context_p = ctypes.c_void_p  # ovstage_population_context_t* (opaque)
ovstage_usd_context_p = ctypes.c_void_p  # ovstage_usd_context_t* (opaque)
ovstage_instancing_context_p = ctypes.c_void_p  # ovstage_instancing_context_t* (opaque)

_CTX = ovstage_data_plane_context_p
_ENQ = ovstage_enqueue_result_t
_ERR = ovstage_api_status_t
_U32P = ctypes.POINTER(ctypes.c_uint32)
_CFT = ctypes.CFUNCTYPE


class ovstage_data_plane_vtable_t(ctypes.Structure):
    _fields_ = [
        # Op tracking
        ("wait_op", _CFT(_ERR, _CTX, ovstage_op_id_t, ovstage_timeout_ns_t, ctypes.POINTER(ovstage_op_wait_result_t))),
        ("release_op", _CFT(_ERR, _CTX, ovstage_op_id_t)),
        # Write-floor advance
        ("advance_write_floor", _CFT(_ENQ, _CTX, ctypes.POINTER(ovstage_write_floor_desc_t))),
        # Ordinal queries
        ("get_oldest_preserved_ordinal", _CFT(_ENQ, _CTX, ctypes.POINTER(ovstage_ordinal_query_handle_t))),
        (
            "get_attribute_write_floor",
            _CFT(_ENQ, _CTX, ovx_token_or_string_t, ctypes.POINTER(ovstage_ordinal_query_handle_t)),
        ),
        (
            "fetch_ordinal",
            _CFT(_ERR, _CTX, ovstage_ordinal_query_handle_t, ovstage_timeout_ns_t, ctypes.POINTER(ovstage_ordinal_t)),
        ),
        ("release_ordinal_query", _CFT(_ENQ, _CTX, ovstage_ordinal_query_handle_t)),
        # Query
        (
            "query",
            _CFT(
                _ENQ,
                _CTX,
                ctypes.POINTER(ovstage_filter_t),
                ctypes.POINTER(ovx_token_t),
                ctypes.c_size_t,
                ctypes.POINTER(ovstage_query_handle_t),
            ),
        ),
        ("query_from_path_list", _CFT(_ERR, _CTX, ovx_primpath_list_t, ctypes.POINTER(ovstage_query_handle_t))),
        (
            "fetch_query_result",
            _CFT(_ERR, _CTX, ovstage_query_handle_t, ovstage_timeout_ns_t, ctypes.POINTER(ovstage_query_result_t)),
        ),
        ("release_query_result", _CFT(_ERR, _CTX, ctypes.POINTER(ovstage_query_result_t))),
        ("release_query", _CFT(_ENQ, _CTX, ovstage_query_handle_t)),
        # Read
        (
            "read_attributes",
            _CFT(
                _ENQ,
                _CTX,
                ovstage_query_handle_t,
                ctypes.POINTER(ovx_token_t),
                ctypes.c_size_t,
                ovstage_ordinal_range_t,
                ctypes.POINTER(ovstage_read_handle_t),
            ),
        ),
        (
            "fetch_read_next",
            _CFT(_ERR, _CTX, ovstage_read_handle_t, ovstage_timeout_ns_t, ctypes.POINTER(ovstage_read_group_t),
                 ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)),
        ),
        ("release_groups", _CFT(_ERR, _CTX, ctypes.POINTER(ovstage_read_group_t), ctypes.c_size_t)),
        ("release_read", _CFT(_ENQ, _CTX, ovstage_read_handle_t)),
        # Write (copy-in)
        (
            "write_attribute",
            _CFT(
                _ENQ,
                _CTX,
                ovstage_query_handle_t,
                ovx_token_or_string_t,
                ovstage_ordinal_t,
                ovstage_write_data_t,
                ctypes.c_int,
            ),
        ),
        # Map / unmap (writable staging buffers)
        (
            "map_attribute",
            _CFT(
                _ENQ,
                _CTX,
                ovstage_query_handle_t,
                ctypes.POINTER(ovstage_map_desc_t),
                ovstage_ordinal_t,
                ctypes.POINTER(ctypes.c_size_t),
                ctypes.c_size_t,
                ctypes.POINTER(ovstage_map_handle_t),
            ),
        ),
        (
            "fetch_map_next",
            _CFT(_ERR, _CTX, ovstage_map_handle_t, ovstage_timeout_ns_t, ctypes.POINTER(ovstage_map_group_t)),
        ),
        (
            "unmap_group",
            _CFT(_ENQ, _CTX, ovstage_map_handle_t, ctypes.POINTER(ovstage_map_group_t), ovstage_cuda_sync_t),
        ),
        ("unmap_attribute", _CFT(_ENQ, _CTX, ovstage_map_handle_t, ovstage_cuda_sync_t)),
        # Structural
        (
            "delete_attributes",
            _CFT(
                _ENQ,
                _CTX,
                ovstage_query_handle_t,
                ctypes.POINTER(ovx_token_or_string_t),
                ctypes.c_size_t,
                ovstage_ordinal_t,
            ),
        ),
        # Diagnostics
        ("get_version", _CFT(None, _CTX, _U32P, _U32P, _U32P)),
        ("get_error_string", _CFT(ctypes.c_char_p, _CTX, ovstage_api_status_t)),
        ("get_last_op_error", _CFT(ovx_string_t, _CTX, ovstage_op_id_t)),
        # Resources
        ("get_path_dictionary", _CFT(path_dictionary_instance_p, _CTX)),
        # Batched write
        (
            "write_attributes",
            _CFT(
                _ENQ,
                _CTX,
                ovstage_query_handle_t,
                ctypes.POINTER(ovstage_attribute_write_t),
                ctypes.c_size_t,
                ovstage_ordinal_t,
                ctypes.c_int,
            ),
        ),
        # Extensions
        ("query_extension", _CFT(_ERR, _CTX, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p))),
        # Subtree clone
        (
            "clone",
            _CFT(_ENQ, _CTX, ovx_string_t, ctypes.POINTER(ovx_string_t), ctypes.c_size_t, ovstage_ordinal_t),
        ),
        (
            "fetch_clone_result",
            _CFT(_ERR, _CTX, ovstage_op_id_t, ovstage_timeout_ns_t, ctypes.POINTER(ovstage_clone_result_t)),
        ),
    ]


ovstage_vtable_p = ctypes.POINTER(ovstage_data_plane_vtable_t)


# ── Device-placement extension ("ovstage.placement"), experimental ──────────
# Not a vtable slot: reached through query_extension, so the vtable above keeps
# its released layout and an older library reports NOT_FOUND instead of being
# called through a slot it does not have. Field order must match
# ovstage_placement_extension_v1_t in
# ovstage_api/experimental/ovstage_placement_extension.h exactly.
class ovstage_extension_header_t(ctypes.Structure):
    """``ovstage_extension_header_t``: begins every extension table."""

    _fields_ = [("struct_size", ctypes.c_uint32), ("version", ctypes.c_uint32)]


OVSTAGE_PLACEMENT_EXTENSION_NAME = b"ovstage.placement"
# Accepted by exact match only; see _placement_extension.
OVSTAGE_PLACEMENT_EXTENSION_VERSION = 1


class ovstage_placement_extension_v1_t(ctypes.Structure):
    """``ovstage_placement_extension_v1_t``: placed write, map and read."""

    _fields_ = [
        ("header", ovstage_extension_header_t),
        (
            "write_attribute_on_device",
            _CFT(
                _ENQ,
                _CTX,
                ovstage_query_handle_t,
                ovx_token_or_string_t,
                ovstage_ordinal_t,
                ovstage_write_data_t,
                ctypes.c_int,
                ctypes.POINTER(ovstage_placement_t),
            ),
        ),
        (
            "write_attributes_on_device",
            _CFT(
                _ENQ,
                _CTX,
                ovstage_query_handle_t,
                ctypes.POINTER(ovstage_attribute_write_t),
                ctypes.c_size_t,
                ovstage_ordinal_t,
                ctypes.c_int,
                ctypes.POINTER(ovstage_placement_t),
            ),
        ),
        # Unlike the two write entries, which take the placement last, this one
        # takes it *before* out_map_handle -- that is the C signature.
        (
            "map_attribute_on_device",
            _CFT(
                _ENQ,
                _CTX,
                ovstage_query_handle_t,
                ctypes.POINTER(ovstage_map_desc_t),
                ovstage_ordinal_t,
                ctypes.POINTER(ctypes.c_size_t),
                ctypes.c_size_t,
                ctypes.POINTER(ovstage_placement_t),
                ctypes.POINTER(ovstage_map_handle_t),
            ),
        ),
        # The placed read, like the placed map, takes the placement *before* its
        # out-handle. Unlike the placed write and map it is permissive: a column
        # that is not current on the named device is made available there rather
        # than the read failing, so there is no device-residency status to mirror
        # here.
        (
            "read_attributes_on_device",
            _CFT(
                _ENQ,
                _CTX,
                ovstage_query_handle_t,
                ctypes.POINTER(ovx_token_t),
                ctypes.c_size_t,
                ovstage_ordinal_range_t,
                ctypes.POINTER(ovstage_placement_t),
                ctypes.POINTER(ovstage_read_handle_t),
            ),
        ),
    ]


# ── Extension vtables (ovstage_ext_vtables.h) ────────────────────────────────
# The family vtables of the extension instances of an instance. A family
# vtable holds one member per function. A member has the exact prototype of
# its function. It takes the context of its family as its first argument, as a
# data-plane slot takes the data-plane context. The lifecycle vtable is the
# exception: get_config and destroy_instance take the instance. The member
# order must match ovstage_ext_vtables.h exactly, which defines every family
# vtable. An implementation that does not serve a function points its member
# at a stub that answers OVSTAGE_ERROR_NOT_SUPPORTED.

_INST = ovstage_instance_p
_HIER_CTX = ovstage_hierarchy_context_p
_POP_CTX = ovstage_population_context_p
_USD_CTX = ovstage_usd_context_p
_INSTANCING_CTX = ovstage_instancing_context_p
_POP_ENQ = ovstage_population_enqueue_result_t
_POP_DESC_P = ctypes.POINTER(ovstage_population_desc_t)
_EXPORT_DEST = ovstage_population_export_destination_handle_t
_EXPORT_DESC_P = ctypes.POINTER(ovstage_population_export_desc_t)
_EXPORT_REPORT_P = ctypes.POINTER(ovstage_population_export_report_t)


class ovstage_lifecycle_vtable_t(ctypes.Structure):
    """The lifecycle family vtable: ``ovstage_get_config`` and ``ovstage_destroy_instance``."""

    _fields_ = [
        ("get_config", _CFT(_ERR, _INST, ctypes.POINTER(ovstage_config_t))),
        ("destroy_instance", _CFT(_ERR, _INST)),
    ]


class ovstage_hierarchy_vtable_t(ctypes.Structure):
    """The hierarchy family vtable: ``ovstage_get_hierarchy`` to ``ovstage_compute_hierarchy``."""

    _fields_ = [
        (
            "get_hierarchy",
            _CFT(
                _ENQ,
                _HIER_CTX,
                ovx_primpath_list_t,
                ovstage_ordinal_t,
                ovstage_hierarchy_relation_t,
                ctypes.POINTER(ovstage_hierarchy_handle_t),
            ),
        ),
        (
            "fetch_hierarchy_result",
            _CFT(_ERR, _HIER_CTX, ovstage_hierarchy_handle_t, ctypes.POINTER(ovstage_hierarchy_result_t)),
        ),
        ("release_hierarchy_result", _CFT(_ERR, _HIER_CTX, ctypes.POINTER(ovstage_hierarchy_result_t))),
        ("release_hierarchy", _CFT(_ENQ, _HIER_CTX, ovstage_hierarchy_handle_t)),
        (
            "get_hierarchy_computation_models",
            _CFT(
                _ERR,
                _HIER_CTX,
                ctypes.POINTER(ctypes.POINTER(ovstage_hierarchy_computation_model_desc_t)),
                ctypes.POINTER(ctypes.c_size_t),
            ),
        ),
        (
            "compute_hierarchy",
            _CFT(_ENQ, _HIER_CTX, ovstage_hierarchy_computation_model_id_t, ovstage_ordinal_t, ovstage_ordinal_t),
        ),
    ]


class ovstage_population_vtable_t(ctypes.Structure):
    """The population family vtable.

    The population members run from ``open_usd_from_file`` to ``wait_op``, the
    functions of ``ovstage_population.h``. The export members follow, the
    functions of ``ovstage_population_export.h``. The member of an export
    function is its name without the prefix ``ovstage_population_export_``.
    The export wait is the exception: its member is ``export_wait_op``,
    because ``wait_op`` is the population wait. Every member takes the
    population context first. A population operation and an export operation
    share one operation stream: either wait fences the operations of the other.
    """

    _fields_ = [
        (
            "open_usd_from_file",
            _CFT(_POP_ENQ, _POP_CTX, ovx_string_t, ovstage_ordinal_t, ctypes.c_double, ctypes.c_uint32),
        ),
        (
            "open_usd_from_string",
            _CFT(_POP_ENQ, _POP_CTX, ovx_string_t, ovstage_ordinal_t, ctypes.c_double, ctypes.c_uint32),
        ),
        (
            "open_usd_from_file_with_desc",
            _CFT(_POP_ENQ, _POP_CTX, ovx_string_t, ovstage_ordinal_t, ctypes.c_double, _POP_DESC_P, ctypes.c_size_t),
        ),
        (
            "open_usd_from_string_with_desc",
            _CFT(_POP_ENQ, _POP_CTX, ovx_string_t, ovstage_ordinal_t, ctypes.c_double, _POP_DESC_P, ctypes.c_size_t),
        ),
        (
            "repopulate_usd_with_desc",
            _CFT(_POP_ENQ, _POP_CTX, ovstage_ordinal_t, ctypes.c_double, _POP_DESC_P, ctypes.c_size_t),
        ),
        (
            "add_usd_reference_from_file",
            _CFT(
                _POP_ENQ,
                _POP_CTX,
                ovx_string_t,
                ovx_string_t,
                ctypes.POINTER(ovstage_population_usd_reference_handle_t),
            ),
        ),
        (
            "add_usd_reference_from_string",
            _CFT(
                _POP_ENQ,
                _POP_CTX,
                ovx_string_t,
                ovx_string_t,
                ctypes.POINTER(ovstage_population_usd_reference_handle_t),
            ),
        ),
        ("remove_usd_reference", _CFT(_POP_ENQ, _POP_CTX, ovstage_population_usd_reference_handle_t)),
        ("reset_usd", _CFT(_POP_ENQ, _POP_CTX)),
        ("apply_usd_time", _CFT(_POP_ENQ, _POP_CTX, ovstage_ordinal_t, ctypes.c_double)),
        ("apply_usd_changes", _CFT(_POP_ENQ, _POP_CTX, ovstage_ordinal_t)),
        (
            "wait_op",
            _CFT(
                _ERR,
                _POP_CTX,
                ovstage_population_op_id_t,
                ovstage_timeout_ns_t,
                ctypes.POINTER(ovstage_population_op_wait_result_t),
            ),
        ),
        # The export functions of ovstage_population_export.h.
        (
            "destination_create",
            _CFT(_ERR, _POP_CTX, ovx_string_t, ctypes.c_int, ctypes.POINTER(_EXPORT_DEST)),
        ),
        (
            "destination_enqueue_create",
            _CFT(_POP_ENQ, _POP_CTX, ovx_string_t, ctypes.c_int, ctypes.POINTER(_EXPORT_DEST)),
        ),
        ("to_destination", _CFT(_ERR, _POP_CTX, _EXPORT_DEST, _EXPORT_DESC_P, _EXPORT_REPORT_P)),
        ("enqueue_to_destination", _CFT(_POP_ENQ, _POP_CTX, _EXPORT_DEST, _EXPORT_DESC_P)),
        ("destination_save", _CFT(_ERR, _POP_CTX, _EXPORT_DEST)),
        ("destination_enqueue_save", _CFT(_POP_ENQ, _POP_CTX, _EXPORT_DEST)),
        ("destination_destroy", _CFT(_ERR, _POP_CTX, _EXPORT_DEST)),
        ("destination_enqueue_destroy", _CFT(_POP_ENQ, _POP_CTX, _EXPORT_DEST)),
        ("to_usd_file", _CFT(_ERR, _POP_CTX, ovx_string_t, _EXPORT_DESC_P, _EXPORT_REPORT_P)),
        ("enqueue_to_usd_file", _CFT(_POP_ENQ, _POP_CTX, ovx_string_t, _EXPORT_DESC_P)),
        ("to_usd_layer_identifier", _CFT(_ERR, _POP_CTX, ovx_string_t, _EXPORT_DESC_P, _EXPORT_REPORT_P)),
        ("to_usd_stage_identifier", _CFT(_ERR, _POP_CTX, ovx_string_t, _EXPORT_DESC_P, _EXPORT_REPORT_P)),
        ("enqueue_to_usd_layer_identifier", _CFT(_POP_ENQ, _POP_CTX, ovx_string_t, _EXPORT_DESC_P)),
        ("enqueue_to_usd_stage_identifier", _CFT(_POP_ENQ, _POP_CTX, ovx_string_t, _EXPORT_DESC_P)),
        (
            "export_wait_op",
            _CFT(_ERR, _POP_CTX, ovstage_population_op_id_t, ovstage_timeout_ns_t, _EXPORT_REPORT_P),
        ),
    ]


class ovstage_usd_vtable_t(ctypes.Structure):
    """The usd family vtable: ``ovstage_get_usd_stage_id``, a TEMPORARY accessor."""

    _fields_ = [
        ("get_usd_stage_id", _CFT(_ERR, _USD_CTX, ctypes.POINTER(ctypes.c_uint64))),
    ]


class ovstage_instancing_vtable_t(ctypes.Structure):
    """The instancing family vtable: the ``ovstage_instancing_*`` queries."""

    _fields_ = [
        ("get_instance_roots", _CFT(_ERR, _INSTANCING_CTX, ovx_primpath_t, ctypes.POINTER(ovx_primpath_list_t))),
        ("get_prototype_root", _CFT(_ERR, _INSTANCING_CTX, ovx_primpath_t, ctypes.POINTER(ovx_primpath_t))),
        ("get_prototype_roots", _CFT(_ERR, _INSTANCING_CTX, ctypes.POINTER(ovx_primpath_list_t))),
    ]


# ── Instance (ovstage_api/ovstage_api_types.h) ───────────────────────────────
# Every instance of the API is a vtable with its context. The data-plane
# instance holds the public vtable. An instance of a family holds its family
# vtable. The factory of an implementation fills every instance before the
# ovstage_instance_t is public. A family the implementation does not serve
# gets a vtable of stubs and a NULL context. The member order must match
# ovstage_api_types.h exactly.


class ovstage_data_plane_instance_t(ctypes.Structure):
    """The data-plane instance: ``{const ovstage_data_plane_vtable_t* vtable; ovstage_data_plane_context_t* context;}``."""

    _fields_ = [
        ("vtable", ovstage_vtable_p),
        ("context", ovstage_data_plane_context_p),
    ]


class ovstage_lifecycle_instance_t(ctypes.Structure):
    """The lifecycle instance: the lifecycle family vtable and its context."""

    _fields_ = [
        ("vtable", ctypes.POINTER(ovstage_lifecycle_vtable_t)),
        ("context", ovstage_lifecycle_context_p),
    ]


class ovstage_hierarchy_instance_t(ctypes.Structure):
    """The hierarchy instance: the hierarchy family vtable and its context."""

    _fields_ = [
        ("vtable", ctypes.POINTER(ovstage_hierarchy_vtable_t)),
        ("context", ovstage_hierarchy_context_p),
    ]


class ovstage_population_instance_t(ctypes.Structure):
    """The population instance: the population family vtable and its context."""

    _fields_ = [
        ("vtable", ctypes.POINTER(ovstage_population_vtable_t)),
        ("context", ovstage_population_context_p),
    ]


class ovstage_usd_instance_t(ctypes.Structure):
    """The usd instance: the usd family vtable and its context."""

    _fields_ = [
        ("vtable", ctypes.POINTER(ovstage_usd_vtable_t)),
        ("context", ovstage_usd_context_p),
    ]


class ovstage_instancing_instance_t(ctypes.Structure):
    """The instancing instance: the instancing family vtable and its context."""

    _fields_ = [
        ("vtable", ctypes.POINTER(ovstage_instancing_vtable_t)),
        ("context", ovstage_instancing_context_p),
    ]


class ovstage_ext_instances_t(ctypes.Structure):
    """``ovstage_ext_instances_t``: the extension instances of an instance, one per family, by value."""

    _fields_ = [
        ("lifecycle", ovstage_lifecycle_instance_t),
        ("hierarchy", ovstage_hierarchy_instance_t),
        ("population", ovstage_population_instance_t),
        ("usd", ovstage_usd_instance_t),
        ("instancing", ovstage_instancing_instance_t),
    ]


# Real layout for the ``ovstage_instance_t`` struct declared field-less above.
# The data-plane slots are reached through ``.data_plane.vtable`` with
# ``.data_plane.context`` as their first argument. The functions of the
# families are reached through ``.ext_instances.<family>.vtable`` with
# ``.ext_instances.<family>.context`` as their first argument.
ovstage_instance_t._fields_ = [
    ("data_plane", ovstage_data_plane_instance_t),
    ("ext_instances", ovstage_ext_instances_t),
]

# Map the historical flat data-plane symbol names to their vtable slot, so
# _InstanceApi can be driven by the same ``ovstage_<slot>`` names the call sites
# already use.
_VTABLE_SLOT_BY_SYMBOL = {"ovstage_" + name: name for (name, _ctype) in ovstage_data_plane_vtable_t._fields_}
# The placed operations keep their ovstage_* names but resolve through the
# placement extension rather than a vtable slot; see _InstanceApi.
_PLACEMENT_ENTRY_BY_SYMBOL = {
    "ovstage_" + name: name for (name, _ctype) in ovstage_placement_extension_v1_t._fields_ if name != "header"
}
# Map the historical name of each wrapper of the families to its family and
# its member of the family vtable, so _InstanceApi serves the same
# ``ovstage_*`` names the call sites already use. Written out in full, in the
# order of the family headers. The lifecycle functions ovstage_get_config and
# ovstage_destroy_instance are exported C functions, bound by name below.
EXT_INSTANCE_MEMBER_BY_SYMBOL = {
    # hierarchy (ovstage.h)
    "ovstage_get_hierarchy": ("hierarchy", "get_hierarchy"),
    "ovstage_fetch_hierarchy_result": ("hierarchy", "fetch_hierarchy_result"),
    "ovstage_release_hierarchy_result": ("hierarchy", "release_hierarchy_result"),
    "ovstage_release_hierarchy": ("hierarchy", "release_hierarchy"),
    "ovstage_get_hierarchy_computation_models": ("hierarchy", "get_hierarchy_computation_models"),
    "ovstage_compute_hierarchy": ("hierarchy", "compute_hierarchy"),
    # population (ovstage_population.h)
    "ovstage_population_open_usd_from_file": ("population", "open_usd_from_file"),
    "ovstage_population_open_usd_from_string": ("population", "open_usd_from_string"),
    "ovstage_population_open_usd_from_file_with_desc": ("population", "open_usd_from_file_with_desc"),
    "ovstage_population_open_usd_from_string_with_desc": ("population", "open_usd_from_string_with_desc"),
    "ovstage_population_repopulate_usd_with_desc": ("population", "repopulate_usd_with_desc"),
    "ovstage_population_add_usd_reference_from_file": ("population", "add_usd_reference_from_file"),
    "ovstage_population_add_usd_reference_from_string": ("population", "add_usd_reference_from_string"),
    "ovstage_population_remove_usd_reference": ("population", "remove_usd_reference"),
    "ovstage_population_reset_usd": ("population", "reset_usd"),
    "ovstage_population_apply_usd_time": ("population", "apply_usd_time"),
    "ovstage_population_apply_usd_changes": ("population", "apply_usd_changes"),
    "ovstage_population_wait_op": ("population", "wait_op"),
    # the export functions of the population family (ovstage_population_export.h)
    "ovstage_population_export_destination_create": ("population", "destination_create"),
    "ovstage_population_export_destination_enqueue_create": ("population", "destination_enqueue_create"),
    "ovstage_population_export_to_destination": ("population", "to_destination"),
    "ovstage_population_export_enqueue_to_destination": ("population", "enqueue_to_destination"),
    "ovstage_population_export_destination_save": ("population", "destination_save"),
    "ovstage_population_export_destination_enqueue_save": ("population", "destination_enqueue_save"),
    "ovstage_population_export_destination_destroy": ("population", "destination_destroy"),
    "ovstage_population_export_destination_enqueue_destroy": ("population", "destination_enqueue_destroy"),
    "ovstage_population_export_to_usd_file": ("population", "to_usd_file"),
    "ovstage_population_export_enqueue_to_usd_file": ("population", "enqueue_to_usd_file"),
    "ovstage_population_export_to_usd_layer_identifier": ("population", "to_usd_layer_identifier"),
    "ovstage_population_export_to_usd_stage_identifier": ("population", "to_usd_stage_identifier"),
    "ovstage_population_export_enqueue_to_usd_layer_identifier": ("population", "enqueue_to_usd_layer_identifier"),
    "ovstage_population_export_enqueue_to_usd_stage_identifier": ("population", "enqueue_to_usd_stage_identifier"),
    # The export wait: wait_op is the population wait.
    "ovstage_population_export_wait_op": ("population", "export_wait_op"),
    # usd (ovstage.h)
    "ovstage_get_usd_stage_id": ("usd", "get_usd_stage_id"),
    # instancing (ovstage_instancing.h)
    "ovstage_instancing_get_instance_roots": ("instancing", "get_instance_roots"),
    "ovstage_instancing_get_prototype_root": ("instancing", "get_prototype_root"),
    "ovstage_instancing_get_prototype_roots": ("instancing", "get_prototype_roots"),
}


def _placement_extension(instance) -> Optional[ovstage_placement_extension_v1_t]:
    """Return the instance's placement table, or ``None`` when it has none.

    Mirrors ovstage_query_placement_extension(): only exactly
    OVSTAGE_PLACEMENT_EXTENSION_VERSION is accepted, since another version is
    incompatible by definition, and a table smaller than the struct defined here
    is treated as absent, since its trailing fields would be read past its end.
    """
    table = ctypes.c_void_p()
    status = instance.data_plane.vtable.contents.query_extension(
        instance.data_plane.context, OVSTAGE_PLACEMENT_EXTENSION_NAME, ctypes.byref(table)
    )
    if status != OVSTAGE_OK or not table.value:
        return None
    ext = ctypes.cast(table, ctypes.POINTER(ovstage_placement_extension_v1_t)).contents
    if ext.header.version != OVSTAGE_PLACEMENT_EXTENSION_VERSION or ext.header.struct_size < ctypes.sizeof(
        ovstage_placement_extension_v1_t
    ):
        return None
    return ext


class _InstanceApi:
    """Facade that serves the ``ovstage_*`` functions of an instance by name.

    Every data-plane slot and every function of the families takes one of two
    receivers as its first argument. ``functools.singledispatch`` selects the
    body by the type of the receiver, the run-time form of the C++ overloads
    of the public headers. A NULL instance and a NULL pair raise the invalid
    instance error. Any other receiver type raises ``TypeError``. The facade
    builds the function of a name once and keeps it as an attribute.

    The first receiver is the ``ovstage_instance_t*`` pointer. A data-plane
    slot resolves ``instance->data_plane.vtable->slot`` and forwards
    ``instance->data_plane.context`` as the C context argument. A function of
    the families resolves its member of
    ``instance->ext_instances.<family>.vtable`` and forwards the context of
    its pair, ``instance->ext_instances.<family>.context``, as the ``static
    inline`` wrapper of its family header does.

    The second receiver is the pointer to the pair itself, as the ``_ex`` form
    of the wrapper takes it: ``ovstage_data_plane_instance_t*`` for a slot,
    ``ovstage_<family>_instance_t*`` for a function of a family. The call reads
    the member of ``pair->vtable`` and forwards ``pair->context``. The instance
    receiver calls the pair receiver.

    A placed operation takes the instance pointer and resolves through the
    placement extension. Any other attribute, such as the thread-local error
    accessors, is delegated unchanged to the underlying CDLL (still flat
    exports).
    """

    def __init__(self, lib: ctypes.CDLL, invalid_instance_error=None):
        self._lib = lib
        self._invalid_instance_error = invalid_instance_error

    def _reject_null_instance(self, name: str, instance_ptr) -> None:
        if not instance_ptr:
            if self._invalid_instance_error is not None:
                raise self._invalid_instance_error(name)
            raise RuntimeError(f"{name} called with a null ovstage instance")

    def _reject_null_pair(self, name: str, pair_ptr) -> None:
        """A NULL pair pointer raises the invalid instance error, as a NULL instance pointer does."""
        if not pair_ptr:
            if self._invalid_instance_error is not None:
                raise self._invalid_instance_error(name)
            raise RuntimeError(f"{name} called with a null ovstage instance")

    def __getattr__(self, name: str):
        slot = _VTABLE_SLOT_BY_SYMBOL.get(name)
        if slot is None:
            family_member = EXT_INSTANCE_MEMBER_BY_SYMBOL.get(name)
            if family_member is not None:
                call = self._ext_instances_call(name, *family_member)
                self.__dict__[name] = call
                return call
            entry = _PLACEMENT_ENTRY_BY_SYMBOL.get(name)
            if entry is not None:
                return self._placement_dispatch(name, entry)
            return getattr(self._lib, name)  # flat symbol

        # A data-plane slot takes the instance pointer or the pointer to the
        # data-plane instance. The instance receiver calls the pair receiver.
        pair_type = ovstage_data_plane_instance_t

        @functools.singledispatch
        def _dispatch(receiver, *args):
            raise TypeError(
                f"{name} takes ovstage_instance_p or POINTER({pair_type.__name__}) "
                f"as its first argument, not {type(receiver).__name__}"
            )

        @_dispatch.register(ovstage_instance_p)
        def _(instance_ptr, *args):
            self._reject_null_instance(name, instance_ptr)
            instance = instance_ptr.contents
            return _dispatch(ctypes.pointer(instance.data_plane), *args)

        @_dispatch.register(ctypes.POINTER(pair_type))
        def _(pair_ptr, *args):
            self._reject_null_pair(name, pair_ptr)
            pair = pair_ptr.contents
            return getattr(pair.vtable.contents, slot)(pair.context, *args)

        _dispatch.__name__ = name
        self.__dict__[name] = _dispatch
        return _dispatch

    def _ext_instances_call(self, name: str, family: str, member: str):
        """Serve a function of the families through the pair of its family.

        The receiver is the instance pointer or the pointer to the pair of the
        family, ``ovstage_<family>_instance_t*``. The instance receiver takes
        ``&instance->ext_instances.<family>`` and calls the pair receiver, as
        the ``static inline`` wrapper of the family header calls its ``_ex``
        form. The pair receiver reads ``pair->vtable-><member>`` and passes
        ``pair->context`` as the first argument. A member of a family receives
        the context of its pair, as a data-plane slot receives the data-plane
        context. A NULL instance and a NULL pair raise the invalid instance
        error, as the data-plane slots do. The C wrapper answers a NULL
        receiver with its documented status instead, but a Stage never passes
        one. Any other receiver type raises ``TypeError``.
        """
        pair_type = dict(ovstage_ext_instances_t._fields_)[family]

        @functools.singledispatch
        def _call(receiver, *args):
            raise TypeError(
                f"{name} takes ovstage_instance_p or POINTER({pair_type.__name__}) "
                f"as its first argument, not {type(receiver).__name__}"
            )

        @_call.register(ovstage_instance_p)
        def _(instance_ptr, *args):
            self._reject_null_instance(name, instance_ptr)
            instance = instance_ptr.contents
            return _call(ctypes.pointer(getattr(instance.ext_instances, family)), *args)

        @_call.register(ctypes.POINTER(pair_type))
        def _(pair_ptr, *args):
            self._reject_null_pair(name, pair_ptr)
            pair = pair_ptr.contents
            return getattr(pair.vtable.contents, member)(pair.context, *args)

        _call.__name__ = name
        return _call

    def _placement_dispatch(self, name: str, entry: str):
        """Dispatch a placed operation through the instance's placement extension.

        Like the C wrappers, an instance without the extension gets a rejected
        enqueue (NOT_FOUND, invalid op id) rather than a call through a missing
        function, so the caller's usual status handling reports it.
        """

        def _dispatch(instance_ptr, *args):
            self._reject_null_instance(name, instance_ptr)
            instance = instance_ptr.contents
            ext = _placement_extension(instance)
            if ext is None:
                rejected = ovstage_enqueue_result_t()
                rejected.status = OVSTAGE_ERROR_NOT_FOUND
                rejected.op_index = OVSTAGE_INVALID_OP_ID
                return rejected
            return getattr(ext, entry)(instance.data_plane.context, *args)

        _dispatch.__name__ = name
        return _dispatch


def instance_api(lib: ctypes.CDLL, invalid_instance_error=None) -> "_InstanceApi":
    """Return a facade that serves the data-plane slots and the functions of the families by name."""
    return _InstanceApi(lib, invalid_instance_error)


# ── Library discovery / loading (mirrors ovrtx's _LibraryLoader) ────────────
OVSTAGE_LIBRARY_PATH_HINT: Optional[str] = None

if sys.platform.startswith("win"):
    OVSTAGE_LIB_NAME = "ovstage-dynamic.dll"
elif sys.platform == "darwin":
    OVSTAGE_LIB_NAME = "libovstage-dynamic.dylib"
else:
    OVSTAGE_LIB_NAME = "libovstage-dynamic.so"


def _resolve_existing_dirs(paths: Iterable[Path]) -> List[Path]:
    out: List[Path] = []
    for candidate in paths:
        try:
            if candidate.exists() and candidate.is_dir():
                out.append(candidate.resolve())
        except OSError:
            continue
    return out


def ovstage_loader_candidate_dirs() -> List[Path]:
    """Ordered candidate directories for ``OVSTAGE_LIB_NAME`` (highest first).

    1. ``<package>/bin``          — wheel layout.
    2. ``LD_LIBRARY_PATH`` (Linux) — the ``repo test`` suite points this at the build tree.
    3. ``PATH``.
    4. ``<package>/../../bin``     — in-tree dev fallback.

    The current working directory is intentionally **not** searched by default:
    loading a native library from a CWD an attacker can influence is a DLL/SO
    hijacking vector (arbitrary native code on import/load). Opt in explicitly by
    setting ``OVSTAGE_ALLOW_CWD_LIBRARY_SEARCH=1`` for example/test layouts that
    keep the library next to the script.
    """
    package_dir = Path(__file__).parent.parent
    candidates: List[Path] = [package_dir / "bin"]
    if os.environ.get("OVSTAGE_ALLOW_CWD_LIBRARY_SEARCH", "") not in ("", "0"):
        try:
            candidates.append(Path.cwd())
        except OSError:
            pass
    if not sys.platform.startswith("win"):
        if ld_paths := os.environ.get("LD_LIBRARY_PATH", ""):
            candidates.extend(Path(p) for p in ld_paths.split(os.pathsep) if p)
    if path_paths := os.environ.get("PATH", ""):
        candidates.extend(Path(p) for p in path_paths.split(os.pathsep) if p)
    candidates.append(package_dir.parent.parent / "bin")
    return candidates


class _LibraryLoader:
    """Lazily loads and configures the singleton ovstage shared loader."""

    def __init__(self):
        self._lib: Optional[ctypes.CDLL] = None
        self._version: Optional[tuple] = None
        self._build_info: Optional[BuildInfo] = None
        self._dll_dir_cookies: list = []  # keep os.add_dll_directory handles alive (Windows)

    @property
    def is_loaded(self) -> bool:
        return self._lib is not None

    @property
    def version(self) -> Optional[tuple]:
        return self._version

    def load(self) -> ctypes.CDLL:
        lib = self.open_loader()
        if self._version is None:
            version = _probe_version(lib)
            if self._build_info is not None:
                _validate_build_info_version(self._build_info, version)
            self._version = version
        return lib

    def open_loader(self) -> ctypes.CDLL:
        """Open only the public loader, without initializing the runtime."""
        if self._lib is None:
            lib = self._open()
            _configure_prototypes(lib)
            build_info = _read_build_info(lib) if hasattr(lib, "ovstage_get_build_info") else None
            self._lib = lib
            self._build_info = build_info
        return self._lib

    def get_build_info(self) -> BuildInfo:
        lib = self.open_loader()
        _require(lib, "ovstage_get_build_info")
        if self._build_info is None:
            raise RuntimeError("ovstage loader did not provide build information")
        return self._build_info

    def _open(self) -> ctypes.CDLL:
        candidates = ovstage_loader_candidate_dirs()
        # The module global is an explicit in-process override; fall back to the
        # OVSTAGE_LIBRARY_PATH_HINT env var the "failed to load" message advertises
        # (consistent with PATH / LD_LIBRARY_PATH / OVSTAGE_ALLOW_CWD_LIBRARY_SEARCH).
        hint = OVSTAGE_LIBRARY_PATH_HINT or os.environ.get("OVSTAGE_LIBRARY_PATH_HINT")
        if hint:
            candidates.insert(0, Path(hint))
        search_dirs = _resolve_existing_dirs(candidates)
        lib_paths = [d / OVSTAGE_LIB_NAME for d in search_dirs]
        last_error = None
        for lib_path in lib_paths:
            if lib_path.exists() and lib_path.is_file():
                # On Windows (Python 3.8+) a DLL loaded by full path resolves its
                # own dependencies from the registered DLL directories, not PATH.
                # The ovstage runtime dependencies sit alongside the loader and in a
                # sibling ``plugins`` dir, so register both before loading.
                self._add_dll_directories(lib_path.parent)
                try:
                    return ctypes.CDLL(str(lib_path))
                except Exception as exc:  # noqa: BLE001
                    last_error = exc
                    continue
        paths_str = "\n  ".join(str(p) for p in lib_paths) or "<no candidate directories>"
        msg = (
            f"Failed to load {OVSTAGE_LIB_NAME}. Set OVSTAGE_LIBRARY_PATH_HINT or add the build "
            f"output directory to LD_LIBRARY_PATH. Tried:\n  {paths_str}"
        )
        if last_error:
            msg += f"\nLast error: {last_error}"
        raise RuntimeError(msg)

    def _add_dll_directories(self, lib_dir: Path) -> None:
        """Register the DLL's directory (and its ``plugins`` subtree) for
        dependency resolution on Windows. No-op on other platforms (which use
        rpath/PATH).

        Each optional plugin ships in its own ``plugins/<name>`` directory, and a
        plugin's own bundled runtime dependencies sit in a *sibling* directory
        (e.g. a plugin in ``plugins/foo/`` depends on ``plugins/foo.lib/foo.dll``).
        Windows' loader only searches directories that have been explicitly
        registered — it does not walk sibling plugin dirs — so registering just
        ``plugins`` leaves those bundled deps unresolvable and the plugin fails to
        load. Register every immediate ``plugins/*`` subdirectory as well so each
        plugin can find its own dependencies, without hardcoding individual plugin
        names."""
        if not sys.platform.startswith("win"):
            return
        plugins_dir = lib_dir / "plugins"
        dirs = [lib_dir, plugins_dir]
        try:
            if plugins_dir.is_dir():
                dirs.extend(sorted(p for p in plugins_dir.iterdir() if p.is_dir()))
        except OSError:
            pass
        for d in dirs:
            try:
                if d.is_dir():
                    self._dll_dir_cookies.append(os.add_dll_directory(str(d)))
            except OSError:
                continue


def _configure_prototypes(lib: ctypes.CDLL) -> None:
    """Set argtypes/restype for the remaining flat exports.

    The process functions, ``ovstage_create_instance``, the thread-local error
    accessors and ``ovstage_population_register_usd_schemas`` are flat
    symbols. The data plane, the path
    dictionary and the functions of the families are reached through vtables
    (see ``ovstage_data_plane_vtable_t``, ``path_dictionary_vtable_t``, the family
    vtables and :class:`_InstanceApi`). Their CFUNCTYPE declarations carry the
    argtypes/restype for those calls. ``population_export_bindings.py`` binds
    the export descriptor initializers.
    """
    inst = ovstage_instance_p
    err = ovstage_api_status_t

    # Stage uses process lifecycle internally when a StageConfig is supplied.
    # The entry points remain absent from the top-level Python API.
    lib.ovstage_initialize.argtypes = [ctypes.POINTER(ovstage_config_t)]
    lib.ovstage_initialize.restype = err
    lib.ovstage_shutdown.argtypes = []
    lib.ovstage_shutdown.restype = err

    if hasattr(lib, "ovstage_get_build_info"):
        lib.ovstage_get_build_info.argtypes = []
        lib.ovstage_get_build_info.restype = ctypes.POINTER(_ovstage_build_info_t)

    # The lifecycle functions are exported C functions. The public vtable and the
    # extension vtables of the instance serve everything else.
    lib.ovstage_create_instance.argtypes = [ctypes.POINTER(ovstage_instance_desc_t), ctypes.POINTER(inst)]
    lib.ovstage_create_instance.restype = err
    lib.ovstage_get_config.argtypes = [inst, ctypes.POINTER(ovstage_config_t)]
    lib.ovstage_get_config.restype = err
    lib.ovstage_destroy_instance.argtypes = [inst]
    lib.ovstage_destroy_instance.restype = err

    # Thread-local last-error retrieval is a free function (no instance), so it
    # can be read even when ovstage_create_instance failed. Mirrors
    # ovstage_population_get_last_error.
    lib.ovstage_get_last_error.argtypes = []
    lib.ovstage_get_last_error.restype = ovx_string_t

    # Logging (ovstage_set_log_callback / ovstage_flush_log): process-global flat
    # exports, not tied to an instance. Guarded so an older libovstage without
    # them still loads (the wrappers raise NOT_SUPPORTED instead).
    if hasattr(lib, "ovstage_set_log_callback"):
        # severity is signed (ovstage_log_severity_t spans -2..3); channel_filter
        # is a nullable ovx_string_t pointer; callback is nullable to disable.
        lib.ovstage_set_log_callback.argtypes = [
            ctypes.c_int,
            ctypes.POINTER(ovx_string_t),
            _log_callback_t,
            ctypes.c_void_p,
        ]
        lib.ovstage_set_log_callback.restype = err
        lib.ovstage_flush_log.argtypes = [ovstage_timeout_ns_t]
        lib.ovstage_flush_log.restype = err

    # USD schema registration (ovstage_population_register_usd_schemas): another process-global
    # flat export. Guarded the same way, so an older libovstage without it still
    # loads and the wrapper raises NOT_SUPPORTED.
    if hasattr(lib, "ovstage_population_register_usd_schemas"):
        lib.ovstage_population_register_usd_schemas.argtypes = [
            ctypes.POINTER(ovx_string_t),
            ctypes.c_size_t,
        ]
        lib.ovstage_population_register_usd_schemas.restype = err

    # The path dictionary is reached through the ``get_path_dictionary`` vtable
    # slot (see ``ovstage_data_plane_vtable_t``) rather than flat exports — nothing to
    # configure here. The instancing queries are members of the instancing
    # family vtable (see ``ovstage_instancing_vtable_t``).

    if hasattr(lib, "ovstage_instancing_resolve_paths"):
        lib.ovstage_instancing_resolve_paths.argtypes = [
            inst, ctypes.POINTER(ovx_primpath_t), ctypes.c_size_t,
            ctypes.POINTER(ovstage_instancing_path_resolution_t),
        ]
        lib.ovstage_instancing_resolve_paths.restype = err
    if hasattr(lib, "ovstage_instancing_get_instance_roots_batch"):
        lib.ovstage_instancing_get_instance_roots_batch.argtypes = [
            inst, ctypes.POINTER(ovx_primpath_t), ctypes.c_size_t,
            ctypes.POINTER(ovx_primpath_list_t),
        ]
        lib.ovstage_instancing_get_instance_roots_batch.restype = err

    # ── ovstage_population error accessors ─────────────────────────────────────
    # The population functions that take a stage are members of the population
    # family vtable (see ``ovstage_population_vtable_t``). The thread-local error
    # accessors take no stage and stay flat exports. Guarded so a libovstage
    # without the population bridge still loads.
    if hasattr(lib, "ovstage_population_get_last_error"):
        lib.ovstage_population_get_last_error.argtypes = []
        lib.ovstage_population_get_last_error.restype = ovx_string_t
        lib.ovstage_population_get_last_op_error.argtypes = [ovstage_population_op_id_t]
        lib.ovstage_population_get_last_op_error.restype = ovx_string_t


def _probe_version(lib: ctypes.CDLL) -> tuple:
    """Read the implementation version via a throwaway instance's vtable.

    ``get_version`` is a vtable slot (it needs a context), so unlike the old
    flat ABI there is no instance-free way to read the version. Creating and
    destroying a probe instance here doubles as a smoke test that the library
    loads, the layout of the ``ovstage_instance_t`` struct matches, and both
    vtables work: ``get_version`` is a slot of the public vtable, and the
    exported ``ovstage_destroy_instance`` calls the lifecycle member.
    """
    desc = ovstage_instance_desc_t()
    desc.name = b"ovstage-python-version-probe"
    inst = ovstage_instance_p()
    code = lib.ovstage_create_instance(ctypes.byref(desc), ctypes.byref(inst))
    if code != OVSTAGE_OK:
        raise RuntimeError(f"ovstage_create_instance failed during version probe (code {int(code)})")
    try:
        instance = inst.contents
        major, minor, patch = ctypes.c_uint32(), ctypes.c_uint32(), ctypes.c_uint32()
        instance.data_plane.vtable.contents.get_version(
            instance.data_plane.context, ctypes.byref(major), ctypes.byref(minor), ctypes.byref(patch)
        )
        return (major.value, minor.value, patch.value)
    finally:
        lib.ovstage_destroy_instance(inst)


_loader = _LibraryLoader()


def load() -> ctypes.CDLL:
    """Return the configured singleton ovstage loader (loading on first use)."""
    return _loader.load()


def library_version() -> Optional[tuple]:
    """Return the loaded library's ``(major, minor, patch)`` version.

    Forces the lazy load so a standalone ``ovstage.library_version()`` returns a real
    version (or raises if the library can't load) instead of ``None`` before first use.
    """
    _loader.load()
    return _loader.version


def get_build_info() -> BuildInfo:
    """Return build information from the native OVStage loader.

    The loader library is opened on first use, but the OVStage runtime is not
    initialized and no Stage instance is created.
    """
    return _loader.get_build_info()


# ── Logging (ovstage_set_log_callback / ovstage_flush_log) ───────────────────
# Process-global free functions, so they live at module scope rather than on
# Stage. The runtime is bootstrapped on demand by creating a Stage; there is no
# public initialize()/shutdown(). Errors surface as OvstageError (imported
# lazily to avoid a bindings <- types import cycle at module load).


def _require(lib: ctypes.CDLL, name: str) -> None:
    if not hasattr(lib, name):
        from .types import OvstageError

        raise OvstageError(OVSTAGE_ERROR_NOT_SUPPORTED, f"libovstage does not export {name}")


def _check_process_status(code: int, name: str) -> None:
    if code != OVSTAGE_OK:
        from .types import OvstageError

        detail = str(_loader.load().ovstage_get_last_error())
        raise OvstageError(code, detail or name)


# The active trampoline and the user's callable are held here so ctypes does not
# garbage-collect them while the C dispatcher thread may still invoke them — a
# freed trampoline would be a use-after-free on the next message.
#
# On *replace* the C dispatcher can still be mid-invocation on the previous
# trampoline: it snapshots the callback pointer under a lock and then calls it
# *unlocked* (see LogDispatcher::dispatchMessage), so a message dequeued just
# before the swap may fire the old pointer after set_log_callback() returns.
# Freeing the old trampoline then would crash. We therefore retire previous
# trampolines into a keep-alive list instead of dropping them. flush_log()
# proves no pre-flush message is still in flight, so it clears the retired list.
_active_log_trampoline = None
_active_log_user_callback = None
_retired_log_trampolines: list = []


def set_log_callback(callback, severity=None, channel_filter: Optional[str] = None) -> None:
    """Install (or clear) a process-global log callback.

    Routes ovstage's log messages — and messages from its USD support layer —
    to ``callback(severity, timestamp, message)``, where ``severity`` is a
    :class:`~ovstage.LogSeverity`, ``timestamp`` is wall-clock seconds since the
    epoch, and ``message`` is an owned ``str`` (a decoded copy that remains valid
    after the call returns).

    Delivery is asynchronous on a dedicated dispatcher thread (created lazily on
    the first callback), so the callback never runs on the logging hot path and
    invocations are serialized. Use :func:`flush_log` to force pending messages
    through before a checkpoint.

    Requires the runtime to be bootstrapped — hold a live :class:`~ovstage.Stage`
    when calling this.

    .. note::
       An exception raised by ``callback`` has its traceback printed to stderr
       (so it is visible for debugging) and is then suppressed — it never
       unwinds into the C dispatcher. Do **not** emit ovstage/USD log messages
       from inside ``callback``: they re-enter the dispatcher and can feed back
       indefinitely.

    :param callback: ``f(severity, timestamp, message)`` callable, or ``None`` to
        flush pending messages and disable delivery.
    :param severity: default :class:`~ovstage.LogSeverity` threshold for channels
        not matched by ``channel_filter``; messages below it are dropped. Defaults
        to :attr:`~ovstage.LogSeverity.WARNING`.
    :param channel_filter: optional comma-separated ``<channel>=<level>`` list
        (e.g. ``"omni.ovstage=verbose"``); ``None`` applies ``severity``
        uniformly. Levels: verbose|debug|info|warn|warning|error|fatal|none.
    :raises OvstageError: ``INVALID_ARGUMENT`` if the filter fails to parse, or
        ``OP_FAILED`` if the runtime is not bootstrapped.
    """
    global _active_log_trampoline, _active_log_user_callback
    from .types import LogSeverity, check_enum_member

    if severity is None:
        severity = LogSeverity.WARNING

    lib = _loader.load()
    _require(lib, "ovstage_set_log_callback")

    filter_ptr = ctypes.byref(ovx_string_t(channel_filter)) if channel_filter else None

    if callback is None:
        _check_process_status(
            lib.ovstage_set_log_callback(
                check_enum_member(severity, LogSeverity, "severity"), filter_ptr, _log_callback_t(), None
            ),
            "ovstage_set_log_callback",
        )
        # The disable call flushes and tears the dispatcher thread down before
        # returning, so neither the active nor any retired trampoline can still
        # be invoked — release them all.
        _active_log_trampoline = None
        _active_log_user_callback = None
        _retired_log_trampolines.clear()
        return

    def _trampoline(sev, timestamp, message, _user_data):
        try:
            callback(LogSeverity(sev), float(timestamp), str(message))
        except Exception:
            # Surface the failure to stderr for debugging, but never let the
            # exception unwind into the C dispatcher thread.
            import traceback

            traceback.print_exc()

    trampoline = _log_callback_t(_trampoline)
    _check_process_status(
        lib.ovstage_set_log_callback(
            check_enum_member(severity, LogSeverity, "severity"), filter_ptr, trampoline, None
        ),
        "ovstage_set_log_callback",
    )
    # Replace the active callback only after installation succeeds. Retire (don't
    # free) the previous trampoline: the dispatcher may still fire it for a message
    # dequeued before the swap. flush_log() later clears the retired list.
    if _active_log_trampoline is not None:
        _retired_log_trampolines.append(_active_log_trampoline)
    _active_log_trampoline = trampoline
    _active_log_user_callback = callback


def flush_log(timeout: int = OVSTAGE_TIMEOUT_INFINITE) -> bool:
    """Block until log messages emitted before this call have been delivered.

    Point-in-time barrier: messages produced concurrently with or after the call
    are not guaranteed to be included. Returns immediately if no callback is
    installed (nothing is buffered).

    .. warning::
       With the default ``OVSTAGE_TIMEOUT_INFINITE`` this blocks until the
       dispatcher drains; a stuck callback makes it hang. Pass a finite
       ``timeout`` if the callback might block.

    :param timeout: max nanoseconds to wait; ``OVSTAGE_TIMEOUT_INFINITE`` blocks,
        ``0`` polls.
    :returns: ``True`` if drained (or no callback installed); ``False`` if not
        drained within ``timeout``.
    :raises TypeError: if ``timeout`` is not an integer (e.g. ``None``).
    :raises ValueError: if ``timeout`` is negative or does not fit in uint64.
    :raises OvstageError: on any error other than a timeout.
    """
    timeout = check_timeout(timeout)
    lib = _loader.load()
    _require(lib, "ovstage_flush_log")
    code = lib.ovstage_flush_log(timeout)
    if code == OVSTAGE_ERROR_TIMEOUT:
        return False
    _check_process_status(code, "ovstage_flush_log")
    # A completed flush proves every message enqueued before it has been
    # dispatched, so no retired trampoline can be in flight any more — free them.
    if code == OVSTAGE_OK:
        _retired_log_trampolines.clear()
    return True
