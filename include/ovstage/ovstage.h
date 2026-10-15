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
 * @brief OVStage instance lifecycle — creation and destruction.
 *
 * @details
 * The OVStage data-plane API (types, handles, operations, diagnostics) is
 * declared in `ovstage_api/ovstage_api.h` as a vtable contract and exposed
 * to callers through inline wrappers in `ovstage_api/ovstage_api_utils.h`
 * (transitively included from `ovstage_api.h`) and sibling extension APIs.
 *
 * This header adds the pieces specific to ovstage: an instance descriptor,
 * create/destroy entry points, and instance helpers. Once an
 * `ovstage_instance_t*` has been produced by `ovstage_create_instance`,
 * call the generic `ovstage_*` wrappers declared in `ovstage_api.h` and the
 * ovstage-specific entry points declared here to drive it.
 *
 * @version 0.3.0
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
 * Build Information
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Return the build information embedded in the OVStage API implementation.
 *
 * This call is safe before ovstage_initialize() and does not initialize the
 * OVStage runtime. The shared and static loaders report the loader build. A
 * directly linked runtime library reports the runtime build. Loader/runtime
 * pairs from one package are built from the same generated information, but a
 * separately selected runtime package can report different values through its
 * callback log and crash metadata.
 *
 * @return A non-NULL pointer owned by the code providing this API. The pointer
 *         and all borrowed string fields remain valid for that code's lifetime.
 */
const ovstage_build_info_t* ovstage_get_build_info(void);

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
 *               OVSTAGE_CONFIG_RUNTIME_DEFAULT_ORDINAL_ADMISSION_POLICY
 *               selects mutation admission, defaulting to NONDECREASING.
 *               Omitted settings and their DEFAULT selectors inherit active
 *               process settings, or select defaults on the first reference.
 *               Later concrete settings must match the active settings. All
 *               settings are validated together: a rejected call changes none
 *               of them and acquires no reference. A policy can change only
 *               after all initialization and instance references are released,
 *               including retained instance handles and attachments.
 * @return OVSTAGE_OK on success; OVSTAGE_ERROR_INVALID_ARGUMENT for malformed,
 *         duplicate, unknown, or conflicting runtime configuration.
 * @post Balance every successful call with one ovstage_shutdown().
 */
ovstage_api_status_t ovstage_initialize(const ovstage_config_t* config);

/**
 * @brief Read back the process configuration captured by an ovstage instance.
 *
 * The returned config contains the explicit entries accepted when the active
 * process state was first initialized, in their original order. It does not add
 * omitted defaults or expose internal settings derived from those entries.
 *
 * The entry array and any string or blob payloads are owned by @p instance and
 * remain valid until ovstage_destroy_instance() is called. The caller must not
 * free or modify them. The caller owns only the outer ovstage_config_t value
 * written to @p out_config.
 *
 * @param instance Initialized ovstage instance.
 * @param[out] out_config Borrowed view of the instance's process config.
 * @return OVSTAGE_OK on success; OVSTAGE_ERROR_INVALID_ARGUMENT if either
 *         argument is NULL.
 */
ovstage_api_status_t ovstage_get_config(const ovstage_instance_t* instance, ovstage_config_t* out_config);

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
 * When ovstage runs standalone (the shipped package used directly), this
 * callback is the only diagnostics sink: with no callback installed the runtime
 * prints nothing to stdout/stderr — except a final sanitized message when a
 * fatal error aborts the process — and operation failures are reported through
 * status codes and the error string accessors regardless of logging. When
 * ovstage is embedded in a host application that configures its own logging,
 * console behavior follows the host's configuration. USD-support-layer messages
 * carry the diagnostic text only (statuses at OVSTAGE_LOG_INFO, warnings and
 * USD coding errors at OVSTAGE_LOG_WARNING, other errors at OVSTAGE_LOG_ERROR),
 * never call-site file/line details.
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
 * @brief Release an ovstage instance handle.
 * @param instance Instance handle to release. May be NULL (no-op).
 * @pre If this is the final handle retaining the stage, all operations, handles,
 *      and result payloads associated with the stage must be released first.
 * @pre No other thread may invoke any `ovstage_*` wrapper, vtable slot, or
 *      internal accessor on this `instance` while or after destroy runs. The
 *      supplied bundle is deallocated before this call returns; any concurrent
 *      or post-destroy use of that handle is a use-after-free.
 * @details Another component may retain a distinct handle to the same stage.
 *          In that case the shared stage state and resources remain alive until
 *          the final retained handle is released. In-flight work remains valid
 *          and must be completed and released through a surviving handle.
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
 * created, so a batch mixing fresh and existing targets clones nothing. A prim that
 * already exists below an absent target root is merged into, unless it holds a cloned
 * attribute with a different type, carries a prim type other than that of its typed
 * source prim, or lacks a cloned attribute connection that has no value column, which
 * fails the clone with `OVSTAGE_ERROR_OP_FAILED` before any prim is created. The per-op
 * error string (`ovstage_get_last_op_error`) names the missing source or the targets
 * that already exist, e.g. `ovstage_clone: target prim already exists at
 * '/World/Tile3'`, so a large batch can be corrected in one round-trip.
 *
 * Data-plane peer of ovrtx's `ovrtx_clone_usd` (the `_usd` postfix is dropped),
 * but — like `write_attribute` / `delete_attributes` — clone is an ordinal-keyed
 * write: it carries an @p ordinal and is rejected with
 * `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION` when
 * @p ordinal is at or below an applicable seal or conflicts with retained target
 * cell/lifecycle history under the configured ordinal admission policy. Each
 * copied attribute records a change at @p ordinal. The STRICTLY_INCREASING
 * policy requires a newer ordinal for later customization even before sealing;
 * NONDECREASING allows customization at the same unsealed ordinal but rejects
 * older intersecting history. FLOOR_ONLY skips retained-history and separate prim-lifecycle
 * admission. Every policy preserves the applicable global, source and destination
 * attribute floors. Disjoint target cells may share an ordinal.
 * The call returns immediately with an
 * `ovstage_enqueue_result_t` (status + `op_index`); await completion through the vtable
 * `wait_op` (and release it with `release_op`) like any other data-plane operation.
 * An asynchronous failure makes `wait_op` return `OVSTAGE_ERROR_OP_FAILED`;
 * `fetch_clone_result` returns the underlying error, and `get_last_op_error`
 * describes it. A synchronous rejection is returned in the enqueue status.
 * The clone's completion report includes the targets' scene hierarchy subtree
 * sizes after cloning, summed over all targets (see `ovstage_clone_result_t`).
 * It is available through ovstage_fetch_clone_result from completion until `release_op`.
 *
 * Path-bearing values that target the source subtree are rebased to the corresponding
 * target subtree. This includes relationship targets (e.g. `material:binding`,
 * `skel:skeleton`), scalar/array path values, and USD attribute connections.
 * Paths outside the source subtree are copied unchanged, so clones can continue to
 * reference shared materials and other shared resources. Every cloned column is
 * ordinal-change-tracked: attribute values, relationship targets, an attribute
 * connection that has no value column, prim type / applied schema / ancestor type
 * metadata, and each typed identity of an attribute name that has different types on
 * different source prims. Scene hierarchy changes, such as the source/target parents'
 * child lists, are not.
 *
 * @param instance         The ovstage instance.
 * @param source_path      Path to the source subtree to clone.
 * @param target_paths     Array of target paths to clone to.
 * @param num_target_paths Number of target paths to clone to.
 * @param ordinal          Ordinal for this clone. Must exceed the global write floor
 *                         and the effective seals of the source subtree's attributes.
 *                         For nonempty targets, it must also exceed the effective
 *                         seals of all reproduced destination attributes, including
 *                         copied prim type, applied schema and ancestor type metadata.
 *                         Independently, the configured history policy checks retained
 *                         destination-cell changes and every derived target prim's
 *                         lifecycle, including the effective `usd-path` floor.
 *                         FLOOR_ONLY skips retained-history and separate lifecycle
 *                         admission. All checks run at execution time. With no targets,
 *                         only the global and source-attribute floor checks apply.
 * @return Enqueue result with status + op_index.
 * - **status == OVSTAGE_OK** if the operation was enqueued successfully.
 * - `OVSTAGE_ERROR_INVALID_ARGUMENT` if a target path is empty or is not a valid path,
 *   or appears more than once in @p target_paths (rejected at enqueue; the offending
 *   path is reported through `ovstage_get_last_error`).
 * - a non-OK status if the enqueue was rejected (e.g. invalid arguments, or the
 *   required runtime capability is unavailable). Per-op execution errors — including
 *   `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`, `OVSTAGE_ERROR_NOT_FOUND` (missing
 *   source), `OVSTAGE_ERROR_PRIM_NOT_FOUND` (a target already exists), and
 *   `OVSTAGE_ERROR_OP_FAILED` (a merge conflict) — are
 *   surfaced through `wait_op` (as `OVSTAGE_ERROR_OP_FAILED` plus the message) and
 *   with their own code through ovstage_fetch_clone_result.
 *
 * @remark Target paths are expected to be disjoint and outside the source subtree.
 *         A target nested under another target of the same batch is not rejected
 *         today; its prims are counted as enumerated in the completion report. A
 *         target that is also a prim the clone creates under another target (e.g.
 *         `/World/T` and `/World/T/Inner` for a source with a child `Inner`) fails
 *         with `OVSTAGE_ERROR_OP_FAILED` and clones nothing. A target path with a
 *         property part (e.g. `/World/T.attr`) cannot hold the source's child prims;
 *         if the source has any, the clone fails with `OVSTAGE_ERROR_OP_FAILED` and
 *         clones nothing.
 *
 * @remark Merging into a prim that already exists below an absent target root is
 *         partial. The existing prim keeps the attributes it has that the source prim
 *         lacks, its applied schemas and ancestor types become the union of its own and
 *         the source prim's, and if the source prim has no prim type it keeps its own.
 *         For an attribute name that the source prim holds both as a value and as a
 *         connection, the value is merged, but the connection is copied (and rebased)
 *         only when the existing prim already has a connection under that name;
 *         otherwise it is not cloned and the clone still succeeds. A newly created
 *         target receives both. Either way such a name is change-tracked under its
 *         value type only, and an existing prim that holds only the connection under
 *         that name fails the clone with `OVSTAGE_ERROR_OP_FAILED`. Delete such a prim
 *         before cloning onto it to get an exact copy.
 *
 * @remark Before its merge and ordinal checks on the target prims, a clone interns
 *         the path of every prim it would create or merge into (each source subtree
 *         prim under each target root). Interned paths are not released individually,
 *         so a clone that fails after that point keeps them: each such clone that uses
 *         new target names keeps memory proportional to source prims times target roots.
 *         Reusing the same names avoids new path entries, but repeated registration
 *         can extend their lifetime. Interned paths can stay allocated after the
 *         instance is destroyed.
 */
ovstage_enqueue_result_t ovstage_clone(
    ovstage_instance_t* instance,
    ovx_string_t        source_path,
    const ovx_string_t* target_paths,
    size_t              num_target_paths,
    ovstage_ordinal_t   ordinal);

/**
 * @brief Fetch the completion report of a clone operation.
 *
 * Waits up to @p timeout (`0` polls, `OVSTAGE_TIMEOUT_INFINITE` blocks — the
 * `ovstage_fetch_*` convention) for the clone identified by @p op_id (the
 * `op_index` returned by ovstage_clone) to complete, then copies its report.
 * The report is per-op completion state like the op's error string: available
 * from completion until `release_op`. Fetching does not release the op; call
 * `release_op` afterwards as for any other operation.
 *
 * @param instance   The ovstage instance.
 * @param op_id      The clone's op id.
 * @param timeout    Maximum time to wait for completion, in nanoseconds.
 * @param[out] out_result Receives the report on `OVSTAGE_OK`; zeroed otherwise.
 * @return OVSTAGE_OK and the report on success.
 * @return OVSTAGE_ERROR_INVALID_ARGUMENT if @p instance or @p out_result is NULL.
 * @return OVSTAGE_ERROR_INVALID_HANDLE if @p op_id is not a live clone operation
 *         of this instance: `OVSTAGE_INVALID_OP_ID`, an id of another kind of
 *         operation, or a clone whose op has already been released.
 * @return OVSTAGE_ERROR_TIMEOUT if the clone has not completed within @p timeout;
 *         the op stays waitable.
 * @return the clone's own failure status — `OVSTAGE_ERROR_NOT_FOUND` (missing
 *         source), `OVSTAGE_ERROR_PRIM_NOT_FOUND` (a target already existed),
 *         `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`, `OVSTAGE_ERROR_OUT_OF_MEMORY` or
 *         `OVSTAGE_ERROR_INTERNAL` (an internal failure, which can also be reported
 *         after the clone was applied, so the targets may exist),
 *         otherwise `OVSTAGE_ERROR_OP_FAILED` — if the clone ran and failed, with the
 *         message available through `ovstage_get_last_op_error(op_id)`.
 *
 * @remark Like `wait_op`, an infinite-timeout fetch may execute the still-pending
 *         clone inline on the calling thread.
 */
ovstage_api_status_t ovstage_fetch_clone_result(
    ovstage_instance_t*     instance,
    ovstage_op_id_t         op_id,
    ovstage_timeout_ns_t    timeout,
    ovstage_clone_result_t* out_result);

/**
 * @brief Enqueue a hierarchy lookup for a batch of prims.
 *
 * `prim_paths` is an immutable ordered `ovx_primpath_list_t`. The list is
 * copied during enqueue; callers may destroy their user-owned list after this
 * call returns OVSTAGE_OK. The operation returns parent, child or sibling
 * relations for each input prim. Fetch the per-input results with
 * ovstage_fetch_hierarchy_result.
 *
 * Hierarchy lookups read the current committed hierarchy when the
 * operation executes. They do not select a historical snapshot.
 *
 * @param instance The ovstage instance.
 * @param prim_paths Ordered prim path list to inspect.
 * @param ordinal Echoed as a caller-supplied tag; it does not select a
 *                historical hierarchy snapshot.
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
 * @brief Return the hierarchy computation models supported by this runtime.
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
 * This computes hierarchy-derived world transforms with the selected runtime
 * model. It does not update derived visibility.
 *
 * The caller supplies the output ordinal assigned to hierarchy-derived
 * results. The computation records an `omni:fabric:worldMatrix` change at
 * `output_ordinal` for every world-matrix row it rewrote. The write floor of
 * that column does not limit `output_ordinal`: a record can land at a sealed
 * ordinal. A consumer that read that ordinal before the computation does not
 * see those rows. Compute before you seal the ordinal when consumers need a
 * complete snapshot at it. A write-floor advance that covers the column runs
 * the runtime default model when the hierarchy is dirty, and records the
 * rewritten rows at the advance ordinal, before the floor moves. Only rows a
 * computation rewrote are recorded this way. A direct write to the column is
 * recorded by the write itself. Ordinal-range reads of the column select the
 * recorded prims.
 *
 * @remark The current implementation ignores `input_ordinal` and computes
 * from the latest committed inputs. Inputs committed after `input_ordinal`
 * are part of the computation.
 *
 * @param instance The ovstage instance.
 * @param computation_model_id Public model enum value: a concrete model, a
 *        DEFAULT_* alias, or RUNTIME_DEFAULT to use the process default
 *        captured when the instance was created.
 * @param input_ordinal Ordinal of the hierarchy inputs. See the remark above
 *        on how the current implementation handles it.
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
