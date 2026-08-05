# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Raw ctypes layer for the ovstage C ABI.

ovstage's data plane is a *vtable* contract (``ovstage_api/ovstage_api.h``):
``libovstage.so`` exports the process and instance lifecycle as flat symbols,
and ``ovstage_create_instance`` hands back an ``ovstage_instance_t`` bundle
(``{const ovstage_vtable_t* vtable; ovstage_context_t* context;}``). Every
data-plane operation (query/read/write/map/ordinal/diagnostics) is reached
through ``instance->vtable->slot(instance->context, ...)`` rather than a flat
export. The path-dictionary interning API is likewise reached through a vtable:
the instance's ``get_path_dictionary`` slot hands back a
``path_dictionary_instance_t`` bundle whose own vtable carries the token/path/
list calls. The library still exports auxiliary APIs such as
``ovstage_instancing_*`` and ``ovstage_population_*`` as flat symbols. Test
gates are discovered through the ``ovstage.test.hooks`` extension.

This module mirrors the C structs (exact field layout), the scalar typedefs,
the error/enum constants, the vtable, and binds the flat prototypes.
:class:`_InstanceApi` (via :func:`instance_api`) presents the data-plane slots
as if they were flat ``ovstage_*`` functions taking the instance bundle as the
first argument, forwarding ``instance->context`` to the resolved vtable slot.
Everything here is intentionally low-level; the Pythonic surface lives in
``stage.py``, ``path_dictionary.py``, ``instancing.py``, and ``population.py``.
"""

import ctypes
import operator
import os
import sys
from pathlib import Path
from typing import Iterable, List, Optional

from .dlpack import DLDataType, DLManagedTensorVersioned, DLTensor

# ── Scalar typedefs (all 64-bit handles / ordinals) ────────────────────────
ovstage_api_status_t = ctypes.c_uint32
ovstage_ordinal_t = ctypes.c_uint64
ovstage_op_id_t = ctypes.c_uint64
ovstage_timeout_ns_t = ctypes.c_uint64
ovstage_query_handle_t = ctypes.c_uint64
ovstage_read_handle_t = ctypes.c_uint64
ovstage_map_handle_t = ctypes.c_uint64
ovstage_ordinal_query_handle_t = ctypes.c_uint64
ovstage_hierarchy_handle_t = ctypes.c_uint64
ovstage_hierarchy_result_id_t = ctypes.c_uint64
ovstage_hierarchy_computation_model_id_t = ctypes.c_int
ovstage_hierarchy_relation_t = ctypes.c_int
ovx_token_t = ctypes.c_uint64
ovx_primpath_t = ctypes.c_uint64
ovx_primpath_list_t = ctypes.c_uint64
ovstage_population_op_id_t = ctypes.c_uint64
ovstage_population_usd_reference_handle_t = ctypes.c_uint64

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
            "timeout must be an int (nanoseconds; 0 polls, TIMEOUT_INFINITE "
            f"blocks), got {type(timeout).__name__}"
        ) from None
    if value < 0:
        raise ValueError(
            f"timeout must be non-negative, got {timeout}; pass TIMEOUT_INFINITE to block"
        )
    if value > OVSTAGE_TIMEOUT_INFINITE:
        raise ValueError(
            f"timeout must fit in uint64 (<= {OVSTAGE_TIMEOUT_INFINITE}), got {timeout}"
        )
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

OVSTAGE_INVALID_OP_ID = 0
OVSTAGE_INVALID_QUERY_HANDLE = 0
OVSTAGE_INVALID_READ_HANDLE = 0
OVSTAGE_INVALID_MAP_HANDLE = 0
OVSTAGE_INVALID_ORDINAL_QUERY_HANDLE = 0
OVSTAGE_INVALID_HIERARCHY_HANDLE = 0
OVSTAGE_INVALID_HIERARCHY_RESULT_ID = 0

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

OVSTAGE_HIERARCHY_PARENT = 0
OVSTAGE_HIERARCHY_CHILDREN = 1
OVSTAGE_HIERARCHY_SIBLINGS = 2

# ovx/types.h — path-dictionary API result status (ovx_api_status_t). The
# refactored path-dictionary API reports only success/failure; a human-readable
# detail rides along in ``ovx_api_result_t.error``.
OVX_API_SUCCESS = 0
OVX_API_ERROR = 1

OVX_INVALID_TOKEN = 0
OVX_INVALID_PRIMPATH = 0
OVX_INVALID_PRIMPATH_LIST = 0


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
        self._bytes = ctypes.create_string_buffer(encoded)
        self.ptr = ctypes.cast(self._bytes, ctypes.c_char_p)
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


# ovstage_log_callback_t: void(ovstage_log_severity_t severity, double timestamp,
# ovx_string_t message /* by value */, void* user_data). ``severity`` is a signed
# enum (c_int); ``message`` is a non-owning view valid only for the call.
_log_callback_t = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_double, ovx_string_t, ctypes.c_void_p)


class ovx_string_or_token_t(ctypes.Structure):
    """{ ovx_token_t token; ovx_string_t string; } — token wins when nonzero."""

    _fields_ = [
        ("token", ovx_token_t),
        ("string", ovx_string_t),
    ]


def make_string_or_token(value) -> ovx_string_or_token_t:
    """Build an ``ovx_string_or_token_t`` from an int token or a Python string.

    When a string is supplied, the backing buffer is kept alive on the returned
    struct (``_string_ref``) so it stays valid for the duration of the call it
    is passed to.
    """
    out = ovx_string_or_token_t()
    if isinstance(value, int):
        out.token = value
    else:
        s = ovx_string_t(str(value))
        out.token = 0
        out.string = s
        out._string_ref = s  # keepalive: s._bytes backs out.string.ptr
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
    """The ovstage instance bundle (``{vtable, context}``).

    Declared field-less here so ``ovstage_instance_p`` (and the flat-symbol
    prototypes that take it) can be defined before the vtable type exists;
    ``_fields_`` is assigned once ``ovstage_vtable_t`` is defined below.
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


# ── ovstage_population result structs (ovstage_population.h) ─────────────────
# The population USD bridge has its own async enqueue/wait model, parallel to
# but distinct from the data-plane one above: every mutating call returns an
# ``ovstage_population_enqueue_result_t`` by value and is awaited via the flat
# ``ovstage_population_wait_op`` (there is no population release_op).
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
        ("paths", ctypes.POINTER(ovx_string_or_token_t)),
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


class ovstage_population_op_wait_result_t(ctypes.Structure):
    _fields_ = [
        ("error_op_ids", ctypes.POINTER(ovstage_population_op_id_t)),
        ("error_op_id_count", ctypes.c_size_t),
        ("lowest_pending_op_id", ovstage_population_op_id_t),
    ]


class ovstage_cuda_sync_t(ctypes.Structure):
    # Mirrors C ovstage_cuda_sync_t { uintptr_t stream; uintptr_t wait_event; }.
    # stream: 0 = no sync, 1 = default stream, >1 = a specific cudaStream_t.
    # wait_event: cudaEvent_t to wait on before the op (0 = none). {0,0} = no sync.
    _fields_ = [
        ("stream", ctypes.c_size_t),
        ("wait_event", ctypes.c_size_t),
    ]


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


class ovstage_read_group_t(ctypes.Structure):
    _fields_ = [
        ("read_group_id", ctypes.c_uint64),
        ("attribute", ovx_token_t),
        ("ordinal", ovstage_ordinal_t),
        ("is_delete", ctypes.c_bool),
        ("is_array", ctypes.c_bool),         # ragged/CSR source column (vs fixed scalar)
        ("semantic", ctypes.c_int),          # ovstage_attribute_semantic_t (recovered from column metadata)
        ("prims", ovstage_prim_group_t),
        ("data", ovstage_data_t),
        ("meta", ovstage_attribute_meta_t),
    ]


class ovstage_map_group_t(ctypes.Structure):
    _fields_ = [
        ("prims", ovstage_prim_group_t),
        ("data", ovstage_data_t),
        ("meta", ovstage_attribute_meta_t),
    ]


class ovstage_predicate_t(ctypes.Structure):
    _fields_ = [
        ("attribute", ovx_string_or_token_t),
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
        ("query_result_id", ctypes.c_uint64),
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
    ]


class ovstage_attribute_write_t(ctypes.Structure):
    _fields_ = [
        ("attribute", ovx_string_or_token_t),
        ("data", ovstage_write_data_t),
    ]


class ovstage_map_desc_t(ctypes.Structure):
    _fields_ = [
        ("attribute", ovx_string_or_token_t),
        ("dtype", DLDataType),               # element type used only when creating a new column (else ignored)
        # ovstage_attribute_semantic_t (NONE=0). Records semantic metadata when
        # the map creates a column; see ovstage_write_data_t
        # for the geometric vs. ID semantic split.
        ("semantic", ctypes.c_int),
        ("prim_mode", ctypes.c_int),         # ovstage_prim_mode_t (UPSERT / INSERT)
    ]


class ovstage_instance_desc_t(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
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
    _PD_RES, _PD_CTX, ctypes.POINTER(ovx_token_t), ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_size_t, ctypes.POINTER(ovx_primpath_t),
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
    _PD_RES, _PD_CTX, ctypes.POINTER(ovx_primpath_t), ctypes.c_size_t,
    ctypes.POINTER(ovx_token_t), ctypes.c_size_t,
    ctypes.POINTER(ctypes.POINTER(ovx_token_t)), ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_size_t),
)
_FN_get_num_paths_from_path_list = ctypes.CFUNCTYPE(
    _PD_RES, _PD_CTX, ovx_primpath_list_t, ctypes.POINTER(ctypes.c_size_t)
)
_FN_get_paths_from_path_list = ctypes.CFUNCTYPE(
    _PD_RES, _PD_CTX, ovx_primpath_list_t, ctypes.c_size_t, ctypes.c_size_t,
    ctypes.POINTER(ovx_primpath_t), ctypes.POINTER(ctypes.c_size_t),
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
# context (ovstage_context_t*) as its first argument — NOT the ovstage_instance_t
# bundle. The CFUNCTYPE argtypes/restype below ARE the data-plane prototypes
# (they replace the flat ovstage_* prototypes that no longer exist as exports).
# Slot order must match ovstage_vtable_t in ovstage_api.h exactly.
ovstage_context_p = ctypes.c_void_p  # ovstage_context_t* (opaque)

_CTX = ovstage_context_p
_ENQ = ovstage_enqueue_result_t
_ERR = ovstage_api_status_t
_U32P = ctypes.POINTER(ctypes.c_uint32)
_CFT = ctypes.CFUNCTYPE


class ovstage_vtable_t(ctypes.Structure):
    _fields_ = [
        # Op tracking
        ("wait_op", _CFT(_ERR, _CTX, ovstage_op_id_t, ovstage_timeout_ns_t,
                         ctypes.POINTER(ovstage_op_wait_result_t))),
        ("release_op", _CFT(_ERR, _CTX, ovstage_op_id_t)),
        # Write-floor advance
        ("advance_write_floor", _CFT(_ENQ, _CTX, ctypes.POINTER(ovstage_write_floor_desc_t))),
        # Ordinal queries
        ("get_oldest_preserved_ordinal", _CFT(_ENQ, _CTX, ctypes.POINTER(ovstage_ordinal_query_handle_t))),
        ("get_attribute_write_floor", _CFT(_ENQ, _CTX, ovx_string_or_token_t,
                                           ctypes.POINTER(ovstage_ordinal_query_handle_t))),
        ("fetch_ordinal", _CFT(_ERR, _CTX, ovstage_ordinal_query_handle_t, ovstage_timeout_ns_t,
                               ctypes.POINTER(ovstage_ordinal_t))),
        ("release_ordinal_query", _CFT(_ENQ, _CTX, ovstage_ordinal_query_handle_t)),
        # Query
        ("query", _CFT(_ENQ, _CTX, ctypes.POINTER(ovstage_filter_t), ctypes.POINTER(ovx_token_t),
                       ctypes.c_size_t, ctypes.POINTER(ovstage_query_handle_t))),
        ("query_from_path_list", _CFT(_ERR, _CTX, ovx_primpath_list_t,
                                      ctypes.POINTER(ovstage_query_handle_t))),
        ("fetch_query_result", _CFT(_ERR, _CTX, ovstage_query_handle_t, ovstage_timeout_ns_t,
                                    ctypes.POINTER(ovstage_query_result_t))),
        ("release_query_result", _CFT(_ERR, _CTX, ctypes.POINTER(ovstage_query_result_t))),
        ("release_query", _CFT(_ENQ, _CTX, ovstage_query_handle_t)),
        # Read
        ("read_attributes", _CFT(_ENQ, _CTX, ovstage_query_handle_t, ctypes.POINTER(ovx_token_t),
                                 ctypes.c_size_t, ovstage_ordinal_range_t,
                                 ctypes.POINTER(ovstage_read_handle_t))),
        ("fetch_read_next", _CFT(_ERR, _CTX, ovstage_read_handle_t, ovstage_timeout_ns_t,
                                 ctypes.POINTER(ovstage_read_group_t))),
        ("release_group", _CFT(_ERR, _CTX, ctypes.POINTER(ovstage_read_group_t))),
        ("release_read", _CFT(_ENQ, _CTX, ovstage_read_handle_t)),
        # Write (copy-in)
        ("write_attribute", _CFT(_ENQ, _CTX, ovstage_query_handle_t, ovx_string_or_token_t,
                                 ovstage_ordinal_t, ovstage_write_data_t, ctypes.c_int)),
        # Map / unmap (zero-copy write)
        ("map_attribute", _CFT(_ENQ, _CTX, ovstage_query_handle_t, ctypes.POINTER(ovstage_map_desc_t),
                               ovstage_ordinal_t, ctypes.POINTER(ctypes.c_size_t),
                               ctypes.c_size_t, ctypes.POINTER(ovstage_map_handle_t))),
        ("fetch_map_next", _CFT(_ERR, _CTX, ovstage_map_handle_t, ovstage_timeout_ns_t,
                                ctypes.POINTER(ovstage_map_group_t))),
        ("unmap_group", _CFT(_ENQ, _CTX, ovstage_map_handle_t, ctypes.POINTER(ovstage_map_group_t),
                             ovstage_cuda_sync_t)),
        ("unmap_attribute", _CFT(_ENQ, _CTX, ovstage_map_handle_t, ovstage_cuda_sync_t)),
        # Structural
        ("delete_attributes", _CFT(_ENQ, _CTX, ovstage_query_handle_t,
                                   ctypes.POINTER(ovx_string_or_token_t), ctypes.c_size_t,
                                   ovstage_ordinal_t)),
        # Diagnostics
        ("get_version", _CFT(None, _CTX, _U32P, _U32P, _U32P)),
        ("get_error_string", _CFT(ctypes.c_char_p, _CTX, ovstage_api_status_t)),
        ("get_last_op_error", _CFT(ovx_string_t, _CTX, ovstage_op_id_t)),
        # Resources
        ("get_path_dictionary", _CFT(path_dictionary_instance_p, _CTX)),
        # Batched write
        ("write_attributes", _CFT(_ENQ, _CTX, ovstage_query_handle_t,
                                   ctypes.POINTER(ovstage_attribute_write_t), ctypes.c_size_t,
                                   ovstage_ordinal_t, ctypes.c_int)),
        # Extensions
        ("query_extension", _CFT(_ERR, _CTX, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p))),
    ]


ovstage_vtable_p = ctypes.POINTER(ovstage_vtable_t)

# Real layout for the instance bundle declared field-less above. ovstage_population_*
# and the lifecycle entry points take a pointer to this struct; the data-plane
# slots are reached through ``.vtable`` with ``.context`` as their first arg.
ovstage_instance_t._fields_ = [
    ("vtable", ovstage_vtable_p),
    ("context", ovstage_context_p),
]

# Map the historical flat data-plane symbol names to their vtable slot, so
# _InstanceApi can be driven by the same ``ovstage_<slot>`` names the call sites
# already use.
_VTABLE_SLOT_BY_SYMBOL = {"ovstage_" + name: name for (name, _ctype) in ovstage_vtable_t._fields_}


class _InstanceApi:
    """Data-plane facade dispatching ovstage_* slots through an instance vtable.

    Call sites pass the ``ovstage_instance_t*`` bundle pointer as the first
    argument exactly as they did against the old flat ABI; for a data-plane slot
    this resolves ``instance->vtable->slot`` and forwards ``instance->context``
    as the C ``instance`` (context) argument. Any other attribute — instance
    lifecycle and auxiliary APIs such as ``ovstage_instancing_*`` and
    ``ovstage_population_*`` - is delegated unchanged to the underlying CDLL
    (still flat exports).
    """

    def __init__(self, lib: ctypes.CDLL, invalid_instance_error=None):
        self._lib = lib
        self._invalid_instance_error = invalid_instance_error

    def __getattr__(self, name: str):
        slot = _VTABLE_SLOT_BY_SYMBOL.get(name)
        if slot is None:
            return getattr(self._lib, name)  # flat symbol

        def _dispatch(instance_ptr, *args):
            if not instance_ptr:
                if self._invalid_instance_error is not None:
                    raise self._invalid_instance_error(name)
                raise RuntimeError(f"{name} called with a null ovstage instance")
            bundle = instance_ptr.contents
            fn = getattr(bundle.vtable.contents, slot)
            return fn(bundle.context, *args)

        _dispatch.__name__ = name
        return _dispatch


def instance_api(lib: ctypes.CDLL, invalid_instance_error=None) -> "_InstanceApi":
    """Return a facade dispatching ovstage data-plane slots through the vtable."""
    return _InstanceApi(lib, invalid_instance_error)


# ── Library discovery / loading (mirrors ovrtx's _LibraryLoader) ────────────
OVSTAGE_LIBRARY_PATH_HINT: Optional[str] = None

if sys.platform.startswith("win"):
    OVSTAGE_LIB_NAME = "ovstage.dll"
elif sys.platform == "darwin":
    OVSTAGE_LIB_NAME = "libovstage.dylib"
else:
    OVSTAGE_LIB_NAME = "libovstage.so"


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
    """Lazily loads and configures a singleton ``libovstage`` CDLL."""

    def __init__(self):
        self._lib: Optional[ctypes.CDLL] = None
        self._version: Optional[tuple] = None
        self._dll_dir_cookies: list = []  # keep os.add_dll_directory handles alive (Windows)

    @property
    def is_loaded(self) -> bool:
        return self._lib is not None

    @property
    def version(self) -> Optional[tuple]:
        return self._version

    def load(self) -> ctypes.CDLL:
        if self._lib is None:
            lib = self._open()
            _configure_prototypes(lib)
            self._version = _probe_version(lib)
            self._lib = lib
        return self._lib

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
                # ovstage.dll's runtime dependencies sit alongside it and in a
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
    """Set argtypes/restype for the flat exports.

    The instance lifecycle and auxiliary APIs such as ``ovstage_instancing_*``
    and ``ovstage_population_*`` are flat symbols. The ovstage data plane and
    the path dictionary are both reached through vtables (see
    ``ovstage_vtable_t`` / :class:`_InstanceApi` and
    ``path_dictionary_vtable_t``), whose CFUNCTYPE slot declarations carry the
    argtypes/restype for those calls.
    """
    inst = ovstage_instance_p
    err = ovstage_api_status_t

    # Stage uses process lifecycle internally when a StageConfig is supplied.
    # The entry points remain absent from the top-level Python API.
    lib.ovstage_initialize.argtypes = [ctypes.POINTER(ovstage_config_t)]
    lib.ovstage_initialize.restype = err
    lib.ovstage_shutdown.argtypes = []
    lib.ovstage_shutdown.restype = err

    # Instance lifecycle (the only flat ovstage_* data-plane entry points). The
    # bundle ovstage_create_instance returns drives the vtable for everything else.
    lib.ovstage_create_instance.argtypes = [ctypes.POINTER(ovstage_instance_desc_t), ctypes.POINTER(inst)]
    lib.ovstage_create_instance.restype = err
    lib.ovstage_destroy_instance.argtypes = [inst]
    lib.ovstage_destroy_instance.restype = err

    # ovstage_clone: flat data-plane entry point (peer of create/destroy, not a
    # vtable slot). Enqueues a subtree clone and returns {status, op_index}; awaited
    # via the vtable ``wait_op`` / ``release_op``.
    if hasattr(lib, "ovstage_clone"):
        lib.ovstage_clone.argtypes = [
            inst, ovx_string_t, ctypes.POINTER(ovx_string_t), ctypes.c_size_t, ovstage_ordinal_t
        ]
        lib.ovstage_clone.restype = ovstage_enqueue_result_t

    # Hierarchy: flat data-plane entry points. Lookup/compute enqueue normal
    # ovstage ops and are observed through the vtable wait_op/release_op path.
    if hasattr(lib, "ovstage_get_hierarchy"):
        lib.ovstage_get_hierarchy.argtypes = [
            inst,
            ovx_primpath_list_t,
            ovstage_ordinal_t,
            ovstage_hierarchy_relation_t,
            ctypes.POINTER(ovstage_hierarchy_handle_t),
        ]
        lib.ovstage_get_hierarchy.restype = ovstage_enqueue_result_t
        lib.ovstage_fetch_hierarchy_result.argtypes = [
            inst,
            ovstage_hierarchy_handle_t,
            ctypes.POINTER(ovstage_hierarchy_result_t),
        ]
        lib.ovstage_fetch_hierarchy_result.restype = err
        lib.ovstage_release_hierarchy_result.argtypes = [inst, ctypes.POINTER(ovstage_hierarchy_result_t)]
        lib.ovstage_release_hierarchy_result.restype = err
        lib.ovstage_release_hierarchy.argtypes = [inst, ovstage_hierarchy_handle_t]
        lib.ovstage_release_hierarchy.restype = ovstage_enqueue_result_t
        lib.ovstage_get_hierarchy_computation_models.argtypes = [
            inst,
            ctypes.POINTER(ctypes.POINTER(ovstage_hierarchy_computation_model_desc_t)),
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.ovstage_get_hierarchy_computation_models.restype = err
        lib.ovstage_compute_hierarchy.argtypes = [
            inst,
            ovstage_hierarchy_computation_model_id_t,
            ovstage_ordinal_t,
            ovstage_ordinal_t,
        ]
        lib.ovstage_compute_hierarchy.restype = ovstage_enqueue_result_t

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

    # The path dictionary is reached through the ``get_path_dictionary`` vtable
    # slot (see ``ovstage_vtable_t``) rather than flat exports — nothing to
    # configure here.

    # ── ovstage_instancing (synchronous scene-graph-instancing queries) ──────
    if hasattr(lib, "ovstage_instancing_get_instance_roots"):
        lib.ovstage_instancing_get_instance_roots.argtypes = [
            inst,
            ovx_primpath_t,
            ctypes.POINTER(ovx_primpath_list_t),
        ]
        lib.ovstage_instancing_get_instance_roots.restype = err
        lib.ovstage_instancing_get_prototype_root.argtypes = [inst, ovx_primpath_t, ctypes.POINTER(ovx_primpath_t)]
        lib.ovstage_instancing_get_prototype_root.restype = err
        lib.ovstage_instancing_get_prototype_roots.argtypes = [inst, ctypes.POINTER(ovx_primpath_list_t)]
        lib.ovstage_instancing_get_prototype_roots.restype = err

    # ── ovstage_population (optional USD population bridge) ─
    # Async enqueue/wait model: the mutating calls take ``ovx_string_t`` views by
    # value and return an ``ovstage_population_enqueue_result_t`` ({status, op_index});
    # completion is awaited via the flat ``ovstage_population_wait_op``.
    pop_enq = ovstage_population_enqueue_result_t
    ref_handle = ovstage_population_usd_reference_handle_t
    if hasattr(lib, "ovstage_population_open_usd_from_file"):
        lib.ovstage_population_open_usd_from_file.argtypes = [inst, ovx_string_t, ovstage_ordinal_t,
                                                              ctypes.c_double, ctypes.c_uint32]
        lib.ovstage_population_open_usd_from_file.restype = pop_enq
        lib.ovstage_population_open_usd_from_string.argtypes = [inst, ovx_string_t, ovstage_ordinal_t,
                                                                ctypes.c_double, ctypes.c_uint32]
        lib.ovstage_population_open_usd_from_string.restype = pop_enq
        lib.ovstage_population_add_usd_reference_from_file.argtypes = [inst, ovx_string_t, ovx_string_t,
                                                                       ctypes.POINTER(ref_handle)]
        lib.ovstage_population_add_usd_reference_from_file.restype = pop_enq
        lib.ovstage_population_add_usd_reference_from_string.argtypes = [inst, ovx_string_t, ovx_string_t,
                                                                         ctypes.POINTER(ref_handle)]
        lib.ovstage_population_add_usd_reference_from_string.restype = pop_enq
        lib.ovstage_population_remove_usd_reference.argtypes = [inst, ref_handle]
        lib.ovstage_population_remove_usd_reference.restype = pop_enq
        lib.ovstage_population_reset_usd.argtypes = [inst]
        lib.ovstage_population_reset_usd.restype = pop_enq
        lib.ovstage_population_apply_usd_time.argtypes = [inst, ovstage_ordinal_t, ctypes.c_double]
        lib.ovstage_population_apply_usd_time.restype = pop_enq
        lib.ovstage_population_apply_usd_changes.argtypes = [inst, ovstage_ordinal_t]
        lib.ovstage_population_apply_usd_changes.restype = pop_enq
        lib.ovstage_population_wait_op.argtypes = [inst, ovstage_population_op_id_t, ovstage_timeout_ns_t,
                                                   ctypes.POINTER(ovstage_population_op_wait_result_t)]
        lib.ovstage_population_wait_op.restype = err
        lib.ovstage_population_get_last_error.argtypes = []
        lib.ovstage_population_get_last_error.restype = ovx_string_t
        lib.ovstage_population_get_last_op_error.argtypes = [ovstage_population_op_id_t]
        lib.ovstage_population_get_last_op_error.restype = ovx_string_t

def _probe_version(lib: ctypes.CDLL) -> tuple:
    """Read the implementation version via a throwaway instance's vtable.

    ``get_version`` is a vtable slot (it needs a context), so unlike the old
    flat ABI there is no instance-free way to read the version. Creating and
    destroying a probe instance here doubles as a smoke test that the library
    loads, the bundle layout matches, and vtable dispatch works.
    """
    desc = ovstage_instance_desc_t()
    desc.name = b"ovstage-python-version-probe"
    inst = ovstage_instance_p()
    code = lib.ovstage_create_instance(ctypes.byref(desc), ctypes.byref(inst))
    if code != OVSTAGE_OK:
        raise RuntimeError(f"ovstage_create_instance failed during version probe (code {int(code)})")
    try:
        bundle = inst.contents
        major, minor, patch = ctypes.c_uint32(), ctypes.c_uint32(), ctypes.c_uint32()
        bundle.vtable.contents.get_version(
            bundle.context, ctypes.byref(major), ctypes.byref(minor), ctypes.byref(patch)
        )
        return (major.value, minor.value, patch.value)
    finally:
        lib.ovstage_destroy_instance(inst)


_loader = _LibraryLoader()


def load() -> ctypes.CDLL:
    """Return the configured singleton ``libovstage`` CDLL (loading on first use)."""
    return _loader.load()


def library_version() -> Optional[tuple]:
    """Return the loaded library's ``(major, minor, patch)`` version.

    Forces the lazy load so a standalone ``ovstage.library_version()`` returns a real
    version (or raises if the library can't load) instead of ``None`` before first use.
    """
    _loader.load()
    return _loader.version


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
    from .types import LogSeverity

    if severity is None:
        severity = LogSeverity.WARNING

    lib = _loader.load()
    _require(lib, "ovstage_set_log_callback")

    filter_ptr = ctypes.byref(ovx_string_t(channel_filter)) if channel_filter else None

    if callback is None:
        _check_process_status(
            lib.ovstage_set_log_callback(int(severity), filter_ptr, _log_callback_t(), None),
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
        lib.ovstage_set_log_callback(int(severity), filter_ptr, trampoline, None),
        "ovstage_set_log_callback",
    )
    # Publish only after the install succeeds. Retire (don't free) the previous
    # trampoline: the dispatcher may still fire it for a message it dequeued
    # before the swap. flush_log() later clears the retired list.
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
