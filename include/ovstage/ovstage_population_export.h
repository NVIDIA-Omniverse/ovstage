/* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * ovstage population export to USD.
 *
 * Public C ABI for authoring selected ovstage runtime state into USD storage.
 * The header intentionally exposes no OpenUSD C++ types. Destinations are
 * addressed by a storage path or resolver identifier that ovstage itself opens
 * and owns; ovstage never authors a USD layer or stage that the caller already
 * has open, because USD identifiers are not portable across USD runtimes.
 *
 * Use this API when an application has populated or otherwise authored an
 * ovstage runtime and needs to persist a selected slice back to USD. The
 * synchronous entry points remain the primitive API. Optional asynchronous
 * entry points enqueue the same work on the per-instance ovpopulation FIFO and
 * retain the resolved USD destination until completion.
 *
 * Async export is a convenience scheduler, not a USD synchronization boundary.
 * Callers must not concurrently read or author the same destination storage,
 * and must fence async work before mixing it with synchronous export.
 * Every accepted async export must eventually receive an export-specific
 * terminal wait; until then its report and destination keepalive remain owned
 * by the ovpopulation state.
 */

#ifndef OVSTAGE_POPULATION_EXPORT_H
#define OVSTAGE_POPULATION_EXPORT_H

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_population.h>
#include <ovstage/ovstage_population_predicate.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

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
typedef uint64_t ovstage_population_export_destination_handle_t;

/** Invalid/sentinel reusable export destination handle. */
#define OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE ((ovstage_population_export_destination_handle_t)0)

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

/**
 * @brief Initialize an export descriptor with conservative defaults.
 *
 * The descriptor selects explicit-predicate export in OVER mode, authors recognized
 * transforms as USD xform ops, and drops metadata whose USD type is unknown.
 *
 * @param desc [out] Descriptor to initialize.
 * @return `OVSTAGE_OK` on success or `OVSTAGE_ERROR_INVALID_ARGUMENT` when
 *         @p desc is null.
 */
ovstage_api_status_t ovstage_population_export_desc_init(
    ovstage_population_export_desc_t* desc);

/**
 * @brief Initialize a current-snapshot descriptor for an ordinal.
 *
 * Applies the defaults from `ovstage_population_export_desc_init`, then records
 * the expected current ordinal. Both predicates retain their ALL defaults.
 *
 * @param desc [out] Descriptor to initialize.
 * @param ordinal Current committed ordinal expected by the caller.
 * @return `OVSTAGE_OK` on success or `OVSTAGE_ERROR_INVALID_ARGUMENT` when
 *         @p desc is null.
 */
ovstage_api_status_t ovstage_population_export_desc_init_snapshot(
    ovstage_population_export_desc_t* desc,
    ovstage_ordinal_t ordinal);

/**
 * @brief Initialize a typed DEF-hierarchy export descriptor rooted at one USD path.
 *
 * Applies current-snapshot defaults, selects `root_path` and its descendants,
 * applies recorded API schemas, and projects schema-declared attributes. Callers
 * may refine the returned descriptor before exporting.
 *
 * The caller-owned `ovx_string_t` object and the character bytes referenced by
 * that view are retained by the descriptor. They must outlive a synchronous
 * export call or, for asynchronous export, successful enqueue; the enqueue
 * then copies them.
 *
 * @param desc [out] Descriptor to initialize.
 * @param ordinal Current committed ordinal expected by the caller.
 * @param root_path Root USD path to select, retained by the descriptor.
 * @return `OVSTAGE_OK` on success or the status from
 *         `ovstage_population_export_desc_init_snapshot`.
 */
static inline ovstage_api_status_t ovstage_population_export_desc_init_typed_hierarchy(
    ovstage_population_export_desc_t* desc,
    ovstage_ordinal_t ordinal,
    const ovx_string_t* root_path)
{
    const ovstage_api_status_t status = ovstage_population_export_desc_init_snapshot(desc, ordinal);
    if (status != OVSTAGE_OK)
        return status;
    desc->layer_mode = OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF;
    desc->source_api_schema_policy = OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_APPLY_RECORDED;
    desc->projection = OVSTAGE_POPULATION_EXPORT_PROJECTION_SCHEMA_DECLARED;
    desc->prim_predicate = ovstage_population_prim_predicate_is_under_path(root_path, 1);
    return OVSTAGE_OK;
}

/**
 * @brief Create a reusable USD export destination and wait for initialization.
 *
 * The destination is bound to @p stage, which must outlive it. Exactly one
 * active destination may own a canonical USD identifier in this process.
 * Creation never writes storage. EMPTY starts from an empty private
 * stage; OPEN_EXISTING fails unless the identifier can be opened and preserves
 * its layer stack while selecting the root layer as the edit target.
 *
 * @param stage Source ovstage instance and owner of destination ordering.
 * @param usd_identifier Local path or resolver identifier for later save.
 * @param mode Initial-content policy.
 * @param out_destination [out] Receives the destination handle on success and
 *        the invalid handle on failure.
 * @return `OVSTAGE_OK` on success or an `ovstage_api_status_t` error code.
 */
ovstage_api_status_t ovstage_population_export_destination_create(
    ovstage_instance_t* stage,
    ovx_string_t usd_identifier,
    ovstage_population_export_destination_mode_t mode,
    ovstage_population_export_destination_handle_t* out_destination);

/**
 * @brief Enqueue creation of a reusable USD export destination.
 *
 * The handle is reserved synchronously and remains usable for later operations
 * enqueued on the same @p stage. Those operations execute after initialization
 * on the per-instance population FIFO. A non-invalid handle does not mean that
 * opening succeeded; await the returned operation to observe its outcome. The
 * identifier is copied before this function returns.
 *
 * @param stage Source ovstage instance and owner of destination ordering.
 * @param usd_identifier Local path or resolver identifier for later save.
 * @param mode Initial-content policy.
 * @param out_destination [out] Receives the reserved destination handle, or
 *        the invalid handle when enqueue is rejected.
 * @return Enqueue result for `ovstage_population_export_wait_op`.
 */
ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_create(
    ovstage_instance_t* stage,
    ovx_string_t usd_identifier,
    ovstage_population_export_destination_mode_t mode,
    ovstage_population_export_destination_handle_t* out_destination);

/**
 * @brief Export into a reusable destination and wait for completion.
 *
 * Successful calls accumulate opinions in the destination without saving.
 * Failure leaves the destination at its previously committed in-memory state.
 * The handle must have been created for the same @p stage.
 *
 * @param stage Source ovstage instance that owns the destination.
 * @param destination Reusable destination created for @p stage.
 * @param desc Export descriptor. The caller retains ownership.
 * @param report [out, optional] Receives counters for this export only.
 * @return `OVSTAGE_OK` on success or an `ovstage_api_status_t` error code.
 */
ovstage_api_status_t ovstage_population_export_to_destination(ovstage_instance_t* stage,
                                                              ovstage_population_export_destination_handle_t destination,
                                                              const ovstage_population_export_desc_t* desc,
                                                              ovstage_population_export_report_t* report);

/**
 * @brief Enqueue export into a reusable destination.
 *
 * The descriptor graph is copied before return. Work is ordered with create,
 * prior exports, save, and destroy operations on the same source stage.
 * Retrieve the report with `ovstage_population_export_wait_op`.
 *
 * @param stage Source ovstage instance that owns the destination.
 * @param destination Reusable destination created for @p stage.
 * @param desc Export descriptor; the complete graph is copied before return.
 * @return Enqueue result for `ovstage_population_export_wait_op`.
 */
ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_destination(
    ovstage_instance_t* stage,
    ovstage_population_export_destination_handle_t destination,
    const ovstage_population_export_desc_t* desc);

/**
 * @brief Persist the complete current destination and wait for completion.
 *
 * A successful save replaces storage at the captured identifier and leaves the
 * destination open for later export and save calls. `.usd`,
 * `.usda`, `.usdc`, and other resolver/file-format identifiers supported by
 * this OpenUSD build are passed through to OpenUSD. Save is explicit; export
 * and destroy never persist implicitly. OpenUSD does not provide a portable
 * atomic-replacement guarantee, so a failed save may have modified storage.
 *
 * @param stage Source ovstage instance that owns the destination.
 * @param destination Reusable destination created for @p stage.
 * @return `OVSTAGE_OK` on success or an `ovstage_api_status_t` error code.
 */
ovstage_api_status_t ovstage_population_export_destination_save(ovstage_instance_t* stage,
                                                                ovstage_population_export_destination_handle_t destination);

/**
 * @brief Enqueue explicit persistence of the complete current destination.
 *
 * @param stage Source ovstage instance that owns the destination.
 * @param destination Reusable destination created for @p stage.
 * @return Enqueue result for `ovstage_population_export_wait_op`.
 */
ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_save(
    ovstage_instance_t* stage, ovstage_population_export_destination_handle_t destination);

/**
 * @brief Destroy a reusable destination without saving and wait for completion.
 *
 * Already-enqueued work completes first. No new work is accepted after destroy
 * is submitted, and the handle is invalid after completion.
 *
 * @param stage Source ovstage instance that owns the destination.
 * @param destination Reusable destination created for @p stage.
 * @return `OVSTAGE_OK` on success or an `ovstage_api_status_t` error code.
 */
ovstage_api_status_t ovstage_population_export_destination_destroy(
    ovstage_instance_t* stage, ovstage_population_export_destination_handle_t destination);

/**
 * @brief Enqueue ordered destruction of a reusable destination without saving.
 *
 * @param stage Source ovstage instance that owns the destination.
 * @param destination Reusable destination created for @p stage.
 * @return Enqueue result for `ovstage_population_export_wait_op`.
 */
ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_destroy(
    ovstage_instance_t* stage, ovstage_population_export_destination_handle_t destination);

/**
 * @brief Export one snapshot to durable USD storage.
 *
 * Convenience equivalent to creating an EMPTY destination, exporting once,
 * saving, and destroying it. The destination is replaced on successful save.
 *
 * @param stage Source ovstage instance and owner of operation ordering.
 * @param usd_identifier Local path or resolver identifier to replace.
 * @param desc Export descriptor. The caller retains ownership.
 * @param report [out, optional] Receives counters for this export.
 * @return `OVSTAGE_OK` on success or an `ovstage_api_status_t` error code.
 */
ovstage_api_status_t ovstage_population_export_to_usd_file(ovstage_instance_t* stage,
                                                           ovx_string_t usd_identifier,
                                                           const ovstage_population_export_desc_t* desc,
                                                           ovstage_population_export_report_t* report);

/**
 * @brief Enqueue the one-shot EMPTY export-and-save convenience operation.
 *
 * The descriptor graph and identifier are copied before return. Retrieve the
 * export report with `ovstage_population_export_wait_op`.
 *
 * @param stage Source ovstage instance and owner of operation ordering.
 * @param usd_identifier Local path or resolver identifier to replace.
 * @param desc Export descriptor; the complete graph is copied before return.
 * @return Enqueue result for `ovstage_population_export_wait_op`.
 */
ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_file(
    ovstage_instance_t* stage, ovx_string_t usd_identifier, const ovstage_population_export_desc_t* desc);

/**
 * @brief Wait for an asynchronous export operation and consume its stored result.
 *
 *  The wait fences every ovpopulation operation through @p op_id. A timeout
 *  leaves the result available for a later wait and does not cancel work. This
 *  function accepts operations returned by async export, destination create,
 *  destination save, and destination destroy functions. A terminal wait consumes
 *  the result; a second export wait for the same id is invalid. Generic
 *  `ovstage_population_wait_op` may be used first as a fence without consuming
 *  this result. Every accepted operation from this header must eventually receive
 *  a terminal call to this function; abandoning it retains its result and any
 *  destination keepalive until the ovstage instance is destroyed.
 *
 *  @param stage       ovstage instance.
 *  @param op_id       Operation id returned by an async function in this header.
 *  @param timeout_ns  Maximum wait duration, or `OVSTAGE_TIMEOUT_INFINITE`.
 *  @param report      [out, optional] Receives the final export report after
 *                     terminal completion; lifecycle operations return a zeroed
 *                     report. Zeroed before waiting and on timeout.
 *  @return `OVSTAGE_OK` on successful completion, `OVSTAGE_ERROR_TIMEOUT` while
 *          work remains pending, the operation failure status when execution
 *          failed, or `OVSTAGE_ERROR_INVALID_ARGUMENT` for an unknown, consumed,
 *          or unrelated operation id.
 */
ovstage_api_status_t ovstage_population_export_wait_op(
    ovstage_instance_t* stage,
    ovstage_population_op_id_t op_id,
    ovstage_timeout_ns_t timeout_ns,
    ovstage_population_export_report_t* report);

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_POPULATION_EXPORT_H */
