/* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * ovstage population export to USD.
 *
 * Public C ABI for authoring selected ovstage runtime state into a USD layer or
 * a stage opened from its root layer. The header intentionally exposes no
 * OpenUSD C++ types. Callers identify destinations by SdfLayer identifier.
 *
 * Use this API when an application has populated or otherwise authored an
 * ovstage runtime and needs to persist a selected slice back to USD. The
 * synchronous entry points remain the primitive API. Optional asynchronous
 * entry points enqueue the same work on the per-instance ovpopulation FIFO and
 * retain the resolved USD destination until completion.
 *
 * This header holds the export functions of the population family. Export is
 * part of the population family because an export operation and a population
 * operation share one operation stream. Either wait fences the operations of
 * the other. The types of the export functions are defined in
 * ovstage_population_types.h: the policies, the rules, the destination handle,
 * the descriptor and the report. The population family vtable, with the
 * export members, is defined in ovstage_ext_vtables.h.
 *
 * Export authors what ovstage holds, never what the original USD source authored.
 * Each value is read from the ovstage state at the export's ordinal: as population
 * stored it, after the conversions described in `ovstage_population.h` ("Populated
 * value types"), or as the application wrote it since. So a schema `int[]` attribute
 * authored as `uint[]` exports as `int[]`, a `double` value populated into a declared
 * `float` exports with `float` precision, and a property that population did not carry
 * over exports as ovstage holds it, or not at all. Export does not consult the source
 * stage's opinions to restore what population changed or dropped.
 *
 * Async export is a convenience scheduler, not a USD synchronization boundary.
 * Callers must not concurrently read or author the same destination stage/layer,
 * and must fence async work before mixing it with synchronous export.
 * Every accepted async export must eventually receive an export-specific
 * terminal wait; until then its report and destination keepalive remain owned
 * by the ovpopulation state.
 */

#ifndef OVSTAGE_POPULATION_EXPORT_H
#define OVSTAGE_POPULATION_EXPORT_H

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_ext_vtables.h>
#include <ovstage/ovstage_population.h>
#include <ovstage/ovstage_population_predicate.h>
#include <ovstage/ovstage_population_types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

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

/* ═══════════════════════════════════════════════════════════════════════════════
 * The export functions
 *
 * The export functions below that take a stage are `static inline` wrappers
 * of the population family. Each wrapper has two forms. The pair form,
 * `ovstage_population_export_<name>_ex`, takes the population instance,
 * `&stage->ext_instances.population`. It rejects a NULL pointer with
 * `OVSTAGE_ERROR_INVALID_ARGUMENT`, records no error string and resets the
 * outputs as the documentation of the function states. Then it calls its
 * member with the context of that instance,
 * `population->vtable-><name>(population->context, ...)`. The member of an
 * export function is its name without the prefix ovstage_population_export_.
 * The member of ovstage_population_export_wait_op is `export_wait_op`,
 * because `wait_op` is the population wait.
 * The instance form, `ovstage_population_export_<name>`, takes the
 * ovstage_instance_t* and calls the pair form on its population instance, so
 * a NULL stage gets the same answer. C++ overloads
 * `ovstage_population_export_<name>` for both receivers, so C++ code spells
 * both forms with the name of the instance form. ovstage_ext_vtables.h defines the
 * vtable. A member takes the context of its family first, then the other
 * parameters of its function. An implementation that does not serve a
 * function points its member at a stub that answers
 * `OVSTAGE_ERROR_NOT_SUPPORTED` and leaves the outputs unchanged.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief The pair form of ovstage_population_export_destination_create: the same
 *         call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_destination_create for this receiver. */
static inline ovstage_api_status_t ovstage_population_export_destination_create_ex(
    ovstage_population_instance_t* population,
    ovx_string_t usd_identifier,
    ovstage_population_export_destination_mode_t mode,
    ovstage_population_export_destination_handle_t* out_destination)
{
    if (population == NULL)
    {
        if (out_destination != NULL)
            *out_destination = OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE;
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    }
    return population->vtable->destination_create(
        population->context, usd_identifier, mode, out_destination);
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
 * @param out_destination [out] Receives the destination handle on success. An
 *        implementation that does not serve this function returns
 *        `OVSTAGE_ERROR_NOT_SUPPORTED` and leaves it unchanged. On any other
 *        failure, receives the invalid handle.
 * @return `OVSTAGE_OK` on success or an `ovstage_api_status_t` error code.
 */
static inline ovstage_api_status_t ovstage_population_export_destination_create(
    ovstage_instance_t* stage,
    ovx_string_t usd_identifier,
    ovstage_population_export_destination_mode_t mode,
    ovstage_population_export_destination_handle_t* out_destination)
{
    return ovstage_population_export_destination_create_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        usd_identifier, mode, out_destination);
}

/** @brief The pair form of ovstage_population_export_destination_enqueue_create:
 *         the same call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_destination_enqueue_create for this
 *         receiver. */
static inline ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_create_ex(
    ovstage_population_instance_t* population,
    ovx_string_t usd_identifier,
    ovstage_population_export_destination_mode_t mode,
    ovstage_population_export_destination_handle_t* out_destination)
{
    if (population == NULL)
    {
        ovstage_population_enqueue_result_t rejected;
        if (out_destination != NULL)
            *out_destination = OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_POPULATION_INVALID_OP_ID;
        return rejected;
    }
    return population->vtable->destination_enqueue_create(
        population->context, usd_identifier, mode, out_destination);
}

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
 * @param out_destination [out] Receives the reserved destination handle. An
 *        implementation that does not serve this function returns
 *        `OVSTAGE_ERROR_NOT_SUPPORTED` and leaves it unchanged. When the
 *        enqueue is rejected for any other reason, receives the invalid handle.
 * @return Enqueue result for `ovstage_population_export_wait_op`.
 */
static inline ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_create(
    ovstage_instance_t* stage,
    ovx_string_t usd_identifier,
    ovstage_population_export_destination_mode_t mode,
    ovstage_population_export_destination_handle_t* out_destination)
{
    return ovstage_population_export_destination_enqueue_create_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        usd_identifier, mode, out_destination);
}

/** @brief The pair form of ovstage_population_export_to_destination: the same
 *         call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_to_destination for this receiver. */
static inline ovstage_api_status_t ovstage_population_export_to_destination_ex(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination,
    const ovstage_population_export_desc_t* desc,
    ovstage_population_export_report_t* report)
{
    if (population == NULL)
    {
        if (report != NULL)
        {
            ovstage_population_export_report_t zero = { 0 };
            *report = zero;
        }
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    }
    return population->vtable->to_destination(
        population->context, destination, desc, report);
}

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
static inline ovstage_api_status_t ovstage_population_export_to_destination(
    ovstage_instance_t* stage,
    ovstage_population_export_destination_handle_t destination,
    const ovstage_population_export_desc_t* desc,
    ovstage_population_export_report_t* report)
{
    return ovstage_population_export_to_destination_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        destination, desc, report);
}

/** @brief The pair form of ovstage_population_export_enqueue_to_destination: the
 *         same call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_enqueue_to_destination for this receiver. */
static inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_destination_ex(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination,
    const ovstage_population_export_desc_t* desc)
{
    if (population == NULL)
    {
        ovstage_population_enqueue_result_t rejected;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_POPULATION_INVALID_OP_ID;
        return rejected;
    }
    return population->vtable->enqueue_to_destination(
        population->context, destination, desc);
}

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
static inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_destination(
    ovstage_instance_t* stage,
    ovstage_population_export_destination_handle_t destination,
    const ovstage_population_export_desc_t* desc)
{
    return ovstage_population_export_enqueue_to_destination_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        destination, desc);
}

/** @brief The pair form of ovstage_population_export_destination_save: the same
 *         call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_destination_save for this receiver. */
static inline ovstage_api_status_t ovstage_population_export_destination_save_ex(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination)
{
    if (population == NULL)
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    return population->vtable->destination_save(population->context, destination);
}

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
static inline ovstage_api_status_t ovstage_population_export_destination_save(
    ovstage_instance_t* stage, ovstage_population_export_destination_handle_t destination)
{
    return ovstage_population_export_destination_save_ex(
        stage != NULL ? &stage->ext_instances.population : NULL, destination);
}

/** @brief The pair form of ovstage_population_export_destination_enqueue_save: the
 *         same call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_destination_enqueue_save for this receiver. */
static inline ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_save_ex(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination)
{
    if (population == NULL)
    {
        ovstage_population_enqueue_result_t rejected;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_POPULATION_INVALID_OP_ID;
        return rejected;
    }
    return population->vtable->destination_enqueue_save(population->context, destination);
}

/**
 * @brief Enqueue explicit persistence of the complete current destination.
 *
 * @param stage Source ovstage instance that owns the destination.
 * @param destination Reusable destination created for @p stage.
 * @return Enqueue result for `ovstage_population_export_wait_op`.
 */
static inline ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_save(
    ovstage_instance_t* stage, ovstage_population_export_destination_handle_t destination)
{
    return ovstage_population_export_destination_enqueue_save_ex(
        stage != NULL ? &stage->ext_instances.population : NULL, destination);
}

/** @brief The pair form of ovstage_population_export_destination_destroy: the same
 *         call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_destination_destroy for this receiver. */
static inline ovstage_api_status_t ovstage_population_export_destination_destroy_ex(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination)
{
    if (population == NULL)
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    return population->vtable->destination_destroy(population->context, destination);
}

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
static inline ovstage_api_status_t ovstage_population_export_destination_destroy(
    ovstage_instance_t* stage, ovstage_population_export_destination_handle_t destination)
{
    return ovstage_population_export_destination_destroy_ex(
        stage != NULL ? &stage->ext_instances.population : NULL, destination);
}

/** @brief The pair form of ovstage_population_export_destination_enqueue_destroy:
 *         the same call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_destination_enqueue_destroy for this
 *         receiver. */
static inline ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_destroy_ex(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination)
{
    if (population == NULL)
    {
        ovstage_population_enqueue_result_t rejected;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_POPULATION_INVALID_OP_ID;
        return rejected;
    }
    return population->vtable->destination_enqueue_destroy(population->context, destination);
}

/**
 * @brief Enqueue ordered destruction of a reusable destination without saving.
 *
 * @param stage Source ovstage instance that owns the destination.
 * @param destination Reusable destination created for @p stage.
 * @return Enqueue result for `ovstage_population_export_wait_op`.
 */
static inline ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_destroy(
    ovstage_instance_t* stage, ovstage_population_export_destination_handle_t destination)
{
    return ovstage_population_export_destination_enqueue_destroy_ex(
        stage != NULL ? &stage->ext_instances.population : NULL, destination);
}

/** @brief The pair form of ovstage_population_export_to_usd_file: the same call
 *         on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_to_usd_file for this receiver. */
static inline ovstage_api_status_t ovstage_population_export_to_usd_file_ex(
    ovstage_population_instance_t* population,
    ovx_string_t usd_identifier,
    const ovstage_population_export_desc_t* desc,
    ovstage_population_export_report_t* report)
{
    if (population == NULL)
    {
        if (report != NULL)
        {
            ovstage_population_export_report_t zero = { 0 };
            *report = zero;
        }
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    }
    return population->vtable->to_usd_file(
        population->context, usd_identifier, desc, report);
}

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
static inline ovstage_api_status_t ovstage_population_export_to_usd_file(
    ovstage_instance_t* stage,
    ovx_string_t usd_identifier,
    const ovstage_population_export_desc_t* desc,
    ovstage_population_export_report_t* report)
{
    return ovstage_population_export_to_usd_file_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        usd_identifier, desc, report);
}

/** @brief The pair form of ovstage_population_export_enqueue_to_usd_file: the
 *         same call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_enqueue_to_usd_file for this receiver. */
static inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_file_ex(
    ovstage_population_instance_t* population,
    ovx_string_t usd_identifier,
    const ovstage_population_export_desc_t* desc)
{
    if (population == NULL)
    {
        ovstage_population_enqueue_result_t rejected;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_POPULATION_INVALID_OP_ID;
        return rejected;
    }
    return population->vtable->enqueue_to_usd_file(
        population->context, usd_identifier, desc);
}

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
static inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_file(
    ovstage_instance_t* stage, ovx_string_t usd_identifier, const ovstage_population_export_desc_t* desc)
{
    return ovstage_population_export_enqueue_to_usd_file_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        usd_identifier, desc);
}

/** @brief The pair form of ovstage_population_export_to_usd_layer_identifier: the
 *         same call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_to_usd_layer_identifier for this receiver. */
static inline ovstage_api_status_t ovstage_population_export_to_usd_layer_identifier_ex(
    ovstage_population_instance_t* population,
    ovx_string_t usd_layer_identifier,
    const ovstage_population_export_desc_t* desc,
    ovstage_population_export_report_t* report)
{
    if (population == NULL)
    {
        if (report != NULL)
        {
            ovstage_population_export_report_t zero = { 0 };
            *report = zero;
        }
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    }
    return population->vtable->to_usd_layer_identifier(
        population->context, usd_layer_identifier, desc, report);
}

/**
 * @brief Export to a USD layer addressed by identifier.
 *
 * This synchronous call authors directly in place. It bypasses the
 * population/export FIFO, provides no rollback, and requires caller-controlled
 * serialization of the destination. The implementation first looks for an already-open SdfLayer with this
 * identifier, then falls back to opening it from storage. Anonymous in-memory
 * layers are supported when they are already resident in USD's layer registry.
 *
 * @param stage Source ovstage instance.
 * @param usd_layer_identifier Identifier of the destination USD layer.
 * @param desc Export descriptor. The caller retains ownership.
 * @param report [out, optional] Receives export diagnostic counters.
 * @return `OVSTAGE_OK` on success or an `ovstage_api_status_t` error code.
 */
static inline ovstage_api_status_t ovstage_population_export_to_usd_layer_identifier(
    ovstage_instance_t* stage,
    ovx_string_t usd_layer_identifier,
    const ovstage_population_export_desc_t* desc,
    ovstage_population_export_report_t* report)
{
    return ovstage_population_export_to_usd_layer_identifier_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        usd_layer_identifier, desc, report);
}

/** @brief The pair form of ovstage_population_export_to_usd_stage_identifier: the
 *         same call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_to_usd_stage_identifier for this receiver. */
static inline ovstage_api_status_t ovstage_population_export_to_usd_stage_identifier_ex(
    ovstage_population_instance_t* population,
    ovx_string_t usd_stage_root_layer_identifier,
    const ovstage_population_export_desc_t* desc,
    ovstage_population_export_report_t* report)
{
    if (population == NULL)
    {
        if (report != NULL)
        {
            ovstage_population_export_report_t zero = { 0 };
            *report = zero;
        }
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    }
    return population->vtable->to_usd_stage_identifier(
        population->context, usd_stage_root_layer_identifier, desc, report);
}

/**
 * @brief Export to a USD stage addressed by its root-layer identifier.
 *
 * This synchronous call opens the stage and authors directly into that stage's
 * default edit target. It bypasses the population/export FIFO, provides no
 * rollback, and requires caller-controlled serialization of the destination.
 * Caller-owned custom edit-target selection is not available through this public
 * C API.
 *
 * @param stage Source ovstage instance.
 * @param usd_stage_root_layer_identifier Root-layer identifier of the destination stage.
 * @param desc Export descriptor. The caller retains ownership.
 * @param report [out, optional] Receives export diagnostic counters.
 * @return `OVSTAGE_OK` on success or an `ovstage_api_status_t` error code.
 */
static inline ovstage_api_status_t ovstage_population_export_to_usd_stage_identifier(
    ovstage_instance_t* stage,
    ovx_string_t usd_stage_root_layer_identifier,
    const ovstage_population_export_desc_t* desc,
    ovstage_population_export_report_t* report)
{
    return ovstage_population_export_to_usd_stage_identifier_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        usd_stage_root_layer_identifier, desc, report);
}

/** @brief The pair form of ovstage_population_export_enqueue_to_usd_layer_identifier:
 *         the same call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_enqueue_to_usd_layer_identifier for this
 *         receiver. */
static inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_layer_identifier_ex(
    ovstage_population_instance_t* population,
    ovx_string_t usd_layer_identifier,
    const ovstage_population_export_desc_t* desc)
{
    if (population == NULL)
    {
        ovstage_population_enqueue_result_t rejected;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_POPULATION_INVALID_OP_ID;
        return rejected;
    }
    return population->vtable->enqueue_to_usd_layer_identifier(
        population->context, usd_layer_identifier, desc);
}

/**
 * @brief Enqueue export to a USD layer addressed by identifier.
 *
 *  The descriptor and every nested array/string are copied before this function
 *  returns. The destination layer is resolved and retained at enqueue time.
 *  Work runs on the same per-instance FIFO used by ovpopulation operations.
 *
 *  The caller must not concurrently read or author the destination layer until
 *  the operation has completed. Synchronous export is not serialized with this
 *  queue and requires the same caller-provided synchronization.
 *
 *  @param stage                 ovstage instance.
 *  @param usd_layer_identifier  Identifier of an existing USD layer.
 *  @param desc                  Export descriptor; the complete descriptor graph
 *                               is copied before this function returns.
 *  @return Enqueue result (see `ovstage_population_enqueue_result_t`). A rejected
 *          enqueue is reported in `status`; execution failures are returned by
 *          `ovstage_population_export_wait_op` with export diagnostics available
 *          through `ovstage_population_get_last_error`.
 */
static inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_layer_identifier(
    ovstage_instance_t* stage,
    ovx_string_t usd_layer_identifier,
    const ovstage_population_export_desc_t* desc)
{
    return ovstage_population_export_enqueue_to_usd_layer_identifier_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        usd_layer_identifier, desc);
}

/** @brief The pair form of ovstage_population_export_enqueue_to_usd_stage_identifier:
 *         the same call on the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_enqueue_to_usd_stage_identifier for this
 *         receiver. */
static inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_stage_identifier_ex(
    ovstage_population_instance_t* population,
    ovx_string_t usd_stage_root_layer_identifier,
    const ovstage_population_export_desc_t* desc)
{
    if (population == NULL)
    {
        ovstage_population_enqueue_result_t rejected;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_POPULATION_INVALID_OP_ID;
        return rejected;
    }
    return population->vtable->enqueue_to_usd_stage_identifier(
        population->context, usd_stage_root_layer_identifier, desc);
}

/**
 * @brief Enqueue export to a USD stage addressed by its root-layer identifier.
 *
 *  The stage is opened and retained at enqueue time. Export authors into its
 *  default edit target. The caller must not read or author that stage or its
 *  layers concurrently with the export.
 *
 *  @param stage                            ovstage instance.
 *  @param usd_stage_root_layer_identifier  Root-layer identifier of the destination stage.
 *  @param desc                             Export descriptor; the complete descriptor graph
 *                                          is copied before this function returns.
 *  @return Enqueue result. Admission failures are reported immediately; execution
 *          failures are returned by `ovstage_population_export_wait_op`.
 */
static inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_stage_identifier(
    ovstage_instance_t* stage,
    ovx_string_t usd_stage_root_layer_identifier,
    const ovstage_population_export_desc_t* desc)
{
    return ovstage_population_export_enqueue_to_usd_stage_identifier_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        usd_stage_root_layer_identifier, desc);
}

/** @brief The pair form of ovstage_population_export_wait_op: the same call on
 *         the population instance,
 *         `&stage->ext_instances.population`. C++ overloads
 *         ovstage_population_export_wait_op for this receiver. */
static inline ovstage_api_status_t ovstage_population_export_wait_op_ex(
    ovstage_population_instance_t* population,
    ovstage_population_op_id_t op_id,
    ovstage_timeout_ns_t timeout_ns,
    ovstage_population_export_report_t* report)
{
    if (population == NULL)
    {
        if (report != NULL)
        {
            ovstage_population_export_report_t zero = { 0 };
            *report = zero;
        }
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    }
    return population->vtable->export_wait_op(
        population->context, op_id, timeout_ns, report);
}

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
 *                     report. An implementation that does not serve this
 *                     function returns `OVSTAGE_ERROR_NOT_SUPPORTED` and leaves
 *                     it unchanged. Zeroed before waiting, on timeout and for a
 *                     NULL @p stage.
 *  @return `OVSTAGE_OK` on successful completion, `OVSTAGE_ERROR_TIMEOUT` while
 *          work remains pending, the operation failure status when execution
 *          failed, or `OVSTAGE_ERROR_INVALID_HANDLE` for any op id that does
 *          not resolve to a live export/destination operation on this instance
 *          — an id this instance's operation stream never issued (zero, issued
 *          by a different instance, or by a stream this instance has since
 *          replaced), one already consumed by an earlier terminal wait, or one
 *          that never identified an export/destination operation.
 */
static inline ovstage_api_status_t ovstage_population_export_wait_op(
    ovstage_instance_t* stage,
    ovstage_population_op_id_t op_id,
    ovstage_timeout_ns_t timeout_ns,
    ovstage_population_export_report_t* report)
{
    return ovstage_population_export_wait_op_ex(
        stage != NULL ? &stage->ext_instances.population : NULL,
        op_id, timeout_ns, report);
}

#ifdef __cplusplus
} /* extern "C" */

/* C++ spells the pair form with the name of the instance form. A literal NULL or
 * nullptr first argument is ambiguous between the two receivers: pass a
 * typed null, or call the _ex form. */
inline ovstage_api_status_t ovstage_population_export_destination_create(
    ovstage_population_instance_t* population, ovx_string_t usd_identifier,
    ovstage_population_export_destination_mode_t mode,
    ovstage_population_export_destination_handle_t* out_destination)
{
    return ovstage_population_export_destination_create_ex(
        population, usd_identifier, mode, out_destination);
}

inline ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_create(
    ovstage_population_instance_t* population, ovx_string_t usd_identifier,
    ovstage_population_export_destination_mode_t mode,
    ovstage_population_export_destination_handle_t* out_destination)
{
    return ovstage_population_export_destination_enqueue_create_ex(
        population, usd_identifier, mode, out_destination);
}

inline ovstage_api_status_t ovstage_population_export_to_destination(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination,
    const ovstage_population_export_desc_t* desc, ovstage_population_export_report_t* report)
{
    return ovstage_population_export_to_destination_ex(population, destination, desc, report);
}

inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_destination(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination,
    const ovstage_population_export_desc_t* desc)
{
    return ovstage_population_export_enqueue_to_destination_ex(population, destination, desc);
}

inline ovstage_api_status_t ovstage_population_export_destination_save(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination)
{
    return ovstage_population_export_destination_save_ex(population, destination);
}

inline ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_save(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination)
{
    return ovstage_population_export_destination_enqueue_save_ex(population, destination);
}

inline ovstage_api_status_t ovstage_population_export_destination_destroy(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination)
{
    return ovstage_population_export_destination_destroy_ex(population, destination);
}

inline ovstage_population_enqueue_result_t ovstage_population_export_destination_enqueue_destroy(
    ovstage_population_instance_t* population,
    ovstage_population_export_destination_handle_t destination)
{
    return ovstage_population_export_destination_enqueue_destroy_ex(population, destination);
}

inline ovstage_api_status_t ovstage_population_export_to_usd_file(
    ovstage_population_instance_t* population, ovx_string_t usd_identifier,
    const ovstage_population_export_desc_t* desc, ovstage_population_export_report_t* report)
{
    return ovstage_population_export_to_usd_file_ex(population, usd_identifier, desc, report);
}

inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_file(
    ovstage_population_instance_t* population, ovx_string_t usd_identifier,
    const ovstage_population_export_desc_t* desc)
{
    return ovstage_population_export_enqueue_to_usd_file_ex(population, usd_identifier, desc);
}

inline ovstage_api_status_t ovstage_population_export_to_usd_layer_identifier(
    ovstage_population_instance_t* population, ovx_string_t usd_layer_identifier,
    const ovstage_population_export_desc_t* desc, ovstage_population_export_report_t* report)
{
    return ovstage_population_export_to_usd_layer_identifier_ex(
        population, usd_layer_identifier, desc, report);
}

inline ovstage_api_status_t ovstage_population_export_to_usd_stage_identifier(
    ovstage_population_instance_t* population,
    ovx_string_t usd_stage_root_layer_identifier,
    const ovstage_population_export_desc_t* desc, ovstage_population_export_report_t* report)
{
    return ovstage_population_export_to_usd_stage_identifier_ex(
        population, usd_stage_root_layer_identifier, desc, report);
}

inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_layer_identifier(
    ovstage_population_instance_t* population, ovx_string_t usd_layer_identifier,
    const ovstage_population_export_desc_t* desc)
{
    return ovstage_population_export_enqueue_to_usd_layer_identifier_ex(
        population, usd_layer_identifier, desc);
}

inline ovstage_population_enqueue_result_t ovstage_population_export_enqueue_to_usd_stage_identifier(
    ovstage_population_instance_t* population,
    ovx_string_t usd_stage_root_layer_identifier,
    const ovstage_population_export_desc_t* desc)
{
    return ovstage_population_export_enqueue_to_usd_stage_identifier_ex(
        population, usd_stage_root_layer_identifier, desc);
}

inline ovstage_api_status_t ovstage_population_export_wait_op(
    ovstage_population_instance_t* population, ovstage_population_op_id_t op_id,
    ovstage_timeout_ns_t timeout_ns, ovstage_population_export_report_t* report)
{
    return ovstage_population_export_wait_op_ex(population, op_id, timeout_ns, report);
}
#endif

#endif /* OVSTAGE_POPULATION_EXPORT_H */
