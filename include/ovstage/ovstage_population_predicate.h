/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * ovstage population predicates: composable prim and property selection.
 *
 * Predicates are shared selection vocabulary. Population selectors and USD
 * export descriptions both embed them without depending on one another.
 */

#ifndef OVSTAGE_POPULATION_PREDICATE_H
#define OVSTAGE_POPULATION_PREDICATE_H

#include "ovstage_api/ovstage_api.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct ovstage_population_prim_predicate_t     ovstage_population_prim_predicate_t;
    typedef struct ovstage_population_property_predicate_t ovstage_population_property_predicate_t;

    /** How an `ovstage_population_prim_predicate_t` matches, and which member of its
     *  `values` union carries the `value_count` values it matches against.
     *
     *  A kind that takes values matches when any one of them matches: the values of a
     *  single predicate are a disjunction, never a conjunction. Requiring more than one
     *  condition is what `OVSTAGE_POPULATION_PRIM_PREDICATE_AND` is for. */
    typedef enum
    {
        /** Matches no prim. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_NONE = 0,
        /** Matches every prim. Takes no values. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_ALL = 1,
        /** `values.subpredicates`: matches when every subpredicate matches. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_AND = 2,
        /** `values.subpredicates`: matches when any subpredicate matches. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_OR = 3,
        /** `values.subpredicates`, exactly one: matches when it does not. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_NOT = 4,
        /** `values.subpredicates`, exactly one: matches when the prim's parent does. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PARENT = 5,
        /** `values.subpredicates`, exactly one: matches when any ancestor does. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_ANCESTOR = 6,
        /** `values.property_predicate`, exactly one: matches when any property of the
         *  prim matches it. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PROPERTY = 7,

        /** `values.strings`: the prim's type name, matched exactly. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_TYPE = 8,
        /** `values.strings`: the prim's type name, or any type it derives from. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_IS_A_TYPE = 9,
        /** `values.strings`: a schema the prim conforms to, matched exactly — its type
         *  name or one of its applied API schemas. The form to use when a list of schema
         *  names mixes the two, which a schema family generally does. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_SCHEMA = 10,
        /** `values.strings`: an API schema applied to the prim, matched exactly —
         *  including the `:instance` suffix a multiple-apply schema carries. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_APPLIED_SCHEMA = 11,
        /** `values.strings`: a namespace an applied API schema name is in, matched at
         *  `:` boundaries — `PhysxJointAxisAPI` matches every instance of it, such as
         *  `PhysxJointAxisAPI:linear`. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_APPLIED_SCHEMA_IN_NAMESPACE = 12,
        /** `values.strings`: the prim's absolute path, matched exactly. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PATH = 13,
        /** `values.strings`: an absolute prim path the prim's own path is at or
         *  beneath. Matched at path-component boundaries, so `/World/Env` does not
         *  match `/World/Environment`.
         *  Does not match a prim inside a native instance. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_IS_UNDER_PATH = 14,
        /** `values.strings`: the prim's model kind, matched exactly — a kind's
         *  ancestors in the kind hierarchy are not considered. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_KIND = 15,
        /** `values.strings`: the prim's purpose, as inherited down the tree — a prim
         *  under a `guide` scope has purpose `guide`. A prim that is not imageable has
         *  no purpose and matches nothing. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PURPOSE = 16,
        /** `values.strings`: a metadata path the prim resolves, in the same form
         *  `prim_metadata_paths` takes — a field name, extended with a `:`-joined key
         *  path to reach inside a dictionary field.
         *
         *  Presence, not value: a path resolving to `false` or `0` matches as much as
         *  one resolving to `true`.
         *
         *  Name a key, not a whole dictionary field. A field matches whenever anything
         *  resolves beneath it, including what a schema registers rather than the prim
         *  authoring it; `customData:myTool:export` selects the prims that carry that
         *  key. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_METADATA = 17,
    } ovstage_population_prim_predicate_kind_t;

    /** How an `ovstage_population_property_predicate_t` matches, and which member of
     *  its `values` union carries the `value_count` values it matches against.
     *
     *  A kind that takes values matches when any one of them matches: the values of a
     *  single predicate are a disjunction, never a conjunction. Requiring more than one
     *  condition is what `OVSTAGE_POPULATION_PROPERTY_PREDICATE_AND` is for. */
    typedef enum
    {
        /** Matches no property. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_NONE = 0,
        /** Matches every property of an in-scope prim. Takes no values. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_ALL = 1,
        /** `values.subpredicates`: matches when every subpredicate matches. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_AND = 2,
        /** `values.subpredicates`: matches when any subpredicate matches. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_OR = 3,
        /** `values.subpredicates`, exactly one: matches when it does not. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_NOT = 4,

        /** `values.strings`: the property's name is declared by one of the named
         *  schemas. Whether the prim carrying the property has that schema applied is
         *  not considered. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_DECLARED_BY_SCHEMA = 5,
        /** `values.strings`: the property's name, matched exactly. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_HAS_NAME = 6,
        /** `values.strings`: a namespace the property's name is in, matched at `:`
         *  boundaries — `material:binding` matches `material:binding:physics` but not
         *  `material:bindingStrength`. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_IN_NAMESPACE = 7,
        /** `values.strings`: a metadata path the property resolves, in the same form
         *  `property_metadata_paths` takes. Path form, presence, and the dictionary-field
         *  caveat are as for `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_METADATA`. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_HAS_METADATA = 8,
        /** Matches an attribute. Takes no values. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_ATTRIBUTE = 9,
        /** Matches a relationship. Takes no values. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_RELATIONSHIP = 10,
        /** Matches a property carrying USD's `custom` modifier, which marks ad hoc client
         *  data not formalized into a schema. Takes no values. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_CUSTOM = 11,
        /** Matches a property carrying an authored opinion, as opposed to one resolving
         *  to its schema fallback. Takes no values. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_AUTHORED = 12,
    } ovstage_population_property_predicate_kind_t;

    /** Which prims a selector puts in scope.
     *
     *  Zero-initialize, then set `kind`, the `values` member that `kind` names, and
     *  `value_count`. Several values of one kind match if any one of them does, so a
     *  list of schema names is a single predicate; combine different kinds with AND /
     *  OR / NOT. A property predicate nests here — as
     *  `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PROPERTY` — so a prim can be gated on the
     *  properties it carries. */
    struct ovstage_population_prim_predicate_t
    {
        ovstage_population_prim_predicate_kind_t kind;
        union
        {
            const ovx_string_t*                            strings;
            const ovstage_population_prim_predicate_t*     subpredicates;
            const ovstage_population_property_predicate_t* property_predicate;
        } values;
        size_t value_count;
    };

    /** Which of an in-scope prim's properties a selector publishes.
     *
     *  Built the same way as `ovstage_population_prim_predicate_t`: `kind` names the
     *  live `values` member, and several values of one kind match if any one of them
     *  does. */
    struct ovstage_population_property_predicate_t
    {
        ovstage_population_property_predicate_kind_t kind;
        union
        {
            const ovx_string_t*                            strings;
            const ovstage_population_property_predicate_t* subpredicates;
        } values;
        size_t value_count;
    };

    /** Predicate constructors — one per kind.
     *
     *  Each sets `kind`, the one `values` member that kind names, and `value_count`
     *  together, so no combination can be built wrongly. See the kind's enumerator for
     *  what it matches.
     *
     *  A nested predicate is referenced, not copied: the object passed to `_and`,
     *  `_or`, `_not`, `_has_parent`, `_has_ancestor`, or `_has_property` must outlive
     *  the predicate built from it. */

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_NONE`. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_none(void)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_NONE;
        predicate.values.strings = 0;
        predicate.value_count = 0;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_ALL`. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_all(void)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_ALL;
        predicate.values.strings = 0;
        predicate.value_count = 0;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_AND` over `value_count` subpredicates. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_and(
        const ovstage_population_prim_predicate_t* subpredicates, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_AND;
        predicate.values.subpredicates = subpredicates;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_OR` over `value_count` subpredicates. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_or(
        const ovstage_population_prim_predicate_t* subpredicates, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_OR;
        predicate.values.subpredicates = subpredicates;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_NOT` over one subpredicate. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_not(
        const ovstage_population_prim_predicate_t* subpredicate)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_NOT;
        predicate.values.subpredicates = subpredicate;
        predicate.value_count = 1;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PARENT` over one subpredicate. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_parent(
        const ovstage_population_prim_predicate_t* subpredicate)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PARENT;
        predicate.values.subpredicates = subpredicate;
        predicate.value_count = 1;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_ANCESTOR` over one subpredicate. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_ancestor(
        const ovstage_population_prim_predicate_t* subpredicate)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_ANCESTOR;
        predicate.values.subpredicates = subpredicate;
        predicate.value_count = 1;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PROPERTY` over one property predicate. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_property(
        const ovstage_population_property_predicate_t* property_predicate)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PROPERTY;
        predicate.values.property_predicate = property_predicate;
        predicate.value_count = 1;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_TYPE` over `value_count` type names. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_type(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_TYPE;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_IS_A_TYPE` over `value_count` type names. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_is_a_type(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_IS_A_TYPE;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_SCHEMA` over `value_count` schema names. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_schema(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_SCHEMA;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_APPLIED_SCHEMA` over `value_count` schema names. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_applied_schema(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_APPLIED_SCHEMA;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_APPLIED_SCHEMA_IN_NAMESPACE` over
     *  `value_count` namespaces. */
    static inline ovstage_population_prim_predicate_t
        ovstage_population_prim_predicate_has_applied_schema_in_namespace(
            const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_APPLIED_SCHEMA_IN_NAMESPACE;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PATH` over `value_count` paths. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_path(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PATH;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_IS_UNDER_PATH` over `value_count` paths. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_is_under_path(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_IS_UNDER_PATH;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_KIND` over `value_count` kinds. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_kind(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_KIND;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PURPOSE` over `value_count` purposes. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_purpose(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PURPOSE;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_METADATA` over `value_count` metadata
     *  paths. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_metadata(
        const ovx_string_t* paths, size_t path_count)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_METADATA;
        predicate.values.strings = paths;
        predicate.value_count = path_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_NONE`. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_none(void)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_NONE;
        predicate.values.strings = 0;
        predicate.value_count = 0;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_ALL`. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_all(void)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_ALL;
        predicate.values.strings = 0;
        predicate.value_count = 0;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_AND` over `value_count` subpredicates. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_and(
        const ovstage_population_property_predicate_t* subpredicates, size_t value_count)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_AND;
        predicate.values.subpredicates = subpredicates;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_OR` over `value_count` subpredicates. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_or(
        const ovstage_population_property_predicate_t* subpredicates, size_t value_count)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_OR;
        predicate.values.subpredicates = subpredicates;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_NOT` over one subpredicate. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_not(
        const ovstage_population_property_predicate_t* subpredicate)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_NOT;
        predicate.values.subpredicates = subpredicate;
        predicate.value_count = 1;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_DECLARED_BY_SCHEMA` over `value_count`
     *  schema names. */
    static inline ovstage_population_property_predicate_t
        ovstage_population_property_predicate_declared_by_schema(
            const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_DECLARED_BY_SCHEMA;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_HAS_NAME` over `value_count` names. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_has_name(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_HAS_NAME;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_IN_NAMESPACE` over `value_count` namespaces. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_in_namespace(
        const ovx_string_t* values, size_t value_count)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_IN_NAMESPACE;
        predicate.values.strings = values;
        predicate.value_count = value_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_HAS_METADATA` over `value_count` metadata
     *  paths. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_has_metadata(
        const ovx_string_t* paths, size_t path_count)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_HAS_METADATA;
        predicate.values.strings = paths;
        predicate.value_count = path_count;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_ATTRIBUTE`. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_is_attribute(void)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_ATTRIBUTE;
        predicate.values.strings = 0;
        predicate.value_count = 0;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_RELATIONSHIP`. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_is_relationship(void)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_RELATIONSHIP;
        predicate.values.strings = 0;
        predicate.value_count = 0;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_CUSTOM`. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_is_custom(void)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_CUSTOM;
        predicate.values.strings = 0;
        predicate.value_count = 0;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_AUTHORED`. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_is_authored(void)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_IS_AUTHORED;
        predicate.values.strings = 0;
        predicate.value_count = 0;
        return predicate;
    }

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_POPULATION_PREDICATE_H */
