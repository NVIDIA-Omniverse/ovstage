/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage.h
 * @brief OVStage instance lifecycle — creation and destruction of the
 *        ovstage data-plane backend.
 *
 * @details
 * The OVStage data-plane API (types, handles, operations, diagnostics) is
 * declared in `ovstage_api/ovstage_api.h` as a vtable contract and exposed
 * to callers through inline wrappers in `ovstage_api/ovstage_api_utils.h`
 * (transitively included from `ovstage_api.h`) and sibling extension APIs.
 *
 * This header adds the pieces specific to the ovstage backend: an instance
 * descriptor, create/destroy entry points, and backend-owned helpers. Once an
 * `ovstage_instance_t*` has been produced by `ovstage_create_instance`,
 * call the generic `ovstage_*` wrappers declared in `ovstage_api.h` and the
 * backend-specific entry points declared here to drive it.
 *
 * @version 0.1.1
 * @date 2026-05-27
 */

#ifndef OVSTAGE_H
#define OVSTAGE_H

#include "ovstage_api/ovstage_api.h"
#include "ovstage_config.h"
#include "ovstage_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════════
 * Process Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Acquire a reference to process-scoped ovstage state.
 *
 * Reference-counted: each successful call must be balanced by one
 * ovstage_shutdown(). Process-scoped state is set up on the first reference
 * (0 -> 1) and torn down on the last (1 -> 0). Instances created with
 * ovstage_create_instance also hold a reference, so process state stays alive
 * while any instance exists.
 *
 * Calling ovstage_initialize() is optional when linking the ovstage shared
 * library directly: instance creation acquires process state on demand. Call it
 * explicitly to (1) pin process-scoped state independent of instance lifetime —
 * so process-level resources stay alive when no instance is live or across
 * instance create/destroy churn — and (2) bootstrap the runtime eagerly, so a
 * framework/plugin failure surfaces here instead of at the first
 * create_instance.
 *
 * @param config Optional process configuration (see ovstage_config_t / the
 *               entry builders in ovstage_config.h).
 *               OVSTAGE_CONFIG_BINARY_PACKAGE_ROOT_PATH is consumed by the
 *               static loader and must be supplied before any other ovstage_*
 *               call when a non-default root is needed.
 *               OVSTAGE_CONFIG_RUNTIME_DEFAULT_HIERARCHY_COMPUTATION_MODEL is
 *               consumed by the runtime and controls automatic hierarchy
 *               updates and the RUNTIME_DEFAULT selector for new instances.
 *               A runtime setting supplied after a process reference already
 *               exists must match the active setting.
 * @return OVSTAGE_OK on success; OVSTAGE_ERROR_INVALID_ARGUMENT for malformed,
 *         duplicate, unknown, or conflicting runtime configuration.
 * @post Balance every successful call with one ovstage_shutdown().
 */
ovstage_api_status_t ovstage_initialize(const ovstage_config_t* config);

/**
 * @brief Release one reference acquired by ovstage_initialize().
 *
 * On the last outstanding reference (explicit initializations and live
 * instances both count), process-scoped state is torn down.
 *
 * @return OVSTAGE_OK on success; OVSTAGE_ERROR_INVALID_ARGUMENT if called
 *         without a matching outstanding ovstage_initialize().
 */
ovstage_api_status_t ovstage_shutdown(void);

/* ═══════════════════════════════════════════════════════════════════════════════
 * Logging
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Install a process-global log callback.
 *
 * Routes ovstage's log messages (and messages from its USD support layer)
 * to @p callback. Process-global: one callback for the whole process, replacing
 * any previous one. Pass @p callback = NULL to disable delivery.
 *
 * Messages are delivered asynchronously on a dedicated dispatcher thread, so the
 * callback never runs on the logging hot path and invocations are serialized.
 * Use ovstage_flush_log to force pending messages through before a checkpoint.
 *
 * The dispatcher thread and runtime log hook are created lazily on the first
 * non-null callback and torn down when the callback is cleared, so a client that
 * never installs a callback pays nothing. Passing @p callback = NULL flushes any
 * pending messages to the current callback and then disables delivery.
 *
 * Requires the ovstage runtime to be bootstrapped (an ovstage_initialize() or a
 * live instance); otherwise returns OVSTAGE_ERROR_OP_FAILED. Keep a process
 * reference (or clear the callback) before releasing the last one — the last
 * ovstage_shutdown / instance destroy flushes and tears the bridge down.
 *
 * @param severity       Default severity threshold for channels not matched by a
 *                       rule in @p channel_filter. Messages below it are dropped.
 *                       OVSTAGE_LOG_NONE drops all by default.
 * @param channel_filter Optional comma-separated `<channel>=<level>` list (e.g.
 *                       "omni.ovstage=verbose"). NULL applies
 *                       @p severity uniformly. Levels: verbose|debug|info|warn|
 *                       warning|error|fatal|none.
 * @param callback       Callback to receive messages, or NULL to disable.
 * @param user_data      Context passed to each callback invocation.
 * @return OVSTAGE_OK on success; OVSTAGE_ERROR_INVALID_ARGUMENT if the filter
 *         string fails to parse; OVSTAGE_ERROR_OP_FAILED if the runtime is not
 *         bootstrapped.
 */
ovstage_api_status_t ovstage_set_log_callback(
    ovstage_log_severity_t severity,
    const ovx_string_t*    channel_filter,
    ovstage_log_callback_t callback,
    void*                  user_data);

/**
 * @brief Block until all log messages emitted before this call have been
 *        delivered through the callback.
 *
 * Point-in-time barrier: messages produced concurrently with or after this call
 * are not guaranteed to be included. Returns OVSTAGE_OK immediately if no
 * callback is installed (nothing is buffered).
 *
 * @param timeout Max time to wait. OVSTAGE_TIMEOUT_INFINITE blocks; 0 polls.
 * @return OVSTAGE_OK if drained (or no callback installed); OVSTAGE_ERROR_TIMEOUT
 *         if not drained within @p timeout.
 */
ovstage_api_status_t ovstage_flush_log(ovstage_timeout_ns_t timeout);

/* ═══════════════════════════════════════════════════════════════════════════════
 * Instance Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Create an ovstage instance.
 * @param desc Configuration descriptor.
 * @param[out] out_instance Receives the new instance (vtable + context bundle).
 * @return OVSTAGE_OK on success.
 * @post Caller owns the instance; destroy via ovstage_destroy_instance.
 */
ovstage_api_status_t ovstage_create_instance(
    const ovstage_instance_desc_t* desc,
    ovstage_instance_t**           out_instance);

/**
 * @brief Destroy an ovstage instance and release all resources.
 * @param instance Instance to destroy. May be NULL (no-op).
 * @pre All operations, handles, and result payloads from this instance must be
 *      released first.
 * @pre No other thread may invoke any `ovstage_*` wrapper, vtable slot, or
 *      internal accessor on this `instance` while or after destroy runs. The
 *      bundle and its context are deallocated before this call returns; any
 *      concurrent or post-destroy use is a use-after-free.
 */
ovstage_api_status_t ovstage_destroy_instance(ovstage_instance_t* instance);

/**
 * @brief Get the source USD stage id backing this ovstage instance.
 *
 * TEMPORARY: this accessor is a stopgap and is expected to be removed. Consumers
 * should read stage data (units, attributes, relationships) through the ovstage
 * query/read API rather than reaching back to the USD stage; do not build a
 * lasting dependency on this entry point.
 *
 * An ovstage instance with USD population support mirrors a USD stage (populated
 * by ovpopulation or the host). This returns the runtime identifier of that
 * source stage.
 *
 * @param instance The ovstage instance.
 * @param[out] out_usd_stage_id Receives the USD stage id.
 * @return OVSTAGE_OK on success.
 * @return OVSTAGE_ERROR_INVALID_ARGUMENT if @p instance or @p out_usd_stage_id is NULL.
 * @return OVSTAGE_ERROR_NOT_SUPPORTED if the instance has no associated USD stage.
 */
ovstage_api_status_t ovstage_get_usd_stage_id(
    ovstage_instance_t* instance,
    uint64_t*           out_usd_stage_id);

/**
 * @brief Enqueue an asynchronous operation to clone the subtree under the source
 *        path to one or more target paths in the runtime stage representation.
 *
 * The source path must exist in the stage; a missing source is rejected with
 * `OVSTAGE_ERROR_NOT_FOUND` (surfaced through `wait_op`). The target paths must not
 * already exist; a target that does is rejected with `OVSTAGE_ERROR_PRIM_NOT_FOUND`
 * (the create-only convention shared with INSERT-mode writes), before any prim is
 * created, so a batch mixing fresh and existing targets clones nothing.
 *
 * Data-plane peer of ovrtx's `ovrtx_clone_usd` (the `_usd` postfix is dropped),
 * but — like `write_attribute` / `delete_attributes` — clone is an ordinal-keyed
 * write: it carries an @p ordinal and is rejected with
 * `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` (surfaced through `wait_op`) when
 * @p ordinal is at or below the effective seal of any attribute it would touch, so
 * it can never mutate sealed ordinals. The call returns immediately with an
 * `ovstage_enqueue_result_t` (status + `op_index`); await completion through the vtable
 * `wait_op` (and release it with `release_op`) like any other data-plane operation.
 *
 * Path-bearing values that target the source subtree are rebased to the corresponding
 * target subtree. This includes relationship targets (e.g. `material:binding`,
 * `skel:skeleton`), scalar/array path values, and USD attribute connections.
 * Paths outside the source subtree are copied unchanged, so clones can continue to
 * reference shared materials and other shared resources. Cloned attribute values,
 * including relationship targets and attribute connections, are ordinal-change-tracked.
 * Scene hierarchy changes, such as the source/target parents' child lists, are not.
 *
 * @param instance         The ovstage instance.
 * @param source_path      Path to the source subtree to clone.
 * @param target_paths     Array of target paths to clone to.
 * @param num_target_paths Number of target paths to clone to.
 * @param ordinal          Ordinal for this clone. Must be greater than the effective
 *                         seal of every attribute the clone reproduces (admission is
 *                         per-attribute over the source subtree, like
 *                         `write_attribute`), evaluated at execution time.
 * @return Enqueue result with status + op_index.
 * - **status == OVSTAGE_OK** if the operation was enqueued successfully.
 * - a non-OK status if the enqueue was rejected (e.g. invalid arguments, or the
 *   required runtime capability is unavailable). Per-op execution errors — including
 *   `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`, `OVSTAGE_ERROR_NOT_FOUND` (missing
 *   source), and `OVSTAGE_ERROR_PRIM_NOT_FOUND` (a target already exists) — are
 *   surfaced through `wait_op`.
 */
ovstage_enqueue_result_t ovstage_clone(
    ovstage_instance_t* instance,
    ovx_string_t        source_path,
    const ovx_string_t* target_paths,
    size_t              num_target_paths,
    ovstage_ordinal_t   ordinal);

/**
 * @brief Enqueue a backend hierarchy lookup for a batch of prims.
 *
 * `prim_paths` is an immutable ordered `ovx_primpath_list_t`. The list is
 * copied during enqueue; callers may destroy their user-owned list after this
 * call returns OVSTAGE_OK. The ovstage backend answers parent/children/sibling
 * hierarchy lookups from its tracked prim-parent hierarchy. Fetch the per-input
 * results with ovstage_fetch_hierarchy_result.
 *
 * Hierarchy lookups observe the latest sealed stage state. Returned relation
 * information is resolved from attribute data at the latest sealed ordinal(s).
 *
 * @param instance The ovstage instance.
 * @param prim_paths Ordered prim path list to inspect.
 * @param ordinal Stage ordinal requested by this lookup.
 * @param relation Hierarchy direction to return for each input prim.
 * @param[out] out_hierarchy_handle Receives the lookup handle.
 * @return Enqueue result with status + op_index. Per-op execution failures are
 *         surfaced through ovstage_wait_op + ovstage_get_last_op_error.
 */
ovstage_enqueue_result_t ovstage_get_hierarchy(
    ovstage_instance_t*             instance,
    ovx_primpath_list_t             prim_paths,
    ovstage_ordinal_t               ordinal,
    ovstage_hierarchy_relation_t    relation,
    ovstage_hierarchy_handle_t*     out_hierarchy_handle);

/**
 * @brief Fetch the result of a completed hierarchy lookup batch.
 *
 * Use ovstage_wait_op with the op_index returned by ovstage_get_hierarchy to wait
 * for completion before fetching. This function does not block.
 *
 * @return OVSTAGE_OK on success,
 *         OVSTAGE_ERROR_TIMEOUT if the producer op has not completed,
 *         OVSTAGE_ERROR_OP_FAILED if the underlying enqueue failed.
 *
 * @post On OVSTAGE_OK, pointers in `*out_result` remain valid until
 *       ovstage_release_hierarchy_result.
 */
ovstage_api_status_t ovstage_fetch_hierarchy_result(
    ovstage_instance_t*          instance,
    ovstage_hierarchy_handle_t   hierarchy_handle,
    ovstage_hierarchy_result_t*  out_result);

/**
 * @brief Release a fetched hierarchy result payload.
 *
 * After this call, pointers in `*result` are invalid.
 */
ovstage_api_status_t ovstage_release_hierarchy_result(
    ovstage_instance_t*                 instance,
    const ovstage_hierarchy_result_t*   result);

/**
 * @brief Enqueue release of a hierarchy handle.
 *
 * Per-handle ordered: the release waits for any in-flight fetch on the same
 * handle to complete before reclaiming resources.
 */
ovstage_enqueue_result_t ovstage_release_hierarchy(
    ovstage_instance_t*          instance,
    ovstage_hierarchy_handle_t   handle);

/**
 * @brief Return the hierarchy computation models supported by this backend.
 *
 * The descriptor array lists the ovstage_hierarchy_computation_model_id_t
 * concrete values and selectors supported by this instance. Names and
 * descriptions are implementation-owned and valid for the lifetime of the
 * instance.
 *
 * @param instance The ovstage instance.
 * @param[out] out_models Receives the implementation-owned descriptor array.
 * @param[out] out_model_count Receives the number of descriptors.
 * @return OVSTAGE_OK on success.
 */
ovstage_api_status_t ovstage_get_hierarchy_computation_models(
    ovstage_instance_t*                                  instance,
    const ovstage_hierarchy_computation_model_desc_t**   out_models,
    size_t*                                              out_model_count);

/**
 * @brief Enqueue hierarchy computation using a runtime computation model.
 *
 * This computes hierarchy-derived stage data such as local/world transforms,
 * visibility, and other derived bounds/state managed by the selected runtime
 * model.
 *
 * The caller supplies the input ordinal to compute from and the output ordinal
 * assigned to hierarchy-derived results.
 *
 * @param instance The ovstage instance.
 * @param computation_model_id Public model enum value: a concrete model, a
 *        DEFAULT_* alias, or RUNTIME_DEFAULT to use the process default
 *        captured when the instance was created.
 * @param input_ordinal Ordinal of hierarchy inputs to compute from.
 * @param output_ordinal Ordinal assigned to hierarchy-derived outputs.
 * @return Enqueue result with status + op_index. Per-op execution failures are
 *         surfaced through ovstage_wait_op + ovstage_get_last_op_error.
 */
ovstage_enqueue_result_t ovstage_compute_hierarchy(
    ovstage_instance_t*                        instance,
    ovstage_hierarchy_computation_model_id_t   computation_model_id,
    ovstage_ordinal_t                          input_ordinal,
    ovstage_ordinal_t                          output_ordinal);

/* ═══════════════════════════════════════════════════════════════════════════════
 * Deferred Design Items
 *
 * The following items are acknowledged but deferred to a future revision:
 *
 * 1. GPU DEVICE CONFIGURATION
 *    - gpu_device_id should NOT be in ovstage_instance_desc_t.
 *    - Should be configurable per-attribute and/or per-write.
 *    - One ovstage instance must support multiple GPU devices.
 *    - Affects: ovstage_instance_desc_t, map/write APIs, tensor device selection.
 *
 * 2. MULTI-ATTRIBUTE ATOMIC WRITES
 *    - Commit positions + velocities atomically to prevent torn reads.
 *    - Would require multi-attribute map handle or transaction concept.
 *    - Current workaround: write all attrs at same time, advance write floor atomically.
 *
 * ═══════════════════════════════════════════════════════════════════════════════ */

#ifdef __cplusplus
}
#endif

#include "ovstage_instancing.h"
#include "ovstage_population.h"

#endif /* OVSTAGE_H */
