# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Raw ctypes layer for ovpopulation runtime -> USD export.

This module is intentionally separate from ``bindings.py`` so the broad ovstage
ctypes layout stays focused on the core data plane and USD population ingest.
The export API is descriptor-heavy and changes on its own cadence, so its C
struct mirrors and flat-symbol prototypes live here.
"""

import ctypes
from typing import Optional

from . import bindings as _core

ovstage_api_status_t = _core.ovstage_api_status_t
ovstage_ordinal_t = _core.ovstage_ordinal_t
ovstage_instance_p = _core.ovstage_instance_p
ovx_string_t = _core.ovx_string_t
ovstage_population_op_id_t = _core.ovstage_population_op_id_t
ovstage_timeout_ns_t = _core.ovstage_timeout_ns_t
ovstage_population_enqueue_result_t = _core.ovstage_population_enqueue_result_t

OVSTAGE_OK = _core.OVSTAGE_OK
OVSTAGE_ERROR_NOT_SUPPORTED = _core.OVSTAGE_ERROR_NOT_SUPPORTED
OVSTAGE_ERROR_TIMEOUT = _core.OVSTAGE_ERROR_TIMEOUT
OVSTAGE_TIMEOUT_INFINITE = _core.OVSTAGE_TIMEOUT_INFINITE

OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE = 0
OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL = 1

OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER = 0
OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF = 1

OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT = 0
OVSTAGE_POPULATION_EXPORT_PATH_MATCH_PREFIX = 1

OVSTAGE_POPULATION_EXPORT_NAME_MATCH_EXACT = 0
OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB = 1

OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP = 0
OVSTAGE_POPULATION_EXPORT_TRANSFORM_NONE = 1
OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_MATRIX_OP = 2

OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP = 0
OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_PRESERVE = 1
OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_FAIL = 2

OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY = 0
OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_APPLY_RECORDED = 1

OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES = 0
OVSTAGE_POPULATION_EXPORT_PROJECTION_SCHEMA_DECLARED = 1

OVSTAGE_POPULATION_EXPORT_RULE_NONE = 0
OVSTAGE_POPULATION_EXPORT_RULE_EXPORT_DEFAULT_VALUE = 1 << 0
OVSTAGE_POPULATION_EXPORT_RULE_DERIVED_ALLOWED = 1 << 1
OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE = 1 << 3
OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED = 1 << 4

OVSTAGE_POPULATION_EXPORT_PROPERTY_INVALID = 0
OVSTAGE_POPULATION_EXPORT_PROPERTY_RELATIONSHIP = 1
OVSTAGE_POPULATION_EXPORT_PROPERTY_CONNECTION = 2
OVSTAGE_POPULATION_EXPORT_PROPERTY_MATERIAL_BINDING = 3

OVSTAGE_POPULATION_EXPORT_METADATA_PRIM = 0
OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_CUSTOM_DATA = 1
OVSTAGE_POPULATION_EXPORT_METADATA_REFERENCES = 2
OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_STANDARD = 3

OVSTAGE_POPULATION_EXPORT_DESTINATION_EMPTY = 0
OVSTAGE_POPULATION_EXPORT_DESTINATION_OPEN_EXISTING = 1
OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE = 0


ovstage_population_prim_predicate_t = _core.ovstage_population_prim_predicate_t
ovstage_population_property_predicate_t = _core.ovstage_population_property_predicate_t
ovstage_population_export_destination_handle_t = ctypes.c_uint64


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


_configured_lib: Optional[ctypes.CDLL] = None


def configure_prototypes(lib: ctypes.CDLL) -> None:
    """Set argtypes/restype for ovstage population export flat symbols."""
    global _configured_lib
    if _configured_lib is lib:
        return

    inst = ovstage_instance_p
    err = ovstage_api_status_t
    if hasattr(lib, "ovstage_population_export_desc_init"):
        lib.ovstage_population_export_desc_init.argtypes = [ctypes.POINTER(ovstage_population_export_desc_t)]
        lib.ovstage_population_export_desc_init.restype = err
        if hasattr(lib, "ovstage_population_export_desc_init_snapshot"):
            lib.ovstage_population_export_desc_init_snapshot.argtypes = [
                ctypes.POINTER(ovstage_population_export_desc_t),
                ovstage_ordinal_t,
            ]
            lib.ovstage_population_export_desc_init_snapshot.restype = err
        if hasattr(lib, "ovstage_population_export_destination_create"):
            lib.ovstage_population_export_destination_create.argtypes = [
                inst,
                ovx_string_t,
                ctypes.c_int,
                ctypes.POINTER(ovstage_population_export_destination_handle_t),
            ]
            lib.ovstage_population_export_destination_create.restype = err
        if hasattr(lib, "ovstage_population_export_to_destination"):
            lib.ovstage_population_export_to_destination.argtypes = [
                inst,
                ovstage_population_export_destination_handle_t,
                ctypes.POINTER(ovstage_population_export_desc_t),
                ctypes.POINTER(ovstage_population_export_report_t),
            ]
            lib.ovstage_population_export_to_destination.restype = err
        if hasattr(lib, "ovstage_population_export_destination_save"):
            lib.ovstage_population_export_destination_save.argtypes = [
                inst,
                ovstage_population_export_destination_handle_t,
            ]
            lib.ovstage_population_export_destination_save.restype = err
        if hasattr(lib, "ovstage_population_export_destination_destroy"):
            lib.ovstage_population_export_destination_destroy.argtypes = [
                inst,
                ovstage_population_export_destination_handle_t,
            ]
            lib.ovstage_population_export_destination_destroy.restype = err
        if hasattr(lib, "ovstage_population_export_to_usd_file"):
            lib.ovstage_population_export_to_usd_file.argtypes = [
                inst,
                ovx_string_t,
                ctypes.POINTER(ovstage_population_export_desc_t),
                ctypes.POINTER(ovstage_population_export_report_t),
            ]
            lib.ovstage_population_export_to_usd_file.restype = err

    if hasattr(lib, "ovstage_population_export_destination_enqueue_create"):
        enqueue_create = lib.ovstage_population_export_destination_enqueue_create
        enqueue_create.argtypes = [
            inst,
            ovx_string_t,
            ctypes.c_int,
            ctypes.POINTER(ovstage_population_export_destination_handle_t),
        ]
        enqueue_create.restype = ovstage_population_enqueue_result_t
    if hasattr(lib, "ovstage_population_export_enqueue_to_destination"):
        enqueue_destination = lib.ovstage_population_export_enqueue_to_destination
        enqueue_destination.argtypes = [
            inst,
            ovstage_population_export_destination_handle_t,
            ctypes.POINTER(ovstage_population_export_desc_t),
        ]
        enqueue_destination.restype = ovstage_population_enqueue_result_t
    if hasattr(lib, "ovstage_population_export_destination_enqueue_save"):
        enqueue_save = lib.ovstage_population_export_destination_enqueue_save
        enqueue_save.argtypes = [inst, ovstage_population_export_destination_handle_t]
        enqueue_save.restype = ovstage_population_enqueue_result_t
    if hasattr(lib, "ovstage_population_export_destination_enqueue_destroy"):
        enqueue_destroy = lib.ovstage_population_export_destination_enqueue_destroy
        enqueue_destroy.argtypes = [inst, ovstage_population_export_destination_handle_t]
        enqueue_destroy.restype = ovstage_population_enqueue_result_t
    if hasattr(lib, "ovstage_population_export_enqueue_to_usd_file"):
        enqueue_file = lib.ovstage_population_export_enqueue_to_usd_file
        enqueue_file.argtypes = [
            inst,
            ovx_string_t,
            ctypes.POINTER(ovstage_population_export_desc_t),
        ]
        enqueue_file.restype = ovstage_population_enqueue_result_t
    if hasattr(lib, "ovstage_population_export_wait_op"):
        lib.ovstage_population_export_wait_op.argtypes = [
            inst,
            ovstage_population_op_id_t,
            ovstage_timeout_ns_t,
            ctypes.POINTER(ovstage_population_export_report_t),
        ]
        lib.ovstage_population_export_wait_op.restype = err

    _configured_lib = lib


def load() -> ctypes.CDLL:
    lib = _core.load()
    configure_prototypes(lib)
    return lib


def available() -> bool:
    try:
        return hasattr(load(), "ovstage_population_export_to_usd_file")
    except Exception:  # noqa: BLE001 - library not loadable in this environment
        return False
