/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage_population_types.h
 * @brief The types of the population family.
 *
 * @details
 * This header holds the types, the macros and the type helpers of the
 * population family: the population types and the export types. The
 * population functions live in ovstage_population.h and the export functions
 * in ovstage_population_export.h. The family vtable lives in
 * ovstage_ext_vtables.h, which includes this header for the member
 * prototypes.
 *
 * This header includes only type headers and the predicate and expression
 * headers. It has no dependency on the family headers or on
 * ovstage_ext_vtables.h.
 *
 * @version 0.4.0
 * @date 2026-10-21
 */

#ifndef OVSTAGE_POPULATION_TYPES_H
#define OVSTAGE_POPULATION_TYPES_H

#include "ovstage_api/ovstage_api_types.h"
#include "ovstage_population_predicate.h"
#include "ovstage_population_expression.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════════
 * Population types
 *
 * The types of the population family. ovstage_population.h holds the functions.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** Coarse population domains for the canonical population entry points.
 *  Bitmask — OR values together.
 *
 *  `OVSTAGE_POPULATION_DOMAIN_NONE` (0) selects no data domain: a `domains`
 *  argument of 0 populates nothing at all. A bit that names no domain below
 *  is ignored. Other data, such as physics, is populated through a
 *  description's selectors (`ovstage_population_desc_t`). */
typedef enum
{
    OVSTAGE_POPULATION_DOMAIN_NONE      = 0u,
    /** Scene data for rendering: supported geometry, lights, cameras, materials,
     *  volumes, and render settings, with their transforms, hierarchy, and
     *  instances. Transformable prims are populated even when they carry no
     *  rendered data, whatever API schemas they apply. A populated prim carries
     *  only the properties this domain handles; properties of other APIs, such as
     *  physics, are populated only by a description that selects them. */
    OVSTAGE_POPULATION_DOMAIN_RENDERING = 1u << 0,
} ovstage_population_domain_t;

/** Derived properties.
 *
 *  Everything else a selector publishes is read off a prim. A derived property is
 *  computed: a rule names a column and gives an expression
 *  (`ovstage_population_expression.h`), and population publishes its value on each prim
 *  the rule applies to. The expression is plain data, validated at enqueue and evaluated
 *  by population. A derived column is an attribute or a relationship; which one follows
 *  from its type, as for a column read off a prim.
 *
 *  Where a rule applies. On each prim the selector's `prim_predicate` selects, narrowed
 *  by the rule's own `prim_predicate` when it has one, and, for a multiple-apply family,
 *  once per applied instance. `when` then decides from the rule's own property on that
 *  prim (`ovstage_population_derive_when_t`). Where the rule applies and the expression
 *  yields a value, the column is published whether or not `property_predicate` selects a
 *  property of that name, and replaces any value read under that name.
 *
 *  Column type. A derived property never changes a type population defines. Where the
 *  prim's schemas declare `name`, or population publishes it in a fixed type ("Populated
 *  value types"), the column has that type. Anywhere else the column has the rule's
 *  `dtype`, `semantic` and `is_array`, any combination a column read off a prim can have;
 *  a rule that leaves them zero takes the type its value yields, so paths publish as a
 *  relationship. The value converts to the column's type as an authored value of another
 *  type does; a value that does not convert fails the evaluation. An `opaque` attribute
 *  holds no value, so the rule sees no property of that name: the column takes the rule's
 *  type or the value's, and `when` finds the property absent.
 *
 *  Names. A derived column is not a USD property, so `name` may hold what a property name
 *  cannot, such as `-`, which keeps it apart from any property a prim authors. It may not be
 *  empty, start or end with `:`, hold `::`, or contain whitespace. It cannot be a transform
 *  column population derives itself (see *Transform attributes*) or another column
 *  population or the stage publishes itself: a name starting with `usd-` or holding
 *  `:usd-metadata:` is rejected. With `multi_apply_schema_family` set, `name` holds
 *  `__INSTANCE_NAME__` once, replaced by each applied instance's name; a family without it,
 *  or `__INSTANCE_NAME__` without a family, is rejected, as is a name some instance name would
 *  make one of the above. A rule for one known instance is a plain rule naming it, kept to
 *  the prims that apply it by a prim predicate
 *  (`OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_APPLIED_SCHEMA` with `Family:instance`).
 *
 *  One deriver per column. Rules in two different descriptions of one call may not
 *  derive the same column: equal names, or a name with `__INSTANCE_NAME__` that can
 *  expand to the other rule's name, are rejected at enqueue, whatever prims the rules
 *  apply to. Within one description, two rules deriving one column on one prim resolve
 *  last-one-wins, in selector and rule order.
 *
 *  Validation. A malformed tree, a type mismatch visible from the tree, and a name or a
 *  conflict as above are rejected at enqueue. An evaluation that fails on a prim
 *  publishes nothing for that prim and is logged once per rule.
 *
 *  @remark With the RENDERING domain populated, `purpose` on an imageable prim and
 *   `extent` on a `PointInstancer` keep the value RENDERING computes; a rule deriving
 *   either publishes nothing on those prims.
 *
 *  Staying current. After `ovstage_population_apply_usd_changes`, a derived column holds
 *  what a fresh population would publish. */

/** Whether a rule publishes on a prim, depending on whether the prim already has the
 *  property the rule derives. */
typedef enum
{
    /** Always. The derived value replaces whatever the prim has. */
    OVSTAGE_POPULATION_DERIVE_ALWAYS = 0,
    /** Only where the prim authors no value for the property: no default or time samples
     *  for an attribute, no targets for a relationship. A value block counts as no value.
     *  An authored value is published as read. */
    OVSTAGE_POPULATION_DERIVE_WHEN_UNAUTHORED = 1,
    /** Only where the prim has no such property at all: no schema declares it and nothing
     *  is authored. A schema fallback is published as read. */
    OVSTAGE_POPULATION_DERIVE_WHEN_ABSENT = 2,
} ovstage_population_derive_when_t;

/** One derived-property rule. Zero-initialize, then set `name` and `value`, and the type
 *  (`dtype`, `semantic`, `is_array`) where no schema declares the column and the value's
 *  own type is not the one wanted; the rest is optional. See *Derived properties* above. */
typedef struct ovstage_population_derived_property_t
{
    /** The column to publish. May contain `__INSTANCE_NAME__` when
     *  `multi_apply_schema_family` is set. */
    ovx_string_t name;

    /** A multiple-apply schema family the rule expands over, replacing
     *  `__INSTANCE_NAME__` in `name`. Empty for a plain column. */
    ovx_string_t multi_apply_schema_family;

    /** Narrows the rule to some of the selector's prims. `NULL` applies it to every prim
     *  the selector selects. Referenced, not copied: must outlive the call. */
    const ovstage_population_prim_predicate_t* prim_predicate;

    /** When the rule publishes on a prim it applies to. Zero is
     *  `OVSTAGE_POPULATION_DERIVE_ALWAYS`. */
    ovstage_population_derive_when_t when;

    /** The type where no schema declares the column and population does not fix one;
     *  zero takes the type the value yields. `dtype.lanes` is the tuple width. */
    DLDataType                   dtype;
    /** What the bytes mean, as for a read column: `OVSTAGE_SEMANTIC_RELATIONSHIP_PATH_ID`
     *  makes the column a relationship, and `OVSTAGE_SEMANTIC_TOKEN_ID` on a `uint64` makes it
     *  a token. A number or a bool takes `OVSTAGE_SEMANTIC_NONE`, and a vector at most a USD
     *  role: `POINT`, `VECTOR`, `NORMAL`, `COLOR` or `TEXTURE_COORDINATE`. Enqueue fails with
     *  `OVSTAGE_ERROR_INVALID_ARGUMENT` on any other. */
    ovstage_attribute_semantic_t semantic;
    /** Whether the column holds an array per prim. */
    bool                         is_array;

    /** The value. Required. Referenced, not copied: the tree must outlive the call. */
    const ovstage_population_expression_t* value;
} ovstage_population_derived_property_t;

/** One population selector: which prims are populated, and which of their
 *  properties and metadata. */
typedef struct ovstage_population_selector_t
{
    /** Which prims this selector populates. */
    ovstage_population_prim_predicate_t     prim_predicate;

    /** Which properties of those prims are populated. */
    ovstage_population_property_predicate_t property_predicate;

    /** Metadata to populate for each selected prim, as paths: a metadata field
     *  name, extended with a `:`-joined key path to reach inside a dictionary
     *  field. A path naming a dictionary rather than a single value populates
     *  every value beneath it, one attribute each. Populated as
     *  `usd-metadata:<path>`. */
    const ovx_string_t* prim_metadata_paths;
    size_t              prim_metadata_path_count;

    /** Metadata to populate for each selected property, as paths -- same addressing
     *  and dictionary flattening as `prim_metadata_paths`. Applies to every property
     *  `property_predicate` selects; use a further selector to populate a field for
     *  some properties only. Populated as `<property>:usd-metadata:<path>`. */
    const ovx_string_t* property_metadata_paths;
    size_t              property_metadata_path_count;

    /** Properties -- attributes and relationships -- to compute for the selected prims
     *  rather than read off them. Each rule applies to every prim `prim_predicate`
     *  selects, narrowed by its own predicate; see *Derived properties*. May be `NULL`
     *  when the count is 0. */
    const ovstage_population_derived_property_t* derived_properties;
    size_t                                       derived_property_count;
} ovstage_population_selector_t;

/** A complete description of what to populate.
 *
 *  Zero-initialize, then set the fields you need. A desc with no selectors and
 *  `domains` set is exactly equivalent to passing that `domains` to the entry
 *  point without a desc; a desc with neither populates nothing.
 *
 *  Descs are supplied as an array and compose: `domains` is OR-ed across every
 *  desc, and their selectors are combined in array order.
 *
 *  The array, and everything it points at, must stay valid only until the call
 *  returns: the description is copied as the operation is enqueued. */
typedef struct ovstage_population_desc_t
{
    /** Bitmask of `ovstage_population_domain_t` — built-in domains to populate
     *  alongside this desc's selectors. `0` for selectors only. */
    uint32_t domains;

    /** The selectors that make up this description. May be `NULL` when `domains`
     *  alone is wanted. */
    const ovstage_population_selector_t* selectors;
    size_t                               selector_count;

    /** Stage metadata to populate onto the root prim, as paths: a stage
     *  metadata field name, extended with a `:`-joined key path to reach inside a
     *  dictionary field — `customLayerData:myTool:version`. A path naming a
     *  dictionary rather than a single value populates every value beneath it, one
     *  attribute each. A path that does not resolve populates nothing. */
    const ovx_string_t* stage_metadata_paths;
    size_t              stage_metadata_path_count;
} ovstage_population_desc_t;

/** Opaque id of an enqueued population operation, returned by the async
 *  entry points and passed to `ovstage_population_wait_op`. Zero is never a
 *  live op id. */
typedef struct ovstage_population_op_id_t
{
    uint64_t raw_id;
} ovstage_population_op_id_t;

#ifdef __cplusplus
#define OVSTAGE_POPULATION_INVALID_OP_ID ovstage_population_op_id_t{0}
#else
#define OVSTAGE_POPULATION_INVALID_OP_ID ((ovstage_population_op_id_t){0})
#endif

/** Opaque handle to a USD reference added by
 *  `ovstage_population_add_usd_reference_from_file` / `_from_string`, passed to
 *  `ovstage_population_remove_usd_reference` to take it back out. Reserved synchronously
 *  by the add call (so it is valid immediately, before the op runs) and freed
 *  by `ovstage_population_remove_usd_reference`, `_reset_usd`, or a fresh `_open_usd_*`.
 *  Zero is never a live handle. */
typedef struct ovstage_population_usd_reference_handle_t
{
    uint64_t raw_id;
} ovstage_population_usd_reference_handle_t;

#ifdef __cplusplus
#define OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE ovstage_population_usd_reference_handle_t{0}
#else
#define OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE ((ovstage_population_usd_reference_handle_t){0})
#endif

/** The `time` value that selects USD's *default* time code rather than a
 *  numeric one.
 *
 *  Every population entry point taking a `time` takes it in seconds. Passing
 *  this instead evaluates each property at USD's default time code.
 *
 *  The value is a NaN.
 *
 *  **Never compare a time against it with `==`.** A NaN is not equal to
 *  itself, so `time == OVSTAGE_POPULATION_DEFAULT_TIME` is false even for the
 *  default time. Use `ovstage_population_is_default_time` instead. */
#define OVSTAGE_POPULATION_DEFAULT_TIME NAN

/** Test whether a `time` selects USD's default time code.
 *
 *  Use this rather than `time == OVSTAGE_POPULATION_DEFAULT_TIME`, which is
 *  false even for the default time.
 *
 *  @param time  A time in seconds, as passed to a population entry point.
 *  @return `true` if @p time selects the default time code, `false` if it
 *   names a numeric time. */
static inline bool ovstage_population_is_default_time(double time)
{
    return isnan(time) != 0;
}

/** Result of enqueuing a population operation.
 *
 *  Population runs asynchronously: `status` reports only that the operation was accepted (`OVSTAGE_OK`). Call `ovstage_population_wait_op(op_id)` to await completion and obtain the outcome (with detail via `ovstage_population_get_last_error`).
 *  `op_index` identifies the operation for `ovstage_population_wait_op`, or is
 *  `OVSTAGE_POPULATION_INVALID_OP_ID` if nothing was enqueued. */
typedef struct ovstage_population_enqueue_result_t
{
    ovstage_api_status_t           status;
    ovstage_population_op_id_t op_index;
} ovstage_population_enqueue_result_t;

/** Output of `ovstage_population_wait_op`.
 *
 *  Because a single wait covers all operations up to and including the awaited
 *  op, more than one op can have failed within that range. `error_op_ids`
 *  lists those failed op ids. Each failure is reported exactly once — by the
 *  first `ovstage_population_wait_op` call (on any thread) whose range covers
 *  it — so a later wait does not re-report it. For each id, retrieve the
 *  human-readable detail with `ovstage_population_get_last_op_error`.
 *
 *  The array (and the strings from `ovstage_population_get_last_op_error`) live
 *  in the calling thread's storage and stay valid only until that same thread's
 *  next `ovstage_population_wait_op` call, which overwrites them; copy anything
 *  you need to keep before calling again. Calls on different threads use
 *  independent buffers and do not clobber each other.
 *
 *  `lowest_pending_op_id` is meaningful only when the wait returns
 *  `OVSTAGE_ERROR_TIMEOUT`: it is the lowest op id in the awaited range that
 *  had not completed when the timeout elapsed (useful for partial-progress
 *  reporting). It is `OVSTAGE_POPULATION_INVALID_OP_ID` otherwise. */
typedef struct ovstage_population_op_wait_result_t
{
    const ovstage_population_op_id_t* error_op_ids;
    size_t                            error_op_id_count;
    ovstage_population_op_id_t        lowest_pending_op_id;
} ovstage_population_op_wait_result_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Population export types
 *
 * The types of the export functions of the population family.
 * ovstage_population_export.h holds the functions.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** Selects the source-state class exported by a descriptor. */
typedef enum
{
    /** Export the current committed ovstage state constrained by predicates and rules. */
    OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE = 0,
    /** Export properties dirtied in the open interval (since_ordinal, ordinal],
     *  intersected with the descriptor's prim and property predicates. */
    OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL = 1,
} ovstage_population_export_selection_t;

/** Controls the authored USD prim specifier for newly materialized prim specs. */
typedef enum
{
    /** Author sparse overlay opinions with SdfSpecifierOver. */
    OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER = 0,
    /** Author durable definitions with SdfSpecifierDef. */
    OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF = 1,
} ovstage_population_export_layer_mode_t;

/** How rule paths are compared against ovstage/USD prim paths. */
typedef enum
{
    /** Match one exact absolute prim path. */
    OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT = 0,
    /** Match the path itself and all descendants under it. */
    OVSTAGE_POPULATION_EXPORT_PATH_MATCH_PREFIX = 1,
} ovstage_population_export_path_match_t;

/** How rule attribute-name patterns are compared against ovstage runtime names. */
typedef enum
{
    /** Match the exact attribute name. */
    OVSTAGE_POPULATION_EXPORT_NAME_MATCH_EXACT = 0,
    /** Match with simple glob wildcards accepted by the implementation. */
    OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB = 1,
} ovstage_population_export_name_match_t;

/** How local transform attributes are emitted to USD. */
typedef enum
{
    /** Prefer standard xformOp opinions such as xformOp:transform where possible. */
    OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP = 0,
    /** Do not export recognized transform attributes. */
    OVSTAGE_POPULATION_EXPORT_TRANSFORM_NONE = 1,
    /** Author local matrices as an explicit USD matrix transform op. */
    OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_MATRIX_OP = 2,
} ovstage_population_export_transform_policy_t;

/** Policy for selected source metadata whose USD schema or type cannot be resolved. */
typedef enum
{
    /** Drop unknown metadata and continue. */
    OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP = 0,
    /** Preserve supported unknown metadata in a namespaced fallback where available. */
    OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_PRESERVE = 1,
    /** Fail the export when unknown selected metadata is encountered. */
    OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_FAIL = 2,
} ovstage_population_export_unknown_metadata_policy_t;

/** Custom-data key used to preserve an array of unresolved applied API schema
 *  names when unknown_metadata_policy is PRESERVE. */
#define OVSTAGE_POPULATION_EXPORT_UNKNOWN_APPLIED_API_SCHEMAS_CUSTOM_DATA_KEY \
    "ovpopulation:unknownAppliedAPISchemas"

/** Controls whether effective applied API schemas recorded on source prims are
 *  authored on destination prims. */
typedef enum
{
    /** Author only API schemas named by explicit api_schema_rules. */
    OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY = 0,
    /** Also author each selected prim's effective usd-schemas metadata. */
    OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_APPLY_RECORDED = 1,
} ovstage_population_export_source_api_schema_policy_t;

/** Controls automatic projection of source runtime attributes. */
typedef enum
{
    /** Author only attributes named by explicit attribute_rules. */
    OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES = 0,
    /** Also author selected attributes declared by each prim's effective USD schemas. */
    OVSTAGE_POPULATION_EXPORT_PROJECTION_SCHEMA_DECLARED = 1,
} ovstage_population_export_projection_t;

/** Per-rule behavior flags. Flags may be ORed together. */
typedef enum
{
    /** No special behavior. */
    OVSTAGE_POPULATION_EXPORT_RULE_NONE = 0,
    /** Export a selected attribute even when the value equals the USD default/fallback. */
    OVSTAGE_POPULATION_EXPORT_RULE_EXPORT_DEFAULT_VALUE = 1u << 0,
    /** Allow schema-derived type resolution instead of requiring an exact explicit type. */
    OVSTAGE_POPULATION_EXPORT_RULE_DERIVED_ALLOWED = 1u << 1,
    /** Infer a custom USD value type from the ovstage runtime tensor shape/type. */
    OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE = 1u << 3,
    /** Treat a missing, unsupported, or non-authoring rule as an export error. */
    OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED = 1u << 4,
} ovstage_population_export_rule_flags_t;

/** USD property kind authored by a property-edge rule. */
typedef enum
{
    /** Invalid sentinel. Zero-initialized property rules are rejected; callers
     *  must select RELATIONSHIP, CONNECTION, or MATERIAL_BINDING explicitly. */
    OVSTAGE_POPULATION_EXPORT_PROPERTY_INVALID = 0,
    /** Author a UsdRelationship target list. */
    OVSTAGE_POPULATION_EXPORT_PROPERTY_RELATIONSHIP = 1,
    /** Author an attribute connection target list. */
    OVSTAGE_POPULATION_EXPORT_PROPERTY_CONNECTION = 2,
    /** Author the material:binding relationship. Other binding names are unsupported. */
    OVSTAGE_POPULATION_EXPORT_PROPERTY_MATERIAL_BINDING = 3,
} ovstage_population_export_property_kind_t;

/** Destination kind for a metadata rule. */
typedef enum
{
    /** Author metadata on matching prim specs. */
    OVSTAGE_POPULATION_EXPORT_METADATA_PRIM = 0,
    /** Author entries into layer customData. */
    OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_CUSTOM_DATA = 1,
    /** Author selected asset references on matching prim specs. */
    OVSTAGE_POPULATION_EXPORT_METADATA_REFERENCES = 2,
    /** Author known layer-level metadata such as defaultPrim or documentation. */
    OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_STANDARD = 3,
} ovstage_population_export_metadata_kind_t;

/** Controls the initial contents of a persistent USD export destination. */
typedef enum
{
    /** Start from an empty in-memory USD stage. Existing stored content is not
     *  read and is replaced only when the destination is explicitly saved. */
    OVSTAGE_POPULATION_EXPORT_DESTINATION_EMPTY = 0,
    /** Open and preserve an existing USD layer stack. Export opinions are
     *  authored into its root layer without flattening composed content. */
    OVSTAGE_POPULATION_EXPORT_DESTINATION_OPEN_EXISTING = 1,
} ovstage_population_export_destination_mode_t;

/** ABI-opaque handle for a reusable, source-stage-bound USD export destination. */
typedef struct ovstage_population_export_destination_handle_t
{
    uint64_t raw_id;
} ovstage_population_export_destination_handle_t;

/** Invalid/sentinel reusable export destination handle. */
#ifdef __cplusplus
#define OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE ovstage_population_export_destination_handle_t{0}
#else
#define OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE ((ovstage_population_export_destination_handle_t){0})
#endif

/** Attribute export rule.
 *
 *  Use this rule to map one or more ovstage runtime attributes to USD
 *  attributes. Empty path means every selected prim. Empty destination_attribute_name
 *  reuses source_attribute_name. Empty usd_type_name asks the exporter to use
 *  the destination schema/existing USD attribute type, or infer a custom type
 *  when OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE is set.
 */
typedef struct
{
    /** Prim path selector; empty means all selected prims. */
    ovx_string_t path;
    /** Exact or prefix path matching mode for path. */
    ovstage_population_export_path_match_t path_match;
    /** ovstage runtime attribute name or glob pattern to read. */
    ovx_string_t source_attribute_name;
    /** Exact/glob matching mode for source_attribute_name. */
    ovstage_population_export_name_match_t source_attribute_name_match;
    /** USD attribute name to author; empty reuses source_attribute_name. */
    ovx_string_t destination_attribute_name;
    /** USD value type name to author; empty uses schema/destination/inference. */
    ovx_string_t usd_type_name;
    /** Optional required schema name used when resolving schema-defined attribute types. */
    ovx_string_t required_schema_name;
    /** Bitmask of ovstage_population_export_rule_flags_t. */
    uint32_t flags;
} ovstage_population_export_attribute_rule_t;

/** Prim export rule.
 *
 *  Use this rule to force creation of USD prim specs and, in DEF mode, to supply
 *  or override the authored USD type name. Prefix rules author each selected
 *  runtime prim under the prefix.
 */
typedef struct
{
    /** Prim path selector. */
    ovx_string_t path;
    /** Exact or prefix path matching mode for path. */
    ovstage_population_export_path_match_t path_match;
    /** USD prim type name to author; empty preserves/infer source type where available. */
    ovx_string_t usd_type_name;
    /** Optional metadata key used to preserve unknown type names when policy permits. */
    ovx_string_t unknown_type_metadata_key;
} ovstage_population_export_prim_rule_t;

/** Applied API schema export rule.
 *
 *  Use this rule to apply a USD API schema to matching prims. Multiple-apply API
 *  schemas use instance_name; single-apply schemas leave it empty.
 */
typedef struct
{
    /** Prim path selector. */
    ovx_string_t path;
    /** Exact or prefix path matching mode for path. */
    ovstage_population_export_path_match_t path_match;
    /** API schema type name to apply, such as "PhysicsRigidBodyAPI". */
    ovx_string_t schema_name;
    /** Multiple-apply instance name; empty for single-apply schemas. */
    ovx_string_t instance_name;
    /** Bitmask of ovstage_population_export_rule_flags_t. */
    uint32_t flags;
} ovstage_population_export_api_schema_rule_t;

/** Property-target export rule.
 *
 *  Relationship and material-binding rules read target paths from the runtime
 *  attribute named by source_property_name. Connection rules read target paths
 *  from the companion runtime attribute named "<source_property_name>.connect".
 *  The source attributes must use the corresponding relationship-path or
 *  connection-path semantic. Empty path means every selected prim. Empty
 *  destination_property_name reuses source_property_name.
 */
typedef struct
{
    /** Prim path selector; empty means all selected prims. */
    ovx_string_t path;
    /** Exact or prefix path matching mode for path. */
    ovstage_population_export_path_match_t path_match;
    /** Runtime property name whose target-path attribute is read. */
    ovx_string_t source_property_name;
    /** USD relationship or attribute name to author; empty reuses source_property_name. */
    ovx_string_t destination_property_name;
    /** USD value type for connection attributes; ignored for relationships. */
    ovx_string_t usd_type_name;
    /** Property kind to author. */
    ovstage_population_export_property_kind_t property_kind;
    /** Bitmask of ovstage_population_export_rule_flags_t. */
    uint32_t flags;
} ovstage_population_export_property_edge_rule_t;

/** Metadata export rule.
 *
 *  Use this rule to map ovstage metadata attributes to USD prim metadata, layer
 *  metadata/customData, or references. Empty path means every selected prim for
 *  prim-scoped metadata; layer-scoped metadata ignores path.
 */
typedef struct
{
    /** Prim path selector for prim-scoped metadata; empty means all selected prims. */
    ovx_string_t path;
    /** Exact or prefix path matching mode for path. */
    ovstage_population_export_path_match_t path_match;
    /** ovstage metadata attribute name to read. */
    ovx_string_t source_metadata_name;
    /** USD metadata/customData key to author; empty reuses source_metadata_name. */
    ovx_string_t destination_metadata_name;
    /** Metadata destination kind. */
    ovstage_population_export_metadata_kind_t metadata_kind;
    /** Bitmask of ovstage_population_export_rule_flags_t. */
    uint32_t flags;
} ovstage_population_export_metadata_rule_t;

/** USD export descriptor. */
typedef struct
{
    /** Must be sizeof(ovstage_population_export_desc_t) for this header
     *  revision. This is not an automatic compatibility mode for old layouts. */
    uint32_t struct_size;
    /** Policy for applied API schemas recorded on source prims. */
    ovstage_population_export_source_api_schema_policy_t source_api_schema_policy;
    /** Policy for automatic projection of source runtime attributes. */
    ovstage_population_export_projection_t projection;

    /** Current committed ordinal expected by the caller. Export queries the current
     *  topology and rejects stale or future ordinals instead of silently exporting
     *  a different source state. */
    ovstage_ordinal_t ordinal;
    /** Lower bound for CHANGED_PROPERTIES_SINCE_ORDINAL; exports the open
     *  interval (since_ordinal, ordinal]. */
    ovstage_ordinal_t since_ordinal;

    ovstage_population_export_selection_t selection;
    ovstage_population_export_layer_mode_t layer_mode;
    ovstage_population_export_transform_policy_t transform_policy;
    ovstage_population_export_unknown_metadata_policy_t unknown_metadata_policy;

    /** Which runtime prims are candidates for export. Defaults to ALL.
     *
     *  Export supports NONE, ALL, AND, OR, NOT, HAS_TYPE, IS_A_TYPE,
     *  HAS_SCHEMA, HAS_APPLIED_SCHEMA, HAS_APPLIED_SCHEMA_IN_NAMESPACE,
     *  HAS_PATH, and IS_UNDER_PATH. Other declared kinds require USD semantics
     *  not preserved by the public runtime representation and are rejected
     *  atomically with OVSTAGE_ERROR_NOT_SUPPORTED. */
    ovstage_population_prim_predicate_t prim_predicate;

    /** Which runtime properties are candidates for property-scoped export rules
     *  and schema-declared projection. Defaults to ALL. Prim rules,
     *  applied-schema rules, and prim/layer metadata rules are governed only by
     *  `prim_predicate`.
     *
     *  Export supports NONE, ALL, AND, OR, NOT, DECLARED_BY_SCHEMA, HAS_NAME,
     *  IN_NAMESPACE, IS_ATTRIBUTE, and IS_RELATIONSHIP. Other declared kinds
     *  are rejected atomically with OVSTAGE_ERROR_NOT_SUPPORTED. */
    ovstage_population_property_predicate_t property_predicate;

    /** Each descriptor array below is limited to 4096
     *  entries. Counts above that limit are rejected before an array is read
     *  or retained. */
    const ovstage_population_export_prim_rule_t* prim_rules;
    size_t prim_rule_count;

    const ovstage_population_export_api_schema_rule_t* api_schema_rules;
    size_t api_schema_rule_count;

    const ovstage_population_export_attribute_rule_t* attribute_rules;
    size_t attribute_rule_count;

    const ovstage_population_export_property_edge_rule_t* property_rules;
    size_t property_rule_count;

    const ovstage_population_export_metadata_rule_t* metadata_rules;
    size_t metadata_rule_count;

} ovstage_population_export_desc_t;

/** Export diagnostic counters. Counters are best-effort progress diagnostics,
 *  not a stable ABI for behavioral decisions. */
typedef struct
{
    size_t prims_examined;
    size_t prims_exported;
    size_t prims_skipped;

    size_t attributes_examined;
    size_t attributes_exported;
    size_t attributes_skipped;
    size_t attributes_unsupported;
    size_t api_schemas_examined;
    size_t api_schemas_applied;
    size_t api_schemas_skipped;

    size_t custom_attributes_inferred;
    size_t unknown_metadata_preserved;

    size_t transforms_examined;
    size_t transforms_exported;
    size_t property_edges_examined;
    size_t property_edges_exported;
    size_t property_edges_skipped;
    size_t property_edges_unsupported;

    size_t metadata_examined;
    size_t metadata_exported;
    size_t metadata_skipped;
    size_t metadata_unsupported;
    /** Export could not read the requested current source ordinal. */
    size_t source_states_unavailable;

} ovstage_population_export_report_t;

#ifdef __cplusplus
}

extern "C++"
{
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_population_op_id_t)
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_population_usd_reference_handle_t)
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_population_export_destination_handle_t)
}
#endif

#endif /* OVSTAGE_POPULATION_TYPES_H */
