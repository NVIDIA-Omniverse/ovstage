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
        /** `values.targeted_by`, with `value_count` 2: matches a prim that a relationship
         *  points at. The relationship must match `targeted_by.property_predicate`, and the
         *  prim carrying it must match `targeted_by.prim_predicate`. A target naming a property
         *  points at the prim that owns the property.
         *
         *  Only relationships on prims population walks count: active, defined, loaded and
         *  not abstract. A relationship inside a native instance counts once, as authored in
         *  the prototype, not once per instance.
         *
         *  It asks only about the prim being matched. It may be the whole prim predicate of a
         *  selector or a derived property, or sit inside `OVSTAGE_POPULATION_PRIM_PREDICATE_AND`,
         *  `OVSTAGE_POPULATION_PRIM_PREDICATE_OR` and `OVSTAGE_POPULATION_PRIM_PREDICATE_NOT`,
         *  at any depth. Anywhere else, including the prim predicate of an expression, enqueue
         *  fails with `OVSTAGE_ERROR_NOT_SUPPORTED`.
         *
         *  Its two predicates may not contain `IS_TARGETED_BY`,
         *  `OVSTAGE_POPULATION_PROPERTY_PREDICATE_TARGETS` or
         *  `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_DESCENDANT`. Enqueue fails with
         *  `OVSTAGE_ERROR_NOT_SUPPORTED` if they do.
         *
         *  After `apply_usd_changes`, a prim is selected as a fresh population would select
         *  it, including when the prims pointing at it or their relationships changed. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_IS_TARGETED_BY = 18,
        /** `values.subpredicates`, exactly one: matches when any descendant of the prim
         *  matches. The descendants are the prims population walks below the prim: active,
         *  defined, loaded and not abstract.
         *
         *  The walk crosses native instance boundaries. When the prim is an instance, its
         *  descendants are the prims of its prototype. When the walk reaches an instance below
         *  the prim, it continues into that instance's prototype, and the prims there are
         *  descendants of the prim as well.
         *
         *  A prim is not its own descendant; `OR` with the subpredicate to include the prim
         *  itself.
         *
         *  After `apply_usd_changes`, a prim is selected as a fresh population would select
         *  it, including after changes below it or inside the prototypes it reaches. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_DESCENDANT = 19,
        /** Takes no values: matches a native instance, a prim USD instances because it is
         *  `instanceable` and has composition arcs. An instance nested inside a prototype
         *  matches too. */
        OVSTAGE_POPULATION_PRIM_PREDICATE_IS_INSTANCE = 20,
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
        /** `values.prim_predicate`, exactly one: matches a relationship any of whose targets
         *  is a prim matching it. A target that names a property path counts as the prim
         *  carrying it, and a target population does not walk (inactive, undefined, abstract,
         *  unloaded or inside an instance) matches nothing. An attribute never matches, even
         *  one with connections: connections are not targets.
         *
         *  Accepted only inside a prim predicate, beneath
         *  `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PROPERTY`; anywhere else it is rejected at
         *  enqueue with `OVSTAGE_ERROR_NOT_SUPPORTED`. After `apply_usd_changes`, a prim is
         *  selected as a fresh population would select it, including after changes to its
         *  targets or to the prims above them. */
        OVSTAGE_POPULATION_PROPERTY_PREDICATE_TARGETS = 13,
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
            struct
            {
                const ovstage_population_prim_predicate_t*     prim_predicate;     /* the referrers */
                const ovstage_population_property_predicate_t* property_predicate; /* their relationships */
            } targeted_by;
        } values;
        size_t value_count;
    };

    /** Which of an in-scope prim's properties a selector includes in population.
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
            const ovstage_population_prim_predicate_t*     prim_predicate;
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
     *  `_or`, `_not`, `_has_parent`, `_has_ancestor`, `_has_descendant`, `_has_property`,
     *  `_is_targeted_by`, or `_targets` must outlive the predicate built from it. */

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

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_DESCENDANT` over one subpredicate. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_has_descendant(
        const ovstage_population_prim_predicate_t* subpredicate)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_DESCENDANT;
        predicate.values.subpredicates = subpredicate;
        predicate.value_count = 1;
        return predicate;
    }

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_IS_INSTANCE`. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_is_instance(void)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_IS_INSTANCE;
        predicate.values.strings = 0;
        predicate.value_count = 0;
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

    /** `OVSTAGE_POPULATION_PRIM_PREDICATE_IS_TARGETED_BY`: targeted by a relationship matching
     *  `relationship` on a prim matching `referrer`. */
    static inline ovstage_population_prim_predicate_t ovstage_population_prim_predicate_is_targeted_by(
        const ovstage_population_prim_predicate_t* referrer, const ovstage_population_property_predicate_t* relationship)
    {
        ovstage_population_prim_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PRIM_PREDICATE_IS_TARGETED_BY;
        predicate.values.targeted_by.prim_predicate = referrer;
        predicate.values.targeted_by.property_predicate = relationship;
        predicate.value_count = 2;
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

    /** `OVSTAGE_POPULATION_PROPERTY_PREDICATE_TARGETS` over one prim predicate. */
    static inline ovstage_population_property_predicate_t ovstage_population_property_predicate_targets(
        const ovstage_population_prim_predicate_t* target)
    {
        ovstage_population_property_predicate_t predicate;
        predicate.kind = OVSTAGE_POPULATION_PROPERTY_PREDICATE_TARGETS;
        predicate.values.prim_predicate = target;
        predicate.value_count = 1;
        return predicate;
    }

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_POPULATION_PREDICATE_H */
