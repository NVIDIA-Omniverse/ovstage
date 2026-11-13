/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage_history.h
 * @brief Runtime selection of the (prim, attribute) pairs whose earlier
 *        values a history-capable instance retains.
 *
 * @details
 * **Experimental.** Everything in this header -- OVSTAGE_HISTORY_EXTENSION,
 * ovstage_history_ext_t and the inline helpers -- is experimental. The API,
 * the ABI and the semantics can change between releases without a
 * deprecation period. The implementation "history_selective" that serves the
 * extension is experimental too. Isolate the use of this header behind an
 * application adapter. Check availability at run time.
 *
 * The selection API is a versioned extension table discovered through the
 * query_extension slot of the data-plane vtable. It needs no additional
 * exported symbols: the inline helpers below resolve the table and call it.
 *
 * A history-capable implementation serves the extension: it turns history
 * on for the pairs the application selects. The selected pairs retain their
 * earlier sealed values, up to OVSTAGE_CONFIG_RETENTION_WINDOW. Every other
 * cell keeps the latest value. An implementation that keeps history off does
 * not serve the extension: every helper then returns
 * OVSTAGE_ERROR_NOT_SUPPORTED and does not change the instance.
 *
 * @remark The implementation "history_selective" is history-capable. Create
 *         an instance of it with an ovstage_instance_desc_t whose
 *         `implementation` member points to that name (ovstage_types.h). The
 *         default implementation "history_latest" keeps history off.
 */

#ifndef OVSTAGE_HISTORY_H
#define OVSTAGE_HISTORY_H

#include <ovstage/ovstage_api/ovstage_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Canonical extension name of the selection table. */
#define OVSTAGE_HISTORY_EXTENSION "ovstage.history.v1"
/** Version of ovstage_history_ext_t served under OVSTAGE_HISTORY_EXTENSION. */
#define OVSTAGE_HISTORY_VERSION 1u

/**
 * Selection table of a history-capable instance.
 *
 * `enable` snapshots the resolved prim membership of an existing, completed
 * query handle and copies the attribute names. Use ovstage_query() with an
 * ovstage_filter_t, or ovstage_query_from_path_list() for exact prims. The
 * query's attribute projection does not select anything: `attributes`
 * explicitly names the retained attributes. Separate calls can select
 * different attributes on different prims; neighboring prims and other
 * attributes of the same prim stay independent.
 *
 * `enable` is additive and idempotent: existing selections and retained
 * values remain intact. `disable` stops retention and discards the retained
 * history of the selected pairs while preserving their current values.
 * Enabling again starts a fresh baseline. Query release does not undo a
 * selection. Later matches of the same filter are not enrolled implicitly;
 * query and enable again after discovery.
 *
 * Call both operations on a quiescent, globally sealed instance: finish
 * pending stage and renderer work, release reads, groups and maps, and seal
 * the current state first. Serialize these synchronous configuration calls
 * against other uses of the instance. The query handle itself may stay alive.
 * Newly selected values that exist are copied at the current sealed global
 * watermark and are readable when the call succeeds. Neither operation
 * advances the watermark or changes live values. An absent attribute begins
 * recording on its first write; no value before enrollment is reconstructed.
 * Existing query handles adopt the new selection for later operations.
 *
 * If copying the baseline fails, mutations and publication are blocked until
 * the identical membership and attribute list is retried successfully, or
 * the instance is destroyed. A failed copy never activates a partial
 * selection.
 *
 * At least one explicit attribute is required. Names and tokens use the
 * instance's path dictionary; input storage is borrowed only for the call.
 * The derived built-ins usd-path, usd-parent, usd-children and usd-active
 * cannot be selected (OVSTAGE_ERROR_INVALID_ARGUMENT). Retention capacity
 * (OVSTAGE_CONFIG_RETENTION_WINDOW) is a separate process setting.
 *
 * @remark The composed instance is not a historical snapshot of the whole
 *         stage. Only explicitly selected pairs retain earlier values; other
 *         cells keep the latest-value behavior documented in ovstage_api.h.
 * @remark Every mutation of a selected cell is recorded in history at the
 *         ordinal of the mutation: a write through a path-list query
 *         (ovstage_query_from_path_list()) or a filter query
 *         (ovstage_query()), with a host or a device payload, a map commit,
 *         a delete, population, hierarchy computation and ovstage_clone().
 *         The runtime reports each committed mutation to the history layer
 *         before the next operation runs, so two mutations of one cell
 *         between two floor advances keep both values. The derived outputs
 *         of the hierarchy update that a global floor advance runs are
 *         recorded at the sealed ordinal. A clone copies the latest values
 *         only; the retained history of the source prim is not cloned. The
 *         history layer records a mutation after the live storage committed
 *         it, so history never holds a value the live storage never held.
 * @remark The history writes of the selected cells complete after the
 *         mutation that caused them returns. A floor advance waits for the
 *         writes of the sealed range. When one of them
 *         did not land, the floor advance fails with OVSTAGE_ERROR_OP_FAILED
 *         and names the failure, the live storage stays sealed, and the
 *         history floor stays behind: a historical read of a selected pair
 *         is refused with OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION until a floor
 *         advance succeeds. Latest reads are unaffected. The retry of the
 *         floor advance records the live value of each such cell at the
 *         floor ordinal. A mutation submitted while a floor advance runs
 *         that reconciliation is refused with OVSTAGE_ERROR_QUEUE_FULL.
 *         Retry it after the floor advance returns. The retained history
 *         holds a bounded number of open ordinals: unsealed ordinals that
 *         received a selected write. The history write of a selected cell at
 *         a further ordinal fails, and the next floor advance reports it as
 *         above. The retry of that floor advance seals the earlier ordinals
 *         first, so the retained history has room for the values it records
 *         at the floor. Only a whole-stage retry (OVSTAGE_SCOPE_ALL) frees
 *         that room: a scoped floor advance seals the ordinals of its
 *         attributes only.
 * @remark A latest read (no start ordinal, end ordinal OVSTAGE_ORDINAL_MAX)
 *         is the answer of the live storage for every query. A read with a
 *         start ordinal is a historical read even at end ordinal
 *         OVSTAGE_ORDINAL_MAX. A
 *         historical read through a filter query covers the prims the filter
 *         reports at the time of the read; it does not evaluate the filter
 *         against the past scene. A prim that left the filter after the
 *         ordinal is absent, a prim that joined it afterwards is present
 *         with its latest value. A mixed query answers with one or more read
 *         groups per backend, and each group references the path list of its
 *         own sub-query. A read that names two spellings of one attribute
 *         reports the retained group under the spelling named first. A read
 *         that spans selected and unselected pairs fails when one backend
 *         rejects it, for example OVSTAGE_ERROR_OUT_OF_RANGE below the
 *         retention frontier of the live storage; split the query by selected
 *         and unselected pairs to read across that frontier.
 * @remark A renderer attached through OVRTX and created with
 *         OVRTX_CONFIG_ENABLE_HISTORY_PRESENTATION presents the retained
 *         numeric values of the selected pairs it reads when it steps at a
 *         committed ordinal below the write floor (ovrtx_step_with_stage). A
 *         renderer without that key makes no history calls, and such a step
 *         fails with a reason that names the key. The live storage holds the
 *         presented values for the duration of that render and returns to
 *         the live values afterwards. Ordinals, floors and retained history
 *         do not change. The renderer derives world transforms and
 *         bounds from the presented inputs, so enroll the inputs it derives
 *         from, for example `omni:xform` and `extent`. Such a step needs a
 *         sealed, quiescent instance with released reads, groups and maps.
 *         Stage operations issued during the render are refused with
 *         OVSTAGE_ERROR_QUEUE_FULL and succeed again after the step. A step
 *         whose presentation meets a publication in progress fails with
 *         OVSTAGE_ERROR_QUEUE_FULL. Retry the step. A step fails when the
 *         live values cannot be restored after the render. The next step
 *         restores them first.
 * @note Experimental. This table, its inline helpers and the implementation
 *       "history_selective" are experimental. Their API, ABI and semantics
 *       can change between releases without a deprecation period.
 */
typedef struct ovstage_history_ext_t
{
    uint32_t struct_size;
    uint32_t version;
    ovstage_api_status_t (*enable)(ovstage_data_plane_context_t* context,
                                   ovstage_query_handle_t query,
                                   const ovx_token_or_string_t* attributes,
                                   size_t attribute_count);
    ovstage_api_status_t (*disable)(ovstage_data_plane_context_t* context,
                                    ovstage_query_handle_t query,
                                    const ovx_token_or_string_t* attributes,
                                    size_t attribute_count);
} ovstage_history_ext_t;

/**
 * Resolve the selection table and apply one enable or disable call.
 *
 * @return OVSTAGE_ERROR_INVALID_ARGUMENT for a NULL instance, query,
 *         attribute array or an empty attribute list;
 *         OVSTAGE_ERROR_NOT_SUPPORTED when the instance is not
 *         history-capable; otherwise the extension's status.
 */
static inline ovstage_api_status_t ovstage_history_apply(ovstage_instance_t* instance,
                                                         ovstage_query_handle_t query,
                                                         const ovx_token_or_string_t* attributes,
                                                         size_t attribute_count,
                                                         bool enabled)
{
    const void* table = 0;
    const ovstage_history_ext_t* extension;
    ovstage_api_status_t status;
    if (!instance || !instance->data_plane.context || !instance->data_plane.vtable ||
        query.raw_id == OVSTAGE_INVALID_QUERY_HANDLE.raw_id || !attributes || !attribute_count)
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    if (!instance->data_plane.vtable->query_extension)
        return OVSTAGE_ERROR_NOT_SUPPORTED;
    status = instance->data_plane.vtable->query_extension(
        instance->data_plane.context, OVSTAGE_HISTORY_EXTENSION, &table);
    if (status != OVSTAGE_OK)
        return status == OVSTAGE_ERROR_NOT_FOUND ? OVSTAGE_ERROR_NOT_SUPPORTED : status;
    extension = (const ovstage_history_ext_t*)table;
    if (!extension || extension->struct_size < sizeof(*extension) || extension->version != OVSTAGE_HISTORY_VERSION ||
        !extension->enable || !extension->disable)
        return OVSTAGE_ERROR_NOT_SUPPORTED;
    return enabled ? extension->enable(instance->data_plane.context, query, attributes, attribute_count) :
                     extension->disable(instance->data_plane.context, query, attributes, attribute_count);
}

/** Select the given attributes on the prims matched by @p query. */
static inline ovstage_api_status_t ovstage_enable_history(ovstage_instance_t* instance,
                                                          ovstage_query_handle_t query,
                                                          const ovx_token_or_string_t* attributes,
                                                          size_t attribute_count)
{
    return ovstage_history_apply(instance, query, attributes, attribute_count, true);
}

/** Stop retaining the given attributes on the prims matched by @p query. */
static inline ovstage_api_status_t ovstage_disable_history(ovstage_instance_t* instance,
                                                           ovstage_query_handle_t query,
                                                           const ovx_token_or_string_t* attributes,
                                                           size_t attribute_count)
{
    return ovstage_history_apply(instance, query, attributes, attribute_count, false);
}

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_HISTORY_H */
