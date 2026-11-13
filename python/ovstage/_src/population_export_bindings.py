# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Constants and type aliases of the export functions of the population family.

The export functions that take a stage are the export members of the
population family vtable, ``ovstage_population_vtable_t`` in ``bindings.py``.
That module mirrors the export types of ``ovstage_population_types.h``: the
destination handle, the rules, the descriptor and the report. This module
holds the export constants, re-exports the export types under their C names,
and binds the prototypes of the descriptor initializers, the flat exports of
the export functions.
"""

import ctypes
from typing import Optional

from . import bindings as _core

ovstage_api_status_t = _core.ovstage_api_status_t
ovstage_ordinal_t = _core.ovstage_ordinal_t
ovstage_population_op_id_t = _core.ovstage_population_op_id_t
ovx_string_t = _core.ovx_string_t

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
OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE = _core.OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE


# The export types of ovstage_population_types.h. ``bindings.py`` defines the
# mirrors, because the export members of ``ovstage_population_vtable_t`` name
# them. The names below are the C names.
ovstage_population_prim_predicate_t = _core.ovstage_population_prim_predicate_t
ovstage_population_property_predicate_t = _core.ovstage_population_property_predicate_t
ovstage_population_export_destination_handle_t = _core.ovstage_population_export_destination_handle_t
ovstage_population_export_attribute_rule_t = _core.ovstage_population_export_attribute_rule_t
ovstage_population_export_prim_rule_t = _core.ovstage_population_export_prim_rule_t
ovstage_population_export_api_schema_rule_t = _core.ovstage_population_export_api_schema_rule_t
ovstage_population_export_property_edge_rule_t = _core.ovstage_population_export_property_edge_rule_t
ovstage_population_export_metadata_rule_t = _core.ovstage_population_export_metadata_rule_t
ovstage_population_export_desc_t = _core.ovstage_population_export_desc_t
ovstage_population_export_report_t = _core.ovstage_population_export_report_t


_configured_lib: Optional[ctypes.CDLL] = None


def configure_prototypes(lib: ctypes.CDLL) -> None:
    """Set argtypes/restype for the export descriptor initializers.

    These are the flat exports of the export functions. The export functions
    that take a stage are the export members of the population family vtable.
    """
    global _configured_lib
    if _configured_lib is lib:
        return

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

    _configured_lib = lib


def load() -> ctypes.CDLL:
    lib = _core.load()
    configure_prototypes(lib)
    return lib


def available() -> bool:
    """True when the loaded libovstage exports the export descriptor initializers."""
    try:
        return hasattr(load(), "ovstage_population_export_desc_init")
    except Exception:  # noqa: BLE001 - library not loadable in this environment
        return False
