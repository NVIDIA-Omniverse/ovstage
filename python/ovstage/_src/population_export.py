# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Python surface for ovpopulation runtime -> USD export."""

import ctypes
from enum import IntEnum
from typing import Callable, Optional

from . import bindings as _core
from . import population_export_bindings as _b
from .selectors import (
    PrimPredicate,
    PropertyPredicate,
    _build_prim_predicate,
    _build_property_predicate,
    _check_predicate,
)
from .types import (
    OvstageError,
    _warn_dropped_resource,
    check_enum_member,
    check_ordinal,
    check_timeout,
    check_uint32,
    check_uint64,
)

OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE = _b.OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE
OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL = (
    _b.OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL
)
OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER = _b.OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER
OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF = _b.OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF
OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT = _b.OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT
OVSTAGE_POPULATION_EXPORT_PATH_MATCH_PREFIX = _b.OVSTAGE_POPULATION_EXPORT_PATH_MATCH_PREFIX
OVSTAGE_POPULATION_EXPORT_NAME_MATCH_EXACT = _b.OVSTAGE_POPULATION_EXPORT_NAME_MATCH_EXACT
OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB = _b.OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB
OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP = _b.OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP
OVSTAGE_POPULATION_EXPORT_TRANSFORM_NONE = _b.OVSTAGE_POPULATION_EXPORT_TRANSFORM_NONE
OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_MATRIX_OP = _b.OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_MATRIX_OP
OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP = _b.OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP
OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_PRESERVE = _b.OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_PRESERVE
OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_FAIL = _b.OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_FAIL
OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY = (
    _b.OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY
)
OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_APPLY_RECORDED = (
    _b.OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_APPLY_RECORDED
)
OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES = _b.OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES
OVSTAGE_POPULATION_EXPORT_PROJECTION_SCHEMA_DECLARED = _b.OVSTAGE_POPULATION_EXPORT_PROJECTION_SCHEMA_DECLARED
OVSTAGE_POPULATION_EXPORT_UNKNOWN_APPLIED_API_SCHEMAS_CUSTOM_DATA_KEY = (
    "ovpopulation:unknownAppliedAPISchemas"
)
OVSTAGE_POPULATION_EXPORT_RULE_NONE = _b.OVSTAGE_POPULATION_EXPORT_RULE_NONE
OVSTAGE_POPULATION_EXPORT_RULE_EXPORT_DEFAULT_VALUE = _b.OVSTAGE_POPULATION_EXPORT_RULE_EXPORT_DEFAULT_VALUE
OVSTAGE_POPULATION_EXPORT_RULE_DERIVED_ALLOWED = _b.OVSTAGE_POPULATION_EXPORT_RULE_DERIVED_ALLOWED
OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE = _b.OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE
OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED = _b.OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED
OVSTAGE_POPULATION_EXPORT_PROPERTY_RELATIONSHIP = _b.OVSTAGE_POPULATION_EXPORT_PROPERTY_RELATIONSHIP
OVSTAGE_POPULATION_EXPORT_PROPERTY_CONNECTION = _b.OVSTAGE_POPULATION_EXPORT_PROPERTY_CONNECTION
OVSTAGE_POPULATION_EXPORT_PROPERTY_MATERIAL_BINDING = _b.OVSTAGE_POPULATION_EXPORT_PROPERTY_MATERIAL_BINDING
OVSTAGE_POPULATION_EXPORT_METADATA_PRIM = _b.OVSTAGE_POPULATION_EXPORT_METADATA_PRIM
OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_CUSTOM_DATA = _b.OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_CUSTOM_DATA
OVSTAGE_POPULATION_EXPORT_METADATA_REFERENCES = _b.OVSTAGE_POPULATION_EXPORT_METADATA_REFERENCES
OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_STANDARD = _b.OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_STANDARD
OVSTAGE_POPULATION_EXPORT_DESTINATION_EMPTY = _b.OVSTAGE_POPULATION_EXPORT_DESTINATION_EMPTY
OVSTAGE_POPULATION_EXPORT_DESTINATION_OPEN_EXISTING = _b.OVSTAGE_POPULATION_EXPORT_DESTINATION_OPEN_EXISTING


def export_available() -> bool:
    """True when libovstage exports the ovstage population USD export bridge."""
    return _b.available()


def _last_error() -> str:
    try:
        return str(_core.load().ovstage_population_get_last_error())
    except Exception:  # noqa: BLE001
        return ""


def _require_export(stage):
    lib = _b.load()
    if not hasattr(lib, "ovstage_population_export_to_usd_file"):
        raise OvstageError(
            _b.OVSTAGE_ERROR_NOT_SUPPORTED,
            "libovstage was built without the ovstage population export-to-USD bridge",
        )
    return lib, stage._inst


def _require_destination_symbols(stage):
    lib, inst = _require_export(stage)
    required = (
        "ovstage_population_export_destination_create",
        "ovstage_population_export_destination_enqueue_create",
        "ovstage_population_export_to_destination",
        "ovstage_population_export_enqueue_to_destination",
        "ovstage_population_export_destination_save",
        "ovstage_population_export_destination_enqueue_save",
        "ovstage_population_export_destination_destroy",
        "ovstage_population_export_destination_enqueue_destroy",
    )
    if not all(hasattr(lib, name) for name in required):
        raise OvstageError(
            _b.OVSTAGE_ERROR_NOT_SUPPORTED,
            "libovstage was built without reusable USD export destinations",
        )
    return lib, inst


class ExportOperation:
    """Handle for an enqueued runtime-to-USD export operation.

    Every accepted operation must eventually receive a terminal :meth:`wait`.
    Timeout polling does not consume the native report or destination keepalive.
    """

    def __init__(self, stage, lib, status: int, op_id: int, *, name: str):
        self._stage = stage
        self._lib = lib
        self.status = int(status)
        self.op_id = int(op_id)
        self._name = name
        self._terminal = False

    @property
    def ok(self) -> bool:
        return self.status == _b.OVSTAGE_OK

    def error_message(self) -> str:
        try:
            return str(self._lib.ovstage_population_get_last_op_error(self.op_id))
        except Exception:  # noqa: BLE001 - fall back to the shared last-error channel
            return ""

    def wait(self, timeout: int = _b.OVSTAGE_TIMEOUT_INFINITE):
        """Return the export report, None on timeout, or raise on failure."""
        if self._terminal:
            raise RuntimeError("export operation result has already been consumed")
        if self.status != _b.OVSTAGE_OK:
            self._terminal = True
            raise OvstageError(self.status, _last_error())

        report = _b.ovstage_population_export_report_t()
        code = self._lib.ovstage_population_export_wait_op(
            self._stage._inst,
            self.op_id,
            check_timeout(timeout),
            ctypes.byref(report),
        )
        if code == _b.OVSTAGE_ERROR_TIMEOUT:
            return None
        self._terminal = True
        if code == _b.OVSTAGE_OK:
            return _report_to_dict(report)
        raise OvstageError(code, self.error_message() or _last_error())


class ExportDestinationMode(IntEnum):
    """Initial-content policy for a reusable USD export destination."""

    EMPTY = OVSTAGE_POPULATION_EXPORT_DESTINATION_EMPTY
    OPEN_EXISTING = OVSTAGE_POPULATION_EXPORT_DESTINATION_OPEN_EXISTING


class SourceApiSchemaPolicy(IntEnum):
    """Policy for applied API schemas recorded on source prims."""

    EXPLICIT_ONLY = OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY
    APPLY_RECORDED = OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_APPLY_RECORDED


class ExportProjection(IntEnum):
    """Automatic source-attribute projection policy."""

    EXPLICIT_RULES = OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES
    SCHEMA_DECLARED = OVSTAGE_POPULATION_EXPORT_PROJECTION_SCHEMA_DECLARED


class _ExportSelection(IntEnum):
    EXPLICIT_PREDICATE = OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE
    CHANGED_PROPERTIES_SINCE_ORDINAL = (
        OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL
    )


class _ExportLayerMode(IntEnum):
    OVER = OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER
    DEF = OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF


class _PathMatch(IntEnum):
    EXACT = OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT
    PREFIX = OVSTAGE_POPULATION_EXPORT_PATH_MATCH_PREFIX


class _NameMatch(IntEnum):
    EXACT = OVSTAGE_POPULATION_EXPORT_NAME_MATCH_EXACT
    GLOB = OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB


class _TransformPolicy(IntEnum):
    USD_XFORM_OP = OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP
    NONE = OVSTAGE_POPULATION_EXPORT_TRANSFORM_NONE
    USD_MATRIX_OP = OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_MATRIX_OP


class _UnknownMetadataPolicy(IntEnum):
    DROP = OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP
    PRESERVE = OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_PRESERVE
    FAIL = OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_FAIL


class _PropertyKind(IntEnum):
    RELATIONSHIP = OVSTAGE_POPULATION_EXPORT_PROPERTY_RELATIONSHIP
    CONNECTION = OVSTAGE_POPULATION_EXPORT_PROPERTY_CONNECTION
    MATERIAL_BINDING = OVSTAGE_POPULATION_EXPORT_PROPERTY_MATERIAL_BINDING


class _MetadataKind(IntEnum):
    PRIM = OVSTAGE_POPULATION_EXPORT_METADATA_PRIM
    LAYER_CUSTOM_DATA = OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_CUSTOM_DATA
    REFERENCES = OVSTAGE_POPULATION_EXPORT_METADATA_REFERENCES
    LAYER_STANDARD = OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_STANDARD


class DestinationOperation:
    """Handle for an enqueued destination lifecycle operation."""

    def __init__(
        self,
        stage,
        lib,
        status: int,
        op_id: int,
        *,
        value=True,
        keepalive=None,
        on_terminal: Optional[Callable[[bool], None]] = None,
    ):
        self._stage = stage
        self._lib = lib
        self.status = int(status)
        self.op_id = int(op_id)
        self._value = value
        self._keepalive = keepalive
        self._on_terminal = on_terminal
        self._terminal = False

    @property
    def ok(self) -> bool:
        return self.status == _b.OVSTAGE_OK

    def _finish(self, success: bool) -> None:
        if self._terminal:
            return
        self._terminal = True
        self._keepalive = None
        if self._on_terminal is not None:
            self._on_terminal(success)

    def wait(self, timeout: int = _b.OVSTAGE_TIMEOUT_INFINITE):
        """Return the operation value, None on timeout, or raise on failure."""
        if self._terminal:
            raise RuntimeError("destination operation result has already been consumed")
        if self.status != _b.OVSTAGE_OK:
            self._finish(False)
            raise OvstageError(self.status, _last_error())

        code = self._lib.ovstage_population_export_wait_op(
            self._stage._inst,
            self.op_id,
            check_timeout(timeout),
            None,
        )
        if code == _b.OVSTAGE_ERROR_TIMEOUT:
            return None
        if code == _b.OVSTAGE_OK:
            self._finish(True)
            return self._value

        self._finish(False)
        raise OvstageError(code, str(self._lib.ovstage_population_get_last_op_error(self.op_id)) or _last_error())


class DestinationCreateOperation(DestinationOperation):
    """Async creation operation whose reserved destination is immediately available."""

    @property
    def destination(self):
        return self._value


class ExportDestination:
    """ABI-opaque, source-stage-bound USD export destination.

    Export calls accumulate in memory. Call :meth:`save` explicitly to persist;
    context-manager exit only closes and discards any unsaved changes.
    If an open destination is dropped while its source stage remains live, the
    finalizer best-effort queues release without saving and emits ``ResourceWarning``.
    """

    def __init__(self, stage, lib, handle: int, identifier: str):
        self._stage = stage
        self._lib = lib
        self._handle = check_uint64(handle, "destination handle")
        self.identifier = str(identifier)
        self._closing = False
        self._closed = False

    @classmethod
    def create(
        cls,
        stage,
        identifier: str,
        mode: int = ExportDestinationMode.EMPTY,
    ):
        """Create a destination and wait until it is ready."""
        lib, inst = _require_destination_symbols(stage)
        identifier_s = _b.ovx_string_t(identifier)
        handle = _b.ovstage_population_export_destination_handle_t()
        code = lib.ovstage_population_export_destination_create(
            inst,
            identifier_s,
            check_enum_member(mode, ExportDestinationMode, "export destination mode"),
            ctypes.byref(handle),
        )
        if code != _b.OVSTAGE_OK:
            raise OvstageError(code, _last_error())
        return cls(stage, lib, handle.value, identifier)

    @classmethod
    def create_async(
        cls,
        stage,
        identifier: str,
        mode: int = ExportDestinationMode.EMPTY,
    ) -> DestinationCreateOperation:
        """Enqueue creation and expose the reserved destination immediately."""
        lib, inst = _require_destination_symbols(stage)
        identifier_s = _b.ovx_string_t(identifier)
        handle = _b.ovstage_population_export_destination_handle_t()
        result = lib.ovstage_population_export_destination_enqueue_create(
            inst,
            identifier_s,
            check_enum_member(mode, ExportDestinationMode, "export destination mode"),
            ctypes.byref(handle),
        )
        if result.status != _b.OVSTAGE_OK:
            raise OvstageError(result.status, _last_error())
        destination = cls(stage, lib, handle.value, identifier)

        def creation_finished(success: bool) -> None:
            if not success:
                destination._closed = True

        return DestinationCreateOperation(
            stage,
            lib,
            result.status,
            result.op_index,
            value=destination,
            keepalive=[identifier_s],
            on_terminal=creation_finished,
        )

    @property
    def handle(self) -> int:
        return self._handle

    @property
    def closed(self) -> bool:
        return self._closed

    def _require_open(self) -> None:
        if self._closed or self._closing:
            raise RuntimeError("export destination is closing or closed")

    def save(self) -> None:
        """Persist all currently accumulated opinions."""
        self._require_open()
        code = self._lib.ovstage_population_export_destination_save(self._stage._inst, self._handle)
        if code != _b.OVSTAGE_OK:
            raise OvstageError(code, _last_error())

    def export(self, ordinal: int, **options) -> dict:
        """Export one selected runtime slice into this destination."""
        return export_to_destination(self._stage, self, ordinal, **options)

    def export_async(self, ordinal: int, **options) -> ExportOperation:
        """Enqueue one selected runtime slice into this destination."""
        return export_to_destination_async(self._stage, self, ordinal, **options)

    def save_async(self) -> DestinationOperation:
        """Enqueue persistence after previously submitted destination work."""
        self._require_open()
        result = self._lib.ovstage_population_export_destination_enqueue_save(self._stage._inst, self._handle)
        if result.status != _b.OVSTAGE_OK:
            raise OvstageError(result.status, _last_error())
        return DestinationOperation(self._stage, self._lib, result.status, result.op_index)

    def close(self) -> None:
        """Release the destination without implicitly saving."""
        if self._closed:
            return
        self._require_open()
        self._closing = True
        code = self._lib.ovstage_population_export_destination_destroy(self._stage._inst, self._handle)
        if code != _b.OVSTAGE_OK:
            self._closing = False
            raise OvstageError(code, _last_error())
        self._closed = True

    def close_async(self) -> DestinationOperation:
        """Enqueue ordered release without implicitly saving."""
        self._require_open()
        result = self._lib.ovstage_population_export_destination_enqueue_destroy(self._stage._inst, self._handle)
        if result.status != _b.OVSTAGE_OK:
            raise OvstageError(result.status, _last_error())
        self._closing = True

        def close_finished(success: bool) -> None:
            if success:
                self._closed = True
            else:
                self._closing = False

        return DestinationOperation(
            self._stage,
            self._lib,
            result.status,
            result.op_index,
            on_terminal=close_finished,
        )

    def __enter__(self):
        self._require_open()
        return self

    def __exit__(self, _exc_type, _exc_value, _traceback):
        if not self._closed and not self._closing:
            self.close()
        return False

    def __del__(self):
        # Finalization can run after the Stage/native library has already gone
        # away.  Do not dispatch in that case; the native instance owns the
        # remaining state.  A finalizer never saves an export destination.
        if getattr(self, "_closed", True) or getattr(self, "_closing", False):
            return
        stage = getattr(self, "_stage", None)
        lib = getattr(self, "_lib", None)
        handle = getattr(self, "_handle", None)
        if stage is None or lib is None or handle is None or not getattr(stage, "_inst", None):
            return
        try:
            # Do not synchronously wait during GC.  The native FIFO owns the
            # queued release and never persists the destination.
            result = lib.ovstage_population_export_destination_enqueue_destroy(stage._inst, handle)
            if result.status == _b.OVSTAGE_OK:
                self._closing = True
        except Exception:
            pass
        _warn_dropped_resource(
            "ExportDestination was garbage-collected without close(); queued a "
            "best-effort destroy without saving when possible. Use a 'with' block "
            "or call close() explicitly after save() when persistence is intended."
        )


def _enqueue_export_operation(stage, lib, result, *, name: str) -> ExportOperation:
    if result.status != _b.OVSTAGE_OK:
        raise OvstageError(result.status, _last_error())
    return ExportOperation(
        stage,
        lib,
        result.status,
        result.op_index,
        name=name,
    )


def _string(value) -> _b.ovx_string_t:
    return _b.ovx_string_t(value or "")


def _rule_value(rule, name, default=None):
    if isinstance(rule, dict):
        return rule.get(name, default)
    return default


def _make_attribute_rules(rules):
    if not rules:
        return None, []
    out = []
    for item in rules:
        if isinstance(item, str):
            item = {"source_attribute_name": item}
        rule = _b.ovstage_population_export_attribute_rule_t()
        rule.path = _string(_rule_value(item, "path", ""))
        rule.path_match = check_enum_member(
            _rule_value(item, "path_match", OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT),
            _PathMatch,
            "attribute rule path_match",
        )
        rule.source_attribute_name = _string(_rule_value(item, "source_attribute_name", ""))
        rule.source_attribute_name_match = check_enum_member(
            _rule_value(item, "source_attribute_name_match", OVSTAGE_POPULATION_EXPORT_NAME_MATCH_EXACT),
            _NameMatch,
            "attribute rule source_attribute_name_match",
        )
        rule.destination_attribute_name = _string(_rule_value(item, "destination_attribute_name", ""))
        rule.usd_type_name = _string(_rule_value(item, "usd_type_name", ""))
        rule.required_schema_name = _string(_rule_value(item, "required_schema_name", ""))
        rule.flags = check_uint32(_rule_value(item, "flags", 0), "attribute rule flags")
        out.append(rule)
    arr = (_b.ovstage_population_export_attribute_rule_t * len(out))(*out)
    return arr, [arr]


def _make_property_rules(rules):
    if not rules:
        return None, []
    out = []
    for item in rules:
        rule = _b.ovstage_population_export_property_edge_rule_t()
        rule.path = _string(_rule_value(item, "path", ""))
        rule.path_match = check_enum_member(
            _rule_value(item, "path_match", OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT),
            _PathMatch,
            "property rule path_match",
        )
        rule.source_property_name = _string(_rule_value(item, "source_property_name", ""))
        rule.destination_property_name = _string(_rule_value(item, "destination_property_name", ""))
        rule.usd_type_name = _string(_rule_value(item, "usd_type_name", ""))
        property_kind = _rule_value(item, "property_kind")
        if property_kind is None:
            raise ValueError("property rule requires an explicit property_kind")
        rule.property_kind = check_enum_member(property_kind, _PropertyKind, "property rule property_kind")
        rule.flags = check_uint32(_rule_value(item, "flags", 0), "property rule flags")
        out.append(rule)
    arr = (_b.ovstage_population_export_property_edge_rule_t * len(out))(*out)
    return arr, [arr]


def _make_prim_rules(rules):
    if not rules:
        return None, []
    out = []
    for item in rules:
        rule = _b.ovstage_population_export_prim_rule_t()
        rule.path = _string(_rule_value(item, "path", ""))
        rule.path_match = check_enum_member(
            _rule_value(item, "path_match", OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT),
            _PathMatch,
            "prim rule path_match",
        )
        rule.usd_type_name = _string(_rule_value(item, "usd_type_name", ""))
        rule.unknown_type_metadata_key = _string(_rule_value(item, "unknown_type_metadata_key", ""))
        out.append(rule)
    arr = (_b.ovstage_population_export_prim_rule_t * len(out))(*out)
    return arr, [arr]


def _make_api_schema_rules(rules):
    if not rules:
        return None, []
    out = []
    for item in rules:
        rule = _b.ovstage_population_export_api_schema_rule_t()
        rule.path = _string(_rule_value(item, "path", ""))
        rule.path_match = check_enum_member(
            _rule_value(item, "path_match", OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT),
            _PathMatch,
            "API schema rule path_match",
        )
        rule.schema_name = _string(_rule_value(item, "schema_name", ""))
        rule.instance_name = _string(_rule_value(item, "instance_name", ""))
        rule.flags = check_uint32(_rule_value(item, "flags", 0), "API schema rule flags")
        out.append(rule)
    arr = (_b.ovstage_population_export_api_schema_rule_t * len(out))(*out)
    return arr, [arr]


def _make_metadata_rules(rules):
    if not rules:
        return None, []
    out = []
    for item in rules:
        rule = _b.ovstage_population_export_metadata_rule_t()
        rule.path = _string(_rule_value(item, "path", ""))
        rule.path_match = check_enum_member(
            _rule_value(item, "path_match", OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT),
            _PathMatch,
            "metadata rule path_match",
        )
        rule.source_metadata_name = _string(_rule_value(item, "source_metadata_name", ""))
        rule.destination_metadata_name = _string(_rule_value(item, "destination_metadata_name", ""))
        rule.metadata_kind = check_enum_member(
            _rule_value(item, "metadata_kind", OVSTAGE_POPULATION_EXPORT_METADATA_PRIM),
            _MetadataKind,
            "metadata rule metadata_kind",
        )
        rule.flags = check_uint32(_rule_value(item, "flags", 0), "metadata rule flags")
        out.append(rule)
    arr = (_b.ovstage_population_export_metadata_rule_t * len(out))(*out)
    return arr, [arr]


def _report_to_dict(report: _b.ovstage_population_export_report_t) -> dict:
    return {name: getattr(report, name) for name, _ in report._fields_}


def _build_export_desc(
    lib,
    ordinal: int,
    *,
    since_ordinal: int,
    selection: int,
    layer_mode: int,
    transform_policy: int,
    unknown_metadata_policy: int,
    source_api_schema_policy: int,
    projection: int,
    prim_predicate,
    property_predicate,
    prim_rules,
    api_schema_rules,
    attribute_rules,
    property_rules,
    metadata_rules,
):
    predicate_refs = []
    predicate_memo = {}
    prim_predicate = PrimPredicate.all() if prim_predicate is None else _check_predicate(prim_predicate, PrimPredicate)
    property_predicate = (
        PropertyPredicate.all()
        if property_predicate is None
        else _check_predicate(property_predicate, PropertyPredicate)
    )
    prim_arr, prim_refs = _make_prim_rules(prim_rules)
    api_arr, api_refs = _make_api_schema_rules(api_schema_rules)
    attr_arr, attr_refs = _make_attribute_rules(attribute_rules)
    prop_arr, prop_refs = _make_property_rules(property_rules)
    meta_arr, meta_refs = _make_metadata_rules(metadata_rules)

    desc = _b.ovstage_population_export_desc_t()
    if not hasattr(lib, "ovstage_population_export_desc_init_snapshot"):
        raise OvstageError(
            _b.OVSTAGE_ERROR_NOT_SUPPORTED,
            "libovstage was built without the USD export snapshot descriptor initializer",
        )
    code = lib.ovstage_population_export_desc_init_snapshot(
        ctypes.byref(desc),
        check_ordinal(ordinal),
    )
    if code != _b.OVSTAGE_OK:
        raise OvstageError(code, _last_error())
    desc.since_ordinal = check_uint64(since_ordinal, "since_ordinal")
    desc.selection = check_enum_member(selection, _ExportSelection, "selection")
    desc.layer_mode = check_enum_member(layer_mode, _ExportLayerMode, "layer_mode")
    desc.transform_policy = check_enum_member(transform_policy, _TransformPolicy, "transform_policy")
    desc.unknown_metadata_policy = check_enum_member(
        unknown_metadata_policy, _UnknownMetadataPolicy, "unknown_metadata_policy"
    )
    desc.source_api_schema_policy = check_enum_member(
        source_api_schema_policy,
        SourceApiSchemaPolicy,
        "source_api_schema_policy",
    )
    desc.projection = check_enum_member(projection, ExportProjection, "projection")
    desc.prim_predicate = _build_prim_predicate(prim_predicate, predicate_refs, predicate_memo)
    desc.property_predicate = _build_property_predicate(property_predicate, predicate_refs, predicate_memo)
    if prim_arr is not None:
        desc.prim_rules = prim_arr
        desc.prim_rule_count = len(prim_arr)
    if api_arr is not None:
        desc.api_schema_rules = api_arr
        desc.api_schema_rule_count = len(api_arr)
    if attr_arr is not None:
        desc.attribute_rules = attr_arr
        desc.attribute_rule_count = len(attr_arr)
    if prop_arr is not None:
        desc.property_rules = prop_arr
        desc.property_rule_count = len(prop_arr)
    if meta_arr is not None:
        desc.metadata_rules = meta_arr
        desc.metadata_rule_count = len(meta_arr)
    refs = [predicate_refs, prim_refs, api_refs, attr_refs, prop_refs, meta_refs]
    return desc, refs


def _export_to_usd_identifier(
    stage,
    identifier: str,
    c_function_name: str,
    public_function_name: str,
    ordinal: int,
    *,
    since_ordinal: int,
    selection: int,
    layer_mode: int,
    transform_policy: int,
    unknown_metadata_policy: int,
    source_api_schema_policy: int,
    projection: int,
    prim_predicate,
    property_predicate,
    prim_rules,
    api_schema_rules,
    attribute_rules,
    property_rules,
    metadata_rules,
) -> dict:
    lib, inst = _require_export(stage)
    if not hasattr(lib, c_function_name):
        raise OvstageError(
            _b.OVSTAGE_ERROR_NOT_SUPPORTED,
            f"libovstage was built without {public_function_name}",
        )
    desc, refs = _build_export_desc(
        lib,
        ordinal,
        since_ordinal=since_ordinal,
        selection=selection,
        layer_mode=layer_mode,
        transform_policy=transform_policy,
        unknown_metadata_policy=unknown_metadata_policy,
        source_api_schema_policy=source_api_schema_policy,
        projection=projection,
        prim_predicate=prim_predicate,
        property_predicate=property_predicate,
        prim_rules=prim_rules,
        api_schema_rules=api_schema_rules,
        attribute_rules=attribute_rules,
        property_rules=property_rules,
        metadata_rules=metadata_rules,
    )
    identifier_s = _b.ovx_string_t(identifier)
    report = _b.ovstage_population_export_report_t()
    refs.append(identifier_s)
    result = getattr(lib, c_function_name)(inst, identifier_s, ctypes.byref(desc), ctypes.byref(report))
    if result != _b.OVSTAGE_OK:
        raise OvstageError(result, f"{public_function_name} failed: {_last_error()}")
    return _report_to_dict(report)


def _enqueue_to_usd_identifier(
    stage,
    identifier: str,
    c_function_name: str,
    public_function_name: str,
    ordinal: int,
    **options,
) -> ExportOperation:
    lib, inst = _require_export(stage)
    if not hasattr(lib, c_function_name):
        raise OvstageError(
            _b.OVSTAGE_ERROR_NOT_SUPPORTED,
            f"libovstage was built without {public_function_name}",
        )

    desc, refs = _build_export_desc(lib, ordinal, **options)
    identifier_s = _b.ovx_string_t(identifier)
    refs.append(identifier_s)
    result = getattr(lib, c_function_name)(
        inst,
        identifier_s,
        ctypes.byref(desc),
    )
    return _enqueue_export_operation(
        stage,
        lib,
        result,
        name=public_function_name,
    )


def _typed_hierarchy_options(
    root_path: str,
    *,
    prim_predicate: Optional[PrimPredicate],
    property_predicate: Optional[PropertyPredicate],
    include_custom_namespaces,
):
    if not isinstance(root_path, str):
        raise TypeError("root_path must be a str")
    if not root_path.startswith("/"):
        raise ValueError("root_path must be an absolute prim path")

    root_predicate = PrimPredicate.is_under_path(root_path)
    if prim_predicate is not None:
        root_predicate = PrimPredicate.and_(
            root_predicate,
            _check_predicate(prim_predicate, PrimPredicate),
        )

    if property_predicate is not None:
        property_predicate = _check_predicate(property_predicate, PropertyPredicate)

    custom_rules = []
    reserved_namespace_roots = {"omni", "usd"}
    if include_custom_namespaces is not None:
        if isinstance(include_custom_namespaces, str):
            raise TypeError("include_custom_namespaces must be a sequence of str")
        for namespace in include_custom_namespaces:
            if not isinstance(namespace, str):
                raise TypeError("include_custom_namespaces entries must be str")
            segments = namespace.split(":")
            if (
                not namespace
                or any(
                    not segment
                    or not (segment[0].isalpha() or segment[0] == "_")
                    or not all(character.isalnum() or character == "_" for character in segment[1:])
                    for segment in segments
                )
            ):
                raise ValueError("custom namespaces must contain valid USD identifier segments")
            if segments[0] in reserved_namespace_roots:
                raise ValueError("custom namespaces cannot select reserved ovstage namespaces")
            custom_rules.append(
                {
                    "source_attribute_name": f"{namespace}:*",
                    "source_attribute_name_match": OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB,
                    "flags": OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE,
                }
            )

    return root_predicate, property_predicate, custom_rules


def _export_typed_hierarchy(
    export_function,
    stage,
    target,
    root_path: str,
    ordinal: int,
    *,
    prim_predicate: Optional[PrimPredicate],
    property_predicate: Optional[PropertyPredicate],
    include_custom_namespaces,
    transform_policy: int,
    unknown_metadata_policy: int,
):
    selected_prims, selected_properties, custom_rules = _typed_hierarchy_options(
        root_path,
        prim_predicate=prim_predicate,
        property_predicate=property_predicate,
        include_custom_namespaces=include_custom_namespaces,
    )
    return export_function(
        stage,
        target,
        ordinal,
        layer_mode=OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF,
        transform_policy=transform_policy,
        unknown_metadata_policy=unknown_metadata_policy,
        source_api_schema_policy=OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_APPLY_RECORDED,
        projection=OVSTAGE_POPULATION_EXPORT_PROJECTION_SCHEMA_DECLARED,
        prim_predicate=selected_prims,
        property_predicate=selected_properties,
        attribute_rules=custom_rules,
    )


def export_typed_hierarchy_to_usd_file(
    stage,
    identifier: str,
    root_path: str,
    ordinal: int,
    *,
    prim_predicate: Optional[PrimPredicate] = None,
    property_predicate: Optional[PropertyPredicate] = None,
    include_custom_namespaces=None,
    transform_policy: int = OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP,
    unknown_metadata_policy: int = OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP,
) -> dict:
    """Export a typed runtime hierarchy and persist it as a replacement USD file."""
    return _export_typed_hierarchy(
        export_to_usd_file,
        stage,
        identifier,
        root_path,
        ordinal,
        prim_predicate=prim_predicate,
        property_predicate=property_predicate,
        include_custom_namespaces=include_custom_namespaces,
        transform_policy=transform_policy,
        unknown_metadata_policy=unknown_metadata_policy,
    )


def export_typed_hierarchy_to_usd_file_async(
    stage,
    identifier: str,
    root_path: str,
    ordinal: int,
    *,
    prim_predicate: Optional[PrimPredicate] = None,
    property_predicate: Optional[PropertyPredicate] = None,
    include_custom_namespaces=None,
    transform_policy: int = OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP,
    unknown_metadata_policy: int = OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP,
) -> ExportOperation:
    """Enqueue typed-hierarchy export and persistence as one export operation."""
    return _export_typed_hierarchy(
        export_to_usd_file_async,
        stage,
        identifier,
        root_path,
        ordinal,
        prim_predicate=prim_predicate,
        property_predicate=property_predicate,
        include_custom_namespaces=include_custom_namespaces,
        transform_policy=transform_policy,
        unknown_metadata_policy=unknown_metadata_policy,
    )


def create_export_destination(
    stage,
    identifier: str,
    mode: int = ExportDestinationMode.EMPTY,
) -> ExportDestination:
    """Create a reusable destination and wait until it is ready."""
    return ExportDestination.create(stage, identifier, mode)


def create_export_destination_async(
    stage,
    identifier: str,
    mode: int = ExportDestinationMode.EMPTY,
) -> DestinationCreateOperation:
    """Enqueue destination creation and expose its reserved handle immediately."""
    return ExportDestination.create_async(stage, identifier, mode)


def export_to_destination(
    stage,
    destination: ExportDestination,
    ordinal: int,
    *,
    since_ordinal: int = 0,
    selection: int = OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE,
    layer_mode: int = OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER,
    transform_policy: int = OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP,
    unknown_metadata_policy: int = OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP,
    source_api_schema_policy: int = OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY,
    projection: int = OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES,
    prim_predicate: Optional[PrimPredicate] = None,
    property_predicate: Optional[PropertyPredicate] = None,
    prim_rules: Optional[list[dict]] = None,
    api_schema_rules: Optional[list[dict]] = None,
    attribute_rules: Optional[list] = None,
    property_rules: Optional[list[dict]] = None,
    metadata_rules: Optional[list[dict]] = None,
) -> dict:
    """Accumulate one selected runtime slice in a reusable destination."""
    if not isinstance(destination, ExportDestination):
        raise TypeError("destination must be an ExportDestination")
    if destination._stage is not stage:
        raise ValueError("export destination belongs to a different source stage")
    destination._require_open()
    lib, inst = _require_destination_symbols(stage)
    desc, _refs = _build_export_desc(
        lib,
        ordinal,
        since_ordinal=since_ordinal,
        selection=selection,
        layer_mode=layer_mode,
        transform_policy=transform_policy,
        unknown_metadata_policy=unknown_metadata_policy,
        source_api_schema_policy=source_api_schema_policy,
        projection=projection,
        prim_predicate=prim_predicate,
        property_predicate=property_predicate,
        prim_rules=prim_rules,
        api_schema_rules=api_schema_rules,
        attribute_rules=attribute_rules,
        property_rules=property_rules,
        metadata_rules=metadata_rules,
    )
    report = _b.ovstage_population_export_report_t()
    code = lib.ovstage_population_export_to_destination(
        inst, destination.handle, ctypes.byref(desc), ctypes.byref(report)
    )
    if code != _b.OVSTAGE_OK:
        raise OvstageError(code, f"export_to_destination failed: {_last_error()}")
    return _report_to_dict(report)


def export_to_destination_async(
    stage,
    destination: ExportDestination,
    ordinal: int,
    *,
    since_ordinal: int = 0,
    selection: int = OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE,
    layer_mode: int = OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER,
    transform_policy: int = OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP,
    unknown_metadata_policy: int = OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP,
    source_api_schema_policy: int = OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY,
    projection: int = OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES,
    prim_predicate: Optional[PrimPredicate] = None,
    property_predicate: Optional[PropertyPredicate] = None,
    prim_rules: Optional[list[dict]] = None,
    api_schema_rules: Optional[list[dict]] = None,
    attribute_rules: Optional[list] = None,
    property_rules: Optional[list[dict]] = None,
    metadata_rules: Optional[list[dict]] = None,
) -> ExportOperation:
    """Enqueue one selected runtime slice into a reusable destination."""
    if not isinstance(destination, ExportDestination):
        raise TypeError("destination must be an ExportDestination")
    if destination._stage is not stage:
        raise ValueError("export destination belongs to a different source stage")
    destination._require_open()
    lib, inst = _require_destination_symbols(stage)
    desc, _refs = _build_export_desc(
        lib,
        ordinal,
        since_ordinal=since_ordinal,
        selection=selection,
        layer_mode=layer_mode,
        transform_policy=transform_policy,
        unknown_metadata_policy=unknown_metadata_policy,
        source_api_schema_policy=source_api_schema_policy,
        projection=projection,
        prim_predicate=prim_predicate,
        property_predicate=property_predicate,
        prim_rules=prim_rules,
        api_schema_rules=api_schema_rules,
        attribute_rules=attribute_rules,
        property_rules=property_rules,
        metadata_rules=metadata_rules,
    )
    result = lib.ovstage_population_export_enqueue_to_destination(inst, destination.handle, ctypes.byref(desc))
    return _enqueue_export_operation(stage, lib, result, name="export_to_destination_async")


def export_to_usd_file(
    stage,
    identifier: str,
    ordinal: int,
    *,
    since_ordinal: int = 0,
    selection: int = OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE,
    layer_mode: int = OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER,
    transform_policy: int = OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP,
    unknown_metadata_policy: int = OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP,
    source_api_schema_policy: int = OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY,
    projection: int = OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES,
    prim_predicate: Optional[PrimPredicate] = None,
    property_predicate: Optional[PropertyPredicate] = None,
    prim_rules: Optional[list[dict]] = None,
    api_schema_rules: Optional[list[dict]] = None,
    attribute_rules: Optional[list] = None,
    property_rules: Optional[list[dict]] = None,
    metadata_rules: Optional[list[dict]] = None,
) -> dict:
    """Export one selected snapshot and persist it as a replacement USD file."""
    return _export_to_usd_identifier(
        stage,
        str(identifier),
        "ovstage_population_export_to_usd_file",
        "export_to_usd_file",
        ordinal,
        since_ordinal=since_ordinal,
        selection=selection,
        layer_mode=layer_mode,
        transform_policy=transform_policy,
        unknown_metadata_policy=unknown_metadata_policy,
        source_api_schema_policy=source_api_schema_policy,
        projection=projection,
        prim_predicate=prim_predicate,
        property_predicate=property_predicate,
        prim_rules=prim_rules,
        api_schema_rules=api_schema_rules,
        attribute_rules=attribute_rules,
        property_rules=property_rules,
        metadata_rules=metadata_rules,
    )


def export_to_usd_file_async(
    stage,
    identifier: str,
    ordinal: int,
    *,
    since_ordinal: int = 0,
    selection: int = OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE,
    layer_mode: int = OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER,
    transform_policy: int = OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP,
    unknown_metadata_policy: int = OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP,
    source_api_schema_policy: int = OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY,
    projection: int = OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES,
    prim_predicate: Optional[PrimPredicate] = None,
    property_predicate: Optional[PropertyPredicate] = None,
    prim_rules: Optional[list[dict]] = None,
    api_schema_rules: Optional[list[dict]] = None,
    attribute_rules: Optional[list] = None,
    property_rules: Optional[list[dict]] = None,
    metadata_rules: Optional[list[dict]] = None,
) -> ExportOperation:
    """Enqueue one selected snapshot export and persistence operation."""
    return _enqueue_to_usd_identifier(
        stage,
        str(identifier),
        "ovstage_population_export_enqueue_to_usd_file",
        "export_to_usd_file_async",
        ordinal,
        since_ordinal=since_ordinal,
        selection=selection,
        layer_mode=layer_mode,
        transform_policy=transform_policy,
        unknown_metadata_policy=unknown_metadata_policy,
        source_api_schema_policy=source_api_schema_policy,
        projection=projection,
        prim_predicate=prim_predicate,
        property_predicate=property_predicate,
        prim_rules=prim_rules,
        api_schema_rules=api_schema_rules,
        attribute_rules=attribute_rules,
        property_rules=property_rules,
        metadata_rules=metadata_rules,
    )
