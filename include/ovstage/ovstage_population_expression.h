/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * ovstage population expressions: the value of a derived property.
 *
 * An expression is a small tree that yields a value for the prim a derived property
 * (`ovstage_population.h`) is evaluated on. The kinds below compute from literals, the
 * stage's metadata, and what USD resolves for that prim: its bound material and its
 * collections. They read no property of the prim; when a rule applies is decided by the
 * rule itself (`ovstage_population_derive_when_t`). The set of kinds, and of the value
 * types they yield, is open: a derived column can have any type a column read off a prim
 * can have, whether or not a kind yields it yet.
 *
 * Built the same way as a predicate (`ovstage_population_predicate.h`): every node has a
 * kind, the `values` member that kind names, and a `value_count`. One constructor per
 * kind sets the three together. Nested expressions and predicates are referenced, not
 * copied, so every node must outlive the call that takes the description. Strings are
 * held by value.
 */

#ifndef OVSTAGE_POPULATION_EXPRESSION_H
#define OVSTAGE_POPULATION_EXPRESSION_H

#include "ovstage_api/ovstage_api.h"
#include "ovstage_population_predicate.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct ovstage_population_expression_t ovstage_population_expression_t;

    /** How an `ovstage_population_expression_t` node computes its value, and which member of
     *  its `values` union carries its operands.
     *
     *  The kinds below yield a **number** (a double), a **bool**, a **vector** (three numbers),
     *  a **token**, or **paths** (an ordered list of prim or property paths, possibly empty).
     *  The value converts to the derived column's type (`ovstage_population_derived_property_t`);
     *  a column whose type is not stated takes the value's own type, so a token publishes as a
     *  token and paths as a relationship.
     *
     *  An expression may also yield **nothing**, where the kind says so; the rule then
     *  publishes nothing for that prim. For paths, nothing and an empty list differ: an empty
     *  list is published as a relationship with no targets.
     *
     *  A type mismatch visible from the tree is rejected at enqueue. A value read from the
     *  stage that has the wrong type fails the evaluation for that prim
     *  (`ovstage_population_derived_property_t`). */
    typedef enum
    {
        /** Not a value: what a zero-initialized node means. Rejected at enqueue. */
        OVSTAGE_POPULATION_EXPRESSION_NONE = 0,

        /** `values.number`: a number literal. */
        OVSTAGE_POPULATION_EXPRESSION_NUMBER = 1,

        /** `values.boolean`: a bool literal. */
        OVSTAGE_POPULATION_EXPRESSION_BOOL = 2,

        /** `values.name`: a stage metadata field holding a number, such as `metersPerUnit`.
         *  Yields that number as USD resolves it, its fallback included. A field that does
         *  not hold a number fails the evaluation. */
        OVSTAGE_POPULATION_EXPRESSION_STAGE_METADATA = 3,

        /** Takes no operands: the unit vector along the stage's `upAxis`, `(0, 1, 0)` for
         *  `"Y"` and `(0, 0, 1)` for `"Z"`, and `(0, 1, 0)` for any other value. */
        OVSTAGE_POPULATION_EXPRESSION_UP_AXIS = 4,

        /** `values.operands[0], [1]`: `a + b`, between two numbers, or two vectors component
         *  by component. */
        OVSTAGE_POPULATION_EXPRESSION_ADD = 5,

        /** `values.operands[0], [1]`: `a - b`, between two numbers, or two vectors component
         *  by component. */
        OVSTAGE_POPULATION_EXPRESSION_SUBTRACT = 6,

        /** `values.operands[0], [1]`: `a * b`, between two numbers, or a number and a vector
         *  in either order (the vector scaled). */
        OVSTAGE_POPULATION_EXPRESSION_MULTIPLY = 7,

        /** `values.operands[0], [1]`: `a / b`, a number or a vector over a number. Division by
         *  zero yields the IEEE result, an infinity or NaN. */
        OVSTAGE_POPULATION_EXPRESSION_DIVIDE = 8,

        /** `values.resolved.name` a material purpose such as `physics`, or empty for the
         *  all-purpose binding, `values.resolved.prim_predicate` a prim predicate or `NULL`: the
         *  material bound to the prim for that purpose, as a one-element list.
         *  `UsdShadeMaterialBindingAPI::ComputeBoundMaterial` resolves the binding: direct,
         *  inherited down the namespace, or through a collection, falling back to the
         *  all-purpose binding. Nothing if no material is bound, or if the predicate is given
         *  and the material does not match it. */
        OVSTAGE_POPULATION_EXPRESSION_RESOLVED_BOUND_MATERIAL = 9,

        /** `values.resolved.name` a material purpose such as `physics`, or empty for the
         *  all-purpose binding, `values.resolved.prim_predicate` a prim predicate or `NULL`: the
         *  strength of the binding that binds the prim's material for that purpose, a token,
         *  `strongerThanDescendants` or `weakerThanDescendants`.
         *  `UsdShadeMaterialBindingAPI::ComputeBoundMaterial` resolves the material, so the
         *  winning binding can be the prim's own, an ancestor's or a collection's. Nothing if no
         *  material is bound, or if the predicate is given and the material does not match it. */
        OVSTAGE_POPULATION_EXPRESSION_RESOLVED_MATERIAL_BINDING_STRENGTH = 10,

        /** `values.resolved.name` a material purpose such as `physics`, or empty for the
         *  all-purpose binding, `values.resolved.prim_predicate` a prim predicate or `NULL`: the
         *  purpose the binding that binds the prim's material was authored for, a token: the
         *  purpose asked for, or `allPurpose` where
         *  `UsdShadeMaterialBindingAPI::ComputeBoundMaterial` fell back to an all-purpose binding.
         *  Nothing if no material is bound, or if the predicate is given and the material does
         *  not match it. */
        OVSTAGE_POPULATION_EXPRESSION_RESOLVED_MATERIAL_BINDING_PURPOSE = 11,

        /** `values.resolved.name` the `<name>` of a `CollectionAPI:<name>` applied to the
         *  prim, `values.resolved.prim_predicate` and `values.resolved.property_predicate` a
         *  prim and a property predicate, each or `NULL`: the collection's members as
         *  `UsdCollectionAPI::ComputeIncludedPaths` resolves them. Includes, excludes,
         *  `expansionRule` and `membershipExpression` are honoured, so `expandPrims` yields
         *  every descendant of an included prim, and `expandPrimsAndProperties` their
         *  properties too. A prim member is kept if it matches the prim predicate and a
         *  property member if it matches the property predicate; a `NULL` predicate keeps every
         *  member of its kind. The list may be empty. Nothing if the prim applies no such
         *  collection. */
        OVSTAGE_POPULATION_EXPRESSION_RESOLVED_COLLECTION_MEMBERS = 12,
    } ovstage_population_expression_kind_t;

    /** One node of an expression. Zero-initialize, then set `kind`, the `values` member that
     *  kind names, and `value_count`, or use the constructors below. Nested expressions and
     *  predicates are referenced, not copied; strings are held by value. */
    struct ovstage_population_expression_t
    {
        ovstage_population_expression_kind_t kind;
        union
        {
            double                                 number;
            bool                                   boolean;
            ovx_string_t                           name;
            const ovstage_population_expression_t* operands[2];
            struct
            {
                ovx_string_t                                   name;
                const ovstage_population_prim_predicate_t*     prim_predicate;     /* may be NULL */
                /* RESOLVED_COLLECTION_MEMBERS only; may be NULL */
                const ovstage_population_property_predicate_t* property_predicate;
            } resolved;
        } values;
        size_t value_count;
    };

    /** `OVSTAGE_POPULATION_EXPRESSION_NUMBER`. */
    static inline ovstage_population_expression_t ovstage_population_expression_number(double value)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_NUMBER;
        expression.values.number = value;
        expression.value_count = 1;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_BOOL`. */
    static inline ovstage_population_expression_t ovstage_population_expression_bool(bool value)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_BOOL;
        expression.values.boolean = value;
        expression.value_count = 1;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_STAGE_METADATA`. */
    static inline ovstage_population_expression_t ovstage_population_expression_stage_metadata(ovx_string_t field)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_STAGE_METADATA;
        expression.values.name = field;
        expression.value_count = 1;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_UP_AXIS`. */
    static inline ovstage_population_expression_t ovstage_population_expression_up_axis(void)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_UP_AXIS;
        expression.value_count = 0;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_ADD`. */
    static inline ovstage_population_expression_t ovstage_population_expression_add(
        const ovstage_population_expression_t* a, const ovstage_population_expression_t* b)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_ADD;
        expression.values.operands[0] = a;
        expression.values.operands[1] = b;
        expression.value_count = 2;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_SUBTRACT`. */
    static inline ovstage_population_expression_t ovstage_population_expression_subtract(
        const ovstage_population_expression_t* a, const ovstage_population_expression_t* b)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_SUBTRACT;
        expression.values.operands[0] = a;
        expression.values.operands[1] = b;
        expression.value_count = 2;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_MULTIPLY`. */
    static inline ovstage_population_expression_t ovstage_population_expression_multiply(
        const ovstage_population_expression_t* a, const ovstage_population_expression_t* b)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_MULTIPLY;
        expression.values.operands[0] = a;
        expression.values.operands[1] = b;
        expression.value_count = 2;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_DIVIDE`. */
    static inline ovstage_population_expression_t ovstage_population_expression_divide(
        const ovstage_population_expression_t* a, const ovstage_population_expression_t* b)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_DIVIDE;
        expression.values.operands[0] = a;
        expression.values.operands[1] = b;
        expression.value_count = 2;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_RESOLVED_BOUND_MATERIAL`. `prim_predicate` may be `NULL`. */
    static inline ovstage_population_expression_t ovstage_population_expression_resolved_bound_material(
        ovx_string_t purpose, const ovstage_population_prim_predicate_t* prim_predicate)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_RESOLVED_BOUND_MATERIAL;
        expression.values.resolved.name = purpose;
        expression.values.resolved.prim_predicate = prim_predicate;
        expression.values.resolved.property_predicate = NULL;
        expression.value_count = 2;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_RESOLVED_MATERIAL_BINDING_STRENGTH`. `prim_predicate` may be `NULL`. */
    static inline ovstage_population_expression_t ovstage_population_expression_resolved_material_binding_strength(
        ovx_string_t purpose, const ovstage_population_prim_predicate_t* prim_predicate)
    {
        ovstage_population_expression_t expression =
            ovstage_population_expression_resolved_bound_material(purpose, prim_predicate);
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_RESOLVED_MATERIAL_BINDING_STRENGTH;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_RESOLVED_MATERIAL_BINDING_PURPOSE`. `prim_predicate` may be `NULL`. */
    static inline ovstage_population_expression_t ovstage_population_expression_resolved_material_binding_purpose(
        ovx_string_t purpose, const ovstage_population_prim_predicate_t* prim_predicate)
    {
        ovstage_population_expression_t expression =
            ovstage_population_expression_resolved_bound_material(purpose, prim_predicate);
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_RESOLVED_MATERIAL_BINDING_PURPOSE;
        return expression;
    }

    /** `OVSTAGE_POPULATION_EXPRESSION_RESOLVED_COLLECTION_MEMBERS`. Either predicate may be
     *  `NULL`. */
    static inline ovstage_population_expression_t ovstage_population_expression_resolved_collection_members(
        ovx_string_t                                   collection,
        const ovstage_population_prim_predicate_t*     prim_predicate,
        const ovstage_population_property_predicate_t* property_predicate)
    {
        ovstage_population_expression_t expression;
        expression.kind = OVSTAGE_POPULATION_EXPRESSION_RESOLVED_COLLECTION_MEMBERS;
        expression.values.resolved.name = collection;
        expression.values.resolved.prim_predicate = prim_predicate;
        expression.values.resolved.property_predicate = property_predicate;
        expression.value_count = 3;
        return expression;
    }

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_POPULATION_EXPRESSION_H */
