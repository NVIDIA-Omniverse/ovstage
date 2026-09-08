# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.
"""Population descriptions: predicates, selectors and descs.

A predicate is built with a constructor per kind and is immutable once built, so a
tree can be shared between selectors and reused across calls. Nothing is converted to
C until a desc is passed to an entry point, and the conversion returns a keepalive
that owns every buffer the C structs point at.

That ownership is the whole reason this layer exists. The C predicate structs
*reference* their subpredicates and string values rather than copying them, so a C
caller has to keep each nested node and each string alive until the call returns.
Here the tree owns its children as ordinary Python objects and the conversion roots
every ctypes buffer in one list, so a caller cannot build a description whose parts
outlive their storage.
"""

from __future__ import annotations

import ctypes
from enum import IntEnum
from typing import Dict, List, Sequence, Tuple

from . import bindings as _b
from .types import (
    PopulationDomain,
    PrimPredicateKind,
    PropertyPredicateKind,
    check_domains,
    check_enum_member,
)

__all__ = [
    "PrimPredicate",
    "PropertyPredicate",
    "Selector",
    "Desc",
]


def _check_str(value, name: str) -> str:
    if not isinstance(value, str):
        raise TypeError(f"{name} must be a str, got {type(value).__name__}")
    if not value:
        raise ValueError(f"{name} must not be empty")
    return value


def _check_str_sequence(values, name: str) -> Tuple[str, ...]:
    if isinstance(values, str):
        raise TypeError(f"{name} must be a sequence of str, not a single str")
    out = tuple(_check_str(v, name) for v in values)
    if not out:
        raise ValueError(f"at least one {name} is required")
    return out


class _Immutable:
    __slots__ = ()

    """Assign each attribute once, in __init__, and refuse every write after that.

    Construction is where these types are validated, so a later write would bypass the
    check entirely -- ``p.strings = "Mesh"`` is the same defect the constructor refuses,
    and ``p.subpredicates = (p,)`` builds a cycle nothing downstream can see.
    """

    def __setattr__(self, name, value):
        if hasattr(self, name):
            raise AttributeError(f"{type(self).__name__} is immutable")
        object.__setattr__(self, name, value)

    def __delattr__(self, name):
        raise AttributeError(f"{type(self).__name__} is immutable")


class _Predicate(_Immutable):
    """One node of a predicate tree: a kind plus whatever that kind matches against.

    Exactly one of ``strings`` / ``subpredicates`` / ``nested`` is populated, chosen by
    the kind, mirroring the C union.
    """

    __slots__ = ("kind", "strings", "subpredicates", "nested")

    #: The kind enum this predicate type is built from; set by each subclass.
    _KIND_ENUM = None

    #: Whether any kind of this type nests the other predicate type.
    _NESTS = False

    #: Which `values` member each kind names, mirroring the C union; `None` for a kind
    #: that takes no values. Set by each subclass, and checked so that `kind` and the
    #: populated slot cannot disagree: the C side reads the union member `kind` names,
    #: so a mismatch there reinterprets one predicate type as another.
    _KIND_SLOTS: dict = {}

    def __init__(self, kind, strings=(), subpredicates=(), nested=None):
        enum_type = type(self)._KIND_ENUM
        if isinstance(kind, IntEnum) and not isinstance(kind, enum_type):
            raise TypeError(
                f"{type(self).__name__} kind must be a {enum_type.__name__}, got {type(kind).__name__}"
            )
        self.kind = enum_type(check_enum_member(kind, enum_type, f"{type(self).__name__} kind"))
        self.strings = _check_str_sequence(strings, "value") if strings else ()
        self.subpredicates = _check_predicates(subpredicates, type(self)) if subpredicates else ()
        if nested is not None and not type(self)._NESTS:
            raise TypeError(f"no {type(self).__name__} kind nests another predicate")
        if nested is not None and (not isinstance(nested, _Predicate) or isinstance(nested, type(self))):
            raise TypeError(
                f"{type(self).__name__} nests the other predicate type, got {type(nested).__name__}"
            )
        self.nested = nested
        self._check_kind_slot()

    def _check_kind_slot(self) -> None:
        """Reject a predicate whose populated slot is not the one its kind names."""
        try:
            expected = type(self)._KIND_SLOTS[self.kind]
        except KeyError:  # a kind added to the enum without a slot entry
            raise TypeError(
                f"{type(self).__name__}.{self.kind.name} declares no values slot"
            ) from None
        filled = tuple(name for name in ("strings", "subpredicates", "nested") if getattr(self, name))
        if filled == ((expected,) if expected else ()):
            return
        got = ", ".join(filled) if filled else "nothing"
        takes = expected if expected else "no values"
        raise TypeError(f"{type(self).__name__}.{self.kind.name} takes {takes}, got {got}")

    def __repr__(self) -> str:
        if self.strings:
            detail = ", ".join(repr(s) for s in self.strings)
        elif self.subpredicates:
            detail = ", ".join(repr(p) for p in self.subpredicates)
        elif self.nested is not None:
            detail = repr(self.nested)
        else:
            detail = ""
        return f"{type(self).__name__}.{self.kind.name}({detail})"


class PrimPredicate(_Predicate):
    """Which prims a selector puts in scope.

    Several values of one kind match if any one of them does, so a list of schema
    names is a single predicate; combine different kinds with :meth:`and_`,
    :meth:`or_` and :meth:`not_`.
    """

    _KIND_ENUM = PrimPredicateKind
    _NESTS = True  # HAS_PROPERTY nests a property predicate
    _KIND_SLOTS = {
        PrimPredicateKind.NONE: None,
        PrimPredicateKind.ALL: None,
        PrimPredicateKind.AND: "subpredicates",
        PrimPredicateKind.OR: "subpredicates",
        PrimPredicateKind.NOT: "subpredicates",
        PrimPredicateKind.HAS_PARENT: "subpredicates",
        PrimPredicateKind.HAS_ANCESTOR: "subpredicates",
        PrimPredicateKind.HAS_PROPERTY: "nested",
        PrimPredicateKind.HAS_TYPE: "strings",
        PrimPredicateKind.IS_A_TYPE: "strings",
        PrimPredicateKind.HAS_SCHEMA: "strings",
        PrimPredicateKind.HAS_APPLIED_SCHEMA: "strings",
        PrimPredicateKind.HAS_APPLIED_SCHEMA_IN_NAMESPACE: "strings",
        PrimPredicateKind.HAS_PATH: "strings",
        PrimPredicateKind.IS_UNDER_PATH: "strings",
        PrimPredicateKind.HAS_KIND: "strings",
        PrimPredicateKind.HAS_PURPOSE: "strings",
        PrimPredicateKind.HAS_METADATA: "strings",
    }

    @staticmethod
    def none() -> "PrimPredicate":
        """Matches no prim. A selector built on it contributes nothing."""
        return PrimPredicate(PrimPredicateKind.NONE)

    @staticmethod
    def all() -> "PrimPredicate":
        """Matches every prim."""
        return PrimPredicate(PrimPredicateKind.ALL)

    @staticmethod
    def and_(*subpredicates: "PrimPredicate") -> "PrimPredicate":
        """Matches when every subpredicate matches."""
        return PrimPredicate(PrimPredicateKind.AND, subpredicates=_check_predicates(subpredicates, PrimPredicate))

    @staticmethod
    def or_(*subpredicates: "PrimPredicate") -> "PrimPredicate":
        """Matches when any subpredicate matches."""
        return PrimPredicate(PrimPredicateKind.OR, subpredicates=_check_predicates(subpredicates, PrimPredicate))

    @staticmethod
    def not_(subpredicate: "PrimPredicate") -> "PrimPredicate":
        """Matches when ``subpredicate`` does not."""
        return PrimPredicate(PrimPredicateKind.NOT, subpredicates=_check_predicates((subpredicate,), PrimPredicate))

    @staticmethod
    def has_parent(subpredicate: "PrimPredicate") -> "PrimPredicate":
        """Matches when the prim's parent matches ``subpredicate``.

        A top-level prim has no parent to match: the pseudo-root is not offered to a
        predicate, so this is false for it.
        """
        return PrimPredicate(
            PrimPredicateKind.HAS_PARENT, subpredicates=_check_predicates((subpredicate,), PrimPredicate)
        )

    @staticmethod
    def has_ancestor(subpredicate: "PrimPredicate") -> "PrimPredicate":
        """Matches when any strict ancestor matches. A prim is not its own ancestor."""
        return PrimPredicate(
            PrimPredicateKind.HAS_ANCESTOR, subpredicates=_check_predicates((subpredicate,), PrimPredicate)
        )

    @staticmethod
    def has_property(subpredicate: "PropertyPredicate") -> "PrimPredicate":
        """Matches when any property of the prim matches ``subpredicate``.

        The properties considered are the union of what the prim authors and what its
        schemas declare, so an unauthored schema property counts; combine with
        :meth:`PropertyPredicate.is_authored` for "carries an authored value".
        """
        return PrimPredicate(
            PrimPredicateKind.HAS_PROPERTY, nested=_check_predicate(subpredicate, PropertyPredicate)
        )

    @staticmethod
    def has_type(*type_names: str) -> "PrimPredicate":
        """Matches the prim's type name exactly."""
        return PrimPredicate(PrimPredicateKind.HAS_TYPE, strings=_check_str_sequence(type_names, "type name"))

    @staticmethod
    def is_a_type(*type_names: str) -> "PrimPredicate":
        """Matches the prim's type name or any type it derives from."""
        return PrimPredicate(PrimPredicateKind.IS_A_TYPE, strings=_check_str_sequence(type_names, "type name"))

    @staticmethod
    def has_schema(*schema_names: str) -> "PrimPredicate":
        """Matches a schema the prim conforms to: its type or an applied API schema.

        The form to use when a list of schema names mixes the two, which a schema
        family generally does.
        """
        return PrimPredicate(PrimPredicateKind.HAS_SCHEMA, strings=_check_str_sequence(schema_names, "schema name"))

    @staticmethod
    def has_applied_schema(*schema_names: str) -> "PrimPredicate":
        """Matches an applied API schema exactly, including any ``:instance`` suffix."""
        return PrimPredicate(
            PrimPredicateKind.HAS_APPLIED_SCHEMA, strings=_check_str_sequence(schema_names, "schema name")
        )

    @staticmethod
    def has_applied_schema_in_namespace(*family_names: str) -> "PrimPredicate":
        """Matches every instance of a multiple-apply schema family."""
        return PrimPredicate(
            PrimPredicateKind.HAS_APPLIED_SCHEMA_IN_NAMESPACE,
            strings=_check_str_sequence(family_names, "family name"),
        )

    @staticmethod
    def has_path(*paths: str) -> "PrimPredicate":
        """Matches the prim's absolute path exactly."""
        return PrimPredicate(PrimPredicateKind.HAS_PATH, strings=_check_str_sequence(paths, "path"))

    @staticmethod
    def is_under_path(*paths: str) -> "PrimPredicate":
        """Matches a prim at or beneath an absolute path, at path-component boundaries."""
        return PrimPredicate(PrimPredicateKind.IS_UNDER_PATH, strings=_check_str_sequence(paths, "path"))

    @staticmethod
    def has_kind(*kinds: str) -> "PrimPredicate":
        """Matches the prim's model kind exactly; the kind hierarchy is not consulted."""
        return PrimPredicate(PrimPredicateKind.HAS_KIND, strings=_check_str_sequence(kinds, "kind"))

    @staticmethod
    def has_metadata(*paths: str) -> "PrimPredicate":
        """Matches a prim resolving one of the named metadata paths.

        Paths take the form :class:`Desc` metadata paths do: a field name, extended with
        a ``:``-joined key path to reach inside a dictionary field. Presence, not value:
        a path resolving to ``False`` matches as much as one resolving to ``True``.

        Name a key, not a whole dictionary field: a field matches whenever anything
        resolves beneath it, including what a schema registers rather than the prim
        authoring it. ``has_metadata("customData:myTool:export")`` selects the prims
        carrying that key.
        """
        return PrimPredicate(PrimPredicateKind.HAS_METADATA, strings=_check_str_sequence(paths, "metadata path"))

    @staticmethod
    def has_purpose(*purposes: str) -> "PrimPredicate":
        """Matches the prim's purpose, as inherited down the tree."""
        return PrimPredicate(PrimPredicateKind.HAS_PURPOSE, strings=_check_str_sequence(purposes, "purpose"))


class PropertyPredicate(_Predicate):
    """Which of an in-scope prim's properties a selector publishes."""

    _KIND_ENUM = PropertyPredicateKind
    _KIND_SLOTS = {
        PropertyPredicateKind.NONE: None,
        PropertyPredicateKind.ALL: None,
        PropertyPredicateKind.AND: "subpredicates",
        PropertyPredicateKind.OR: "subpredicates",
        PropertyPredicateKind.NOT: "subpredicates",
        PropertyPredicateKind.DECLARED_BY_SCHEMA: "strings",
        PropertyPredicateKind.HAS_NAME: "strings",
        PropertyPredicateKind.IN_NAMESPACE: "strings",
        PropertyPredicateKind.HAS_METADATA: "strings",
        PropertyPredicateKind.IS_ATTRIBUTE: None,
        PropertyPredicateKind.IS_RELATIONSHIP: None,
        PropertyPredicateKind.IS_CUSTOM: None,
        PropertyPredicateKind.IS_AUTHORED: None,
    }

    @staticmethod
    def none() -> "PropertyPredicate":
        """Matches no property. The prim is still populated, with its reserved values."""
        return PropertyPredicate(PropertyPredicateKind.NONE)

    @staticmethod
    def all() -> "PropertyPredicate":
        """Matches every property of an in-scope prim."""
        return PropertyPredicate(PropertyPredicateKind.ALL)

    @staticmethod
    def and_(*subpredicates: "PropertyPredicate") -> "PropertyPredicate":
        """Matches when every subpredicate matches."""
        return PropertyPredicate(
            PropertyPredicateKind.AND, subpredicates=_check_predicates(subpredicates, PropertyPredicate)
        )

    @staticmethod
    def or_(*subpredicates: "PropertyPredicate") -> "PropertyPredicate":
        """Matches when any subpredicate matches."""
        return PropertyPredicate(
            PropertyPredicateKind.OR, subpredicates=_check_predicates(subpredicates, PropertyPredicate)
        )

    @staticmethod
    def not_(subpredicate: "PropertyPredicate") -> "PropertyPredicate":
        """Matches when ``subpredicate`` does not."""
        return PropertyPredicate(
            PropertyPredicateKind.NOT, subpredicates=_check_predicates((subpredicate,), PropertyPredicate)
        )

    @staticmethod
    def declared_by_schema(*schema_names: str) -> "PropertyPredicate":
        """Matches a property whose name one of the named schemas declares.

        Whether the prim carrying the property has that schema applied is not
        considered. A name is tried as both a type and an applied API schema, so a
        mixed list needs no partitioning.
        """
        return PropertyPredicate(
            PropertyPredicateKind.DECLARED_BY_SCHEMA, strings=_check_str_sequence(schema_names, "schema name")
        )

    @staticmethod
    def has_name(*names: str) -> "PropertyPredicate":
        """Matches the property's name exactly."""
        return PropertyPredicate(PropertyPredicateKind.HAS_NAME, strings=_check_str_sequence(names, "property name"))

    @staticmethod
    def in_namespace(*namespaces: str) -> "PropertyPredicate":
        """Matches a property in a namespace, at ``:`` boundaries.

        ``material:binding`` matches ``material:binding:physics`` but not
        ``material:bindingStrength``, and not a property named ``material:binding``
        itself -- a name is *in* a namespace rather than equal to it.
        """
        return PropertyPredicate(
            PropertyPredicateKind.IN_NAMESPACE, strings=_check_str_sequence(namespaces, "namespace")
        )

    @staticmethod
    def has_metadata(*paths: str) -> "PropertyPredicate":
        """Matches a property resolving one of the named metadata paths.

        Same path form and presence semantics as :meth:`PrimPredicate.has_metadata`,
        against the property's own metadata.
        """
        return PropertyPredicate(
            PropertyPredicateKind.HAS_METADATA, strings=_check_str_sequence(paths, "metadata path")
        )

    @staticmethod
    def is_attribute() -> "PropertyPredicate":
        """Matches an attribute."""
        return PropertyPredicate(PropertyPredicateKind.IS_ATTRIBUTE)

    @staticmethod
    def is_relationship() -> "PropertyPredicate":
        """Matches a relationship."""
        return PropertyPredicate(PropertyPredicateKind.IS_RELATIONSHIP)

    @staticmethod
    def is_custom() -> "PropertyPredicate":
        """Matches a property carrying USD's ``custom`` modifier."""
        return PropertyPredicate(PropertyPredicateKind.IS_CUSTOM)

    @staticmethod
    def is_authored() -> "PropertyPredicate":
        """Matches a property carrying an authored opinion rather than a schema fallback."""
        return PropertyPredicate(PropertyPredicateKind.IS_AUTHORED)


def _check_predicate(pred, expected):
    if not isinstance(pred, expected):
        raise TypeError(f"expected a {expected.__name__}, got {type(pred).__name__}")
    return pred


def _check_predicates(preds, expected) -> Tuple:
    if not preds:
        raise ValueError(f"{expected.__name__} combinator takes at least one subpredicate")
    return tuple(_check_predicate(p, expected) for p in preds)


class Selector(_Immutable):
    """One (which prims, which of their properties, which metadata) rule.

    Selectors compose: a prim is in scope if any selector matches it, and its
    properties are the union over the selectors that did.
    """

    __slots__ = ("prim_predicate", "property_predicate", "prim_metadata_paths", "property_metadata_paths")

    def __init__(self, prim_predicate=None, property_predicate=None,
                 prim_metadata_paths=(), property_metadata_paths=()):
        self.prim_predicate = (
            PrimPredicate.none() if prim_predicate is None else _check_predicate(prim_predicate, PrimPredicate)
        )
        self.property_predicate = (
            PropertyPredicate.none()
            if property_predicate is None
            else _check_predicate(property_predicate, PropertyPredicate)
        )
        self.prim_metadata_paths = _check_metadata_paths(prim_metadata_paths, "prim_metadata_paths")
        self.property_metadata_paths = _check_metadata_paths(property_metadata_paths, "property_metadata_paths")


class Desc(_Immutable):
    """What one contributor asks to be populated.

    Descs compose: ``domains`` is OR-ed across the array an entry point is given, and
    their selectors and stage metadata paths are combined.
    """

    __slots__ = ("domains", "selectors", "stage_metadata_paths")

    def __init__(self, domains: int = PopulationDomain.NONE, selectors=(), stage_metadata_paths=()):
        self.domains = check_domains(domains)
        self.selectors = tuple(_check_predicate(s, Selector) for s in selectors)
        self.stage_metadata_paths = _check_metadata_paths(stage_metadata_paths, "stage_metadata_paths")


def _check_metadata_paths(paths, name) -> Tuple[str, ...]:
    if isinstance(paths, str):
        raise TypeError(f"{name} must be a sequence of str, not a single str")
    return tuple(_check_str(p, f"{name} entry") for p in paths)


def _string_array(values: Sequence[str], keepalive: List):
    """One ``ovx_string_t[]`` over `values`, rooted in `keepalive`."""
    if not values:
        return None, 0
    views = [_b.ovx_string_t(v) for v in values]
    array = (_b.ovx_string_t * len(views))(*views)
    # The array copies the structs, but each view owns the encoded bytes its `ptr`
    # addresses -- so both the originals and the array have to outlive the call.
    keepalive.append(views)
    keepalive.append(array)
    return array, len(views)


def _build_prim_predicate(pred: PrimPredicate, keepalive: List, memo: Dict):
    # One C node per distinct predicate object. A predicate used in more than one place is
    # built once and its struct copied into each slot, so a shared subtree costs its own
    # size rather than one copy per path that reaches it. The memo holds the source object
    # so its id stays unique for as long as the memo does.
    built = memo.get(id(pred))
    if built is not None:
        return built[1]
    out = _b.ovstage_population_prim_predicate_t()
    out.kind = check_enum_member(pred.kind, PrimPredicateKind, "prim predicate kind")
    if pred.strings:
        array, count = _string_array(pred.strings, keepalive)
        out.values.strings = ctypes.cast(array, ctypes.POINTER(_b.ovx_string_t))
        out.value_count = count
    elif pred.subpredicates:
        children = [_build_prim_predicate(p, keepalive, memo) for p in pred.subpredicates]
        array = (_b.ovstage_population_prim_predicate_t * len(children))(*children)
        keepalive.append(array)
        out.values.subpredicates = ctypes.cast(array, ctypes.POINTER(_b.ovstage_population_prim_predicate_t))
        out.value_count = len(children)
    elif pred.nested is not None:
        nested = _build_property_predicate(pred.nested, keepalive, memo)
        keepalive.append(nested)
        out.values.property_predicate = ctypes.pointer(nested)
        out.value_count = 1
    else:
        out.values.strings = None
        out.value_count = 0
    memo[id(pred)] = (pred, out)
    return out


def _build_property_predicate(pred: PropertyPredicate, keepalive: List, memo: Dict):
    built = memo.get(id(pred))
    if built is not None:
        return built[1]
    out = _b.ovstage_population_property_predicate_t()
    out.kind = check_enum_member(pred.kind, PropertyPredicateKind, "property predicate kind")
    if pred.strings:
        array, count = _string_array(pred.strings, keepalive)
        out.values.strings = ctypes.cast(array, ctypes.POINTER(_b.ovx_string_t))
        out.value_count = count
    elif pred.subpredicates:
        children = [_build_property_predicate(p, keepalive, memo) for p in pred.subpredicates]
        array = (_b.ovstage_population_property_predicate_t * len(children))(*children)
        keepalive.append(array)
        out.values.subpredicates = ctypes.cast(array, ctypes.POINTER(_b.ovstage_population_property_predicate_t))
        out.value_count = len(children)
    else:
        out.values.strings = None
        out.value_count = 0
    memo[id(pred)] = (pred, out)
    return out


def build_descs(descs: Sequence[Desc]) -> Tuple:
    """Convert `descs` to a C array, with the keepalive that owns everything it points at.

    Returns ``(array, count, keepalive)``. The caller must hold `keepalive` for as long
    as the C side reads the array; every nested predicate, string buffer and selector
    array is rooted in it.
    """
    keepalive: List = []
    memo: Dict = {}
    built = []
    for desc in descs:
        cdesc = _b.ovstage_population_desc_t()
        cdesc.domains = desc.domains
        selectors = []
        for selector in desc.selectors:
            csel = _b.ovstage_population_selector_t()
            csel.prim_predicate = _build_prim_predicate(selector.prim_predicate, keepalive, memo)
            csel.property_predicate = _build_property_predicate(selector.property_predicate, keepalive, memo)
            prim_paths, prim_count = _string_array(selector.prim_metadata_paths, keepalive)
            csel.prim_metadata_paths = ctypes.cast(prim_paths, ctypes.POINTER(_b.ovx_string_t))
            csel.prim_metadata_path_count = prim_count
            prop_paths, prop_count = _string_array(selector.property_metadata_paths, keepalive)
            csel.property_metadata_paths = ctypes.cast(prop_paths, ctypes.POINTER(_b.ovx_string_t))
            csel.property_metadata_path_count = prop_count
            selectors.append(csel)
        if selectors:
            sel_array = (_b.ovstage_population_selector_t * len(selectors))(*selectors)
            keepalive.append(sel_array)
            cdesc.selectors = ctypes.cast(sel_array, ctypes.POINTER(_b.ovstage_population_selector_t))
            cdesc.selector_count = len(selectors)
        else:
            cdesc.selectors = None
            cdesc.selector_count = 0
        stage_paths, stage_count = _string_array(desc.stage_metadata_paths, keepalive)
        cdesc.stage_metadata_paths = ctypes.cast(stage_paths, ctypes.POINTER(_b.ovx_string_t))
        cdesc.stage_metadata_path_count = stage_count
        built.append(cdesc)

    if not built:
        return None, 0, keepalive  # the C entry point takes a NULL array with a count of 0
    array = (_b.ovstage_population_desc_t * len(built))(*built)
    keepalive.append(array)
    return array, len(built), keepalive
