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
 * the process-level functions, the functions of the lifecycle, hierarchy and
 * usd families, and the subtree clone wrappers. Once an `ovstage_instance_t*`
 * has been produced by `ovstage_create_instance`, call the generic `ovstage_*`
 * wrappers declared in `ovstage_api.h` and the functions declared here to
 * drive it.
 *
 * The lifecycle functions `ovstage_create_instance`, `ovstage_get_config` and
 * `ovstage_destroy_instance` are exported C functions of the library. The
 * exported bodies of `ovstage_get_config` and `ovstage_destroy_instance` call
 * their members through the lifecycle instance. `ovstage_ext_vtables.h`
 * defines the family vtables and states the rules of the extension instances.
 *
 * The functions of the hierarchy and usd families are `static inline`
 * wrappers, like the data-plane wrappers of `ovstage_api_utils.h`. Each
 * wrapper has two forms. The pair form, `ovstage_<name>_ex`, takes the
 * extension instance of its family, `ovstage_<family>_instance_t*`. It
 * rejects a NULL pair with the documented answer. Then it calls its member
 * with the context of that pair first,
 * `<family>->vtable-><member>(<family>->context, ...)`. The instance form
 * takes `ovstage_instance_t*` and passes `&instance->ext_instances.<family>`
 * to the pair form, or NULL for a NULL instance. C++ overloads the name of
 * the instance form over the pair type, so both receivers share one name.
 *
 * `ovstage_clone` and `ovstage_fetch_clone_result` are `static inline`
 * wrappers over the slots `clone` and `fetch_clone_result` of the public
 * vtable: a clone is an operation of the data plane. Their pair form takes
 * `ovstage_data_plane_instance_t*`, and the instance form passes
 * `&instance->data_plane`.
 *
 * @version 0.4.0
 * @date 2026-10-29
 */

#ifndef OVSTAGE_H
#define OVSTAGE_H

#include "ovstage_api/ovstage_api.h"
#include "ovstage_config.h"
#include "ovstage_ext_vtables.h"
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
 * @brief Initialize process-wide ovstage configuration and resources.
 *
 * Pair each successful call with one ovstage_shutdown(). Explicit initialization lets applications supply configuration before creating stages and keep process-level resources available between stage lifetimes.
 *
 * Explicit initialization is optional when linking the shared library directly and using default configuration. Static-loader applications must supply the required package location before other ovstage calls. Initialization also reports startup failures before the first stage is created.
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
 *               OVSTAGE_CONFIG_POPULATION_THREAD_LIMIT bounds the threads a
 *               population operation may use to read what its selectors
 *               select, defaulting to 1 (serial).
 *               OVSTAGE_CONFIG_RETENTION_WINDOW sets the number of sealed
 *               ordinals a history-capable instance retains, defaulting to 4.
 *               The implementation of an instance is a per-instance choice.
 *               The `implementation` member of ovstage_instance_desc_t
 *               selects it at ovstage_create_instance: the default keeps
 *               history off, and a history-capable implementation turns
 *               history on for the selected pairs.
 *               Omitted settings and their DEFAULT selectors inherit active
 *               process settings, or select defaults on the first reference.
 *               Later concrete settings must match the active settings. All
 *               settings are validated together: a rejected call changes none
 *               of them and acquires no reference. A policy can change only
 *               after all initialization and instance references are released,
 *               including retained instance handles and attachments.
 * @note Experimental. The setting OVSTAGE_CONFIG_RETENTION_WINDOW and the
 *       history-capable implementation "history_selective" are experimental.
 *       Their API, ABI and semantics can change between releases without a
 *       deprecation period.
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
 *         argument is NULL. The library serves this function for every
 *         implementation it creates an instance of.
 */
ovstage_api_status_t ovstage_get_config(const ovstage_instance_t* instance, ovstage_config_t* out_config);

/**
 * @brief Finish one successful explicit ovstage initialization.
 *
 * Call once for each successful ovstage_initialize(). This does not destroy application stages; destroy them separately after completing their work. Process-level configuration remains active while stages or unmatched successful initialization calls remain.
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
 * Messages are delivered asynchronously, one callback invocation at a time. Do not assume delivery on the calling thread. Use ovstage_flush_log to wait for pending messages before a checkpoint.
 *
 * Passing @p callback = NULL delivers pending messages to the current callback and then disables delivery.
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
 * Requires a successful ovstage_initialize() not yet matched by ovstage_shutdown(), or a live instance; otherwise returns OVSTAGE_ERROR_OP_FAILED. When no live stages or unmatched successful initialization calls remain, logging is disabled after pending messages are delivered. Register the callback again after reinitialization. To preserve logging between stage lifetimes, initialize explicitly and call the matching shutdown only when finished.
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
 * @brief Create an ovstage instance of the implementation the descriptor selects.
 *
 * The `implementation` member of @p desc points to the name of the
 * implementation. NULL or an empty string selects the default
 * implementation. A name that no implementation registered fails the call
 * with OVSTAGE_ERROR_INVALID_ARGUMENT. The call then acquires no process
 * reference and writes no instance, and ovstage_get_last_error reports the
 * name. The selection is per instance: instances of different
 * implementations share one process. The call acquires a process reference
 * that the instance holds until ovstage_destroy_instance releases it.
 *
 * @remark The implementations of this release are "history_latest", the
 *         default, and "history_selective". "history_latest" keeps history
 *         off: the instance keeps the latest value of every cell, a read
 *         returns it, and the history selection calls report
 *         OVSTAGE_ERROR_NOT_SUPPORTED. "history_selective" turns history on
 *         for the pairs the application selects: ovstage_enable_history()
 *         (ovstage_history.h) selects the (prim, attribute) pairs that
 *         retain their earlier sealed values, up to
 *         OVSTAGE_CONFIG_RETENTION_WINDOW. Every other cell keeps the latest
 *         value.
 * @note Experimental. The implementation "history_selective" and the history
 *       selection API of ovstage_history.h are experimental. Their API, ABI
 *       and semantics can change between releases without a deprecation
 *       period.
 *
 * @param desc Configuration descriptor.
 * @param[out] out_instance Receives the new instance. The library fills the lifecycle pair and the stub pairs, and the factory of the selected implementation fills the pairs it serves.
 * @return OVSTAGE_OK on success; OVSTAGE_ERROR_INVALID_ARGUMENT when no
 *         implementation registered the name in @p desc.
 * @post Caller owns the instance; destroy via ovstage_destroy_instance.
 * @remark The instance's backing USD stage captures its resolver context here:
 *  the `ArResolverContext` bound on the calling thread (`ArResolverContextBinder`),
 *  merged with the resolver's default context with the bound objects taking
 *  precedence per context type, or the default context alone when nothing is
 *  bound. USD population binds that captured context around its layer opens for
 *  the instance's lifetime, so a context-dependent resolver's context must be in
 *  place before this call; one bound only afterwards is not seen. This call may
 *  also be the process's first `ArGetResolver()`, which fixes OpenUSD's primary
 *  resolver: a host that selects one (`ArSetPreferredResolver`) does so before
 *  creating its first instance. Destroy the instance before releasing the
 *  resolver runtime its context refers to.
 */
ovstage_api_status_t ovstage_create_instance(
    const ovstage_instance_desc_t* desc,
    ovstage_instance_t**           out_instance);

/**
 * @brief Destroy the caller's ovstage instance handle.
 * @param instance Instance handle to destroy. May be NULL (no-op).
 * @pre Complete required work and release application-owned operations, query/read/map handles, and result payloads before final stage cleanup.
 * @pre No other thread may use this instance while or after destruction runs.
 * @details The supplied handle is invalid after a call that returns OVSTAGE_OK. Destroying it does not detach an already attached renderer; use that renderer's public detach or destruction API to end the attachment. Do not use the destroyed handle to access data, submit operations, or wait for work.
 * @return OVSTAGE_OK when the instance is destroyed, and for a NULL instance.
 *         An implementation may refuse the destroy while it still needs the
 *         instance, with a status other than OVSTAGE_OK that it documents.
 *         The library then frees nothing and the instance stays valid, with
 *         its process reference. Call this function again after the condition
 *         the implementation documents is cleared. The library serves this
 *         function for every implementation it creates an instance of.
 */
ovstage_api_status_t ovstage_destroy_instance(ovstage_instance_t* instance);

/** @brief The pair form of ovstage_get_usd_stage_id: the same call on the usd
 *         instance, `&instance->ext_instances.usd`. C++ overloads
 *         ovstage_get_usd_stage_id for this receiver. */
static inline ovstage_api_status_t ovstage_get_usd_stage_id_ex(
    ovstage_usd_instance_t* usd,
    uint64_t*               out_usd_stage_id)
{
    if (usd == NULL)
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    return usd->vtable->get_usd_stage_id(usd->context, out_usd_stage_id);
}

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
static inline ovstage_api_status_t ovstage_get_usd_stage_id(
    ovstage_instance_t* instance,
    uint64_t*           out_usd_stage_id)
{
    return ovstage_get_usd_stage_id_ex(
        instance != NULL ? &instance->ext_instances.usd : NULL, out_usd_stage_id);
}

/** @brief The pair form of ovstage_clone: the same call on the data-plane
 *         instance, `&instance->data_plane`. C++ overloads ovstage_clone for
 *         this receiver. */
static inline ovstage_enqueue_result_t ovstage_clone_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovx_string_t                   source_path,
    const ovx_string_t*            target_paths,
    size_t                         num_target_paths,
    ovstage_ordinal_t              ordinal)
{
    if (data_plane == NULL)
    {
        ovstage_enqueue_result_t rejected;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_INVALID_OP_ID;
        return rejected;
    }
    return data_plane->vtable->clone(
        data_plane->context, source_path, target_paths, num_target_paths, ordinal);
}

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
 * Cloned prims are entered in the scene hierarchy when the clone is published, like
 * prims created by writes and maps: a hierarchy lookup (`ovstage_get_hierarchy`)
 * reports a cloned prim under its parent in the copy, and a clone root under its
 * destination parent whenever that parent is a prim, whether it was created through
 * this API (by a write, a map, or a clone) or by USD population. A clone root whose
 * destination parent does not exist attaches to the nearest existing ancestor, is
 * reported as having no parent, and its derived transforms compose under that
 * ancestor. When a later write, map, clone or USD population creates the parent, the
 * clone root is reconnected under it and its derived transforms are recomputed on the
 * next computation trigger (a reconnection that USD population would make while a
 * fetched read group covers a prim it would give `omni:fabric:worldMatrix` is deferred
 * until the next population step, `ovstage_compute_hierarchy` call, or write, map or
 * clone of that parent after the group is released; a write, map or clone that would
 * make such a promotion fails instead); a written or mapped parent of cloned prims that carry
 * `omni:fabric:worldMatrix` receives that output itself once it has both the clones
 * under it and a local transform, in whichever order they arrive, so the recomputation
 * has a parent transform to compose with (see the transforms documentation).
 * The ancestor a clone root attaches to while its destination parent is absent
 * receives that output the same way when the clone is published.
 * Giving a prim that output relocates its storage, so a clone that would give it to a
 * prim an outstanding read covers — the destination parent, one of its ancestors, or
 * an existing descendant the clone merges into — fails with `OVSTAGE_ERROR_OP_FAILED`
 * before any prim is created; release the read group and clone again.
 * Conversely, a clone whose target root path still has children
 * entered in the hierarchy from a deleted prim re-attaches those children under the
 * new root, as a write or map recreating that root does; they count in the report's
 * `prims_cloned`. Creating destination parents before cloning is recommended: the
 * copies then land in their final place at once, with nothing to reconnect later.
 *
 * @param instance         The ovstage instance.
 * @param source_path      Absolute path to the source subtree to clone, in canonical spelling.
 * @param target_paths     Array of absolute prim, variant-selection, or property paths to clone to,
 *                         in canonical spelling.
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
 * - In the default `history_latest` implementation, `OVSTAGE_ERROR_INVALID_ARGUMENT`
 *   if a source or target path is relative, malformed, or noncanonical; a target
 *   has an unsupported path kind; or a target appears more than once in
 *   @p target_paths (rejected at enqueue; details are reported through
 *   `ovstage_get_last_op_error(instance, OVSTAGE_INVALID_OP_ID)`).
 * - a non-OK status if the enqueue was rejected (e.g. invalid arguments, or the
 *   required runtime capability is unavailable). Per-op execution errors — including
 *   `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`, `OVSTAGE_ERROR_NOT_FOUND` (missing
 *   source), `OVSTAGE_ERROR_PRIM_NOT_FOUND` (a target already exists), and
 *   `OVSTAGE_ERROR_OP_FAILED` (a merge conflict, or a prim that would receive
 *   `omni:fabric:worldMatrix` while an outstanding read covers it) — are
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
static inline ovstage_enqueue_result_t ovstage_clone(
    ovstage_instance_t* instance,
    ovx_string_t        source_path,
    const ovx_string_t* target_paths,
    size_t              num_target_paths,
    ovstage_ordinal_t   ordinal)
{
    return ovstage_clone_ex(
        instance != NULL ? &instance->data_plane : NULL,
        source_path, target_paths, num_target_paths, ordinal);
}

/** @brief The pair form of ovstage_fetch_clone_result: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_fetch_clone_result for this receiver. */
static inline ovstage_api_status_t ovstage_fetch_clone_result_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_op_id_t                op_id,
    ovstage_timeout_ns_t           timeout,
    ovstage_clone_result_t*        out_result)
{
    if (data_plane == NULL)
    {
        if (out_result != NULL)
        {
            ovstage_clone_result_t zero = { 0 };
            *out_result = zero;
        }
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    }
    return data_plane->vtable->fetch_clone_result(data_plane->context, op_id, timeout, out_result);
}

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
 * @param[out] out_result Receives the report on `OVSTAGE_OK`. An implementation
 *             that does not serve this function returns
 *             `OVSTAGE_ERROR_NOT_SUPPORTED` and leaves it unchanged. Zeroed on
 *             any other failure.
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
 *         otherwise `OVSTAGE_ERROR_OP_FAILED` (a merge conflict, or a prim that would
 *         receive `omni:fabric:worldMatrix` while an outstanding read covers it) — if
 *         the clone ran and failed, with the message available through
 *         `ovstage_get_last_op_error(op_id)`.
 *
 * @remark Like `wait_op`, an infinite-timeout fetch may execute the still-pending
 *         clone inline on the calling thread.
 */
static inline ovstage_api_status_t ovstage_fetch_clone_result(
    ovstage_instance_t*     instance,
    ovstage_op_id_t         op_id,
    ovstage_timeout_ns_t    timeout,
    ovstage_clone_result_t* out_result)
{
    return ovstage_fetch_clone_result_ex(
        instance != NULL ? &instance->data_plane : NULL, op_id, timeout, out_result);
}

/** @brief The pair form of ovstage_get_hierarchy: the same call on the hierarchy
 *         instance, `&instance->ext_instances.hierarchy`. C++ overloads
 *         ovstage_get_hierarchy for this receiver. */
static inline ovstage_enqueue_result_t ovstage_get_hierarchy_ex(
    ovstage_hierarchy_instance_t*   hierarchy,
    ovx_primpath_list_t             prim_paths,
    ovstage_ordinal_t               ordinal,
    ovstage_hierarchy_relation_t    relation,
    ovstage_hierarchy_handle_t*     out_hierarchy_handle)
{
    if (hierarchy == NULL)
    {
        ovstage_enqueue_result_t rejected;
        if (out_hierarchy_handle != NULL)
            *out_hierarchy_handle = OVSTAGE_INVALID_HIERARCHY_HANDLE;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_INVALID_OP_ID;
        return rejected;
    }
    return hierarchy->vtable->get_hierarchy(
        hierarchy->context, prim_paths, ordinal, relation, out_hierarchy_handle);
}

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
static inline ovstage_enqueue_result_t ovstage_get_hierarchy(
    ovstage_instance_t*             instance,
    ovx_primpath_list_t             prim_paths,
    ovstage_ordinal_t               ordinal,
    ovstage_hierarchy_relation_t    relation,
    ovstage_hierarchy_handle_t*     out_hierarchy_handle)
{
    return ovstage_get_hierarchy_ex(
        instance != NULL ? &instance->ext_instances.hierarchy : NULL,
        prim_paths, ordinal, relation, out_hierarchy_handle);
}

/** @brief The pair form of ovstage_fetch_hierarchy_result: the same call on the
 *         hierarchy instance, `&instance->ext_instances.hierarchy`. C++
 *         overloads ovstage_fetch_hierarchy_result for this receiver. */
static inline ovstage_api_status_t ovstage_fetch_hierarchy_result_ex(
    ovstage_hierarchy_instance_t* hierarchy,
    ovstage_hierarchy_handle_t    hierarchy_handle,
    ovstage_hierarchy_result_t*   out_result)
{
    if (hierarchy == NULL)
    {
        if (out_result != NULL)
        {
#ifdef __cplusplus
            ovstage_hierarchy_result_t zero{};
#else
            ovstage_hierarchy_result_t zero = { 0 };
#endif
            *out_result = zero;
        }
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    }
    return hierarchy->vtable->fetch_hierarchy_result(
        hierarchy->context, hierarchy_handle, out_result);
}

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
static inline ovstage_api_status_t ovstage_fetch_hierarchy_result(
    ovstage_instance_t*          instance,
    ovstage_hierarchy_handle_t   hierarchy_handle,
    ovstage_hierarchy_result_t*  out_result)
{
    return ovstage_fetch_hierarchy_result_ex(
        instance != NULL ? &instance->ext_instances.hierarchy : NULL,
        hierarchy_handle, out_result);
}

/** @brief The pair form of ovstage_release_hierarchy_result: the same call on
 *         the hierarchy instance, `&instance->ext_instances.hierarchy`. C++
 *         overloads ovstage_release_hierarchy_result for this receiver. */
static inline ovstage_api_status_t ovstage_release_hierarchy_result_ex(
    ovstage_hierarchy_instance_t*       hierarchy,
    const ovstage_hierarchy_result_t*   result)
{
    if (hierarchy == NULL)
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    return hierarchy->vtable->release_hierarchy_result(hierarchy->context, result);
}

/**
 * @brief Release a fetched hierarchy result payload.
 *
 * After this call, pointers in `*result` are invalid.
 */
static inline ovstage_api_status_t ovstage_release_hierarchy_result(
    ovstage_instance_t*                 instance,
    const ovstage_hierarchy_result_t*   result)
{
    return ovstage_release_hierarchy_result_ex(
        instance != NULL ? &instance->ext_instances.hierarchy : NULL, result);
}

/** @brief The pair form of ovstage_release_hierarchy: the same call on the
 *         hierarchy instance, `&instance->ext_instances.hierarchy`. C++
 *         overloads ovstage_release_hierarchy for this receiver. */
static inline ovstage_enqueue_result_t ovstage_release_hierarchy_ex(
    ovstage_hierarchy_instance_t* hierarchy,
    ovstage_hierarchy_handle_t    handle)
{
    if (hierarchy == NULL)
    {
        ovstage_enqueue_result_t rejected;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_INVALID_OP_ID;
        return rejected;
    }
    return hierarchy->vtable->release_hierarchy(hierarchy->context, handle);
}

/**
 * @brief Enqueue release of a hierarchy handle.
 *
 * Per-handle ordered: the release waits for any in-flight fetch on the same
 * handle to complete before reclaiming resources.
 */
static inline ovstage_enqueue_result_t ovstage_release_hierarchy(
    ovstage_instance_t*          instance,
    ovstage_hierarchy_handle_t   handle)
{
    return ovstage_release_hierarchy_ex(
        instance != NULL ? &instance->ext_instances.hierarchy : NULL, handle);
}

/** @brief The pair form of ovstage_get_hierarchy_computation_models: the same
 *         call on the hierarchy instance, `&instance->ext_instances.hierarchy`.
 *         C++ overloads ovstage_get_hierarchy_computation_models for this
 *         receiver. */
static inline ovstage_api_status_t ovstage_get_hierarchy_computation_models_ex(
    ovstage_hierarchy_instance_t*                        hierarchy,
    const ovstage_hierarchy_computation_model_desc_t**   out_models,
    size_t*                                              out_model_count)
{
    if (hierarchy == NULL)
    {
        if (out_models != NULL)
            *out_models = NULL;
        if (out_model_count != NULL)
            *out_model_count = 0;
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    }
    return hierarchy->vtable->get_hierarchy_computation_models(
        hierarchy->context, out_models, out_model_count);
}

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
static inline ovstage_api_status_t ovstage_get_hierarchy_computation_models(
    ovstage_instance_t*                                  instance,
    const ovstage_hierarchy_computation_model_desc_t**   out_models,
    size_t*                                              out_model_count)
{
    return ovstage_get_hierarchy_computation_models_ex(
        instance != NULL ? &instance->ext_instances.hierarchy : NULL,
        out_models, out_model_count);
}

/** @brief The pair form of ovstage_compute_hierarchy: the same call on the
 *         hierarchy instance, `&instance->ext_instances.hierarchy`. C++
 *         overloads ovstage_compute_hierarchy for this receiver. */
static inline ovstage_enqueue_result_t ovstage_compute_hierarchy_ex(
    ovstage_hierarchy_instance_t*              hierarchy,
    ovstage_hierarchy_computation_model_id_t   computation_model_id,
    ovstage_ordinal_t                          input_ordinal,
    ovstage_ordinal_t                          output_ordinal)
{
    if (hierarchy == NULL)
    {
        ovstage_enqueue_result_t rejected;
        rejected.status = OVSTAGE_ERROR_INVALID_ARGUMENT;
        rejected.op_index = OVSTAGE_INVALID_OP_ID;
        return rejected;
    }
    return hierarchy->vtable->compute_hierarchy(
        hierarchy->context, computation_model_id, input_ordinal, output_ordinal);
}

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
static inline ovstage_enqueue_result_t ovstage_compute_hierarchy(
    ovstage_instance_t*                        instance,
    ovstage_hierarchy_computation_model_id_t   computation_model_id,
    ovstage_ordinal_t                          input_ordinal,
    ovstage_ordinal_t                          output_ordinal)
{
    return ovstage_compute_hierarchy_ex(
        instance != NULL ? &instance->ext_instances.hierarchy : NULL,
        computation_model_id, input_ordinal, output_ordinal);
}

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
} /* extern "C" */

/* C++ spells the pair form with the name of the instance form. A literal NULL or
 * nullptr first argument is ambiguous between the two receivers: pass a
 * typed null, or call the _ex form. */
inline ovstage_api_status_t ovstage_get_usd_stage_id(
    ovstage_usd_instance_t* usd, uint64_t* out_usd_stage_id)
{
    return ovstage_get_usd_stage_id_ex(usd, out_usd_stage_id);
}

inline ovstage_enqueue_result_t ovstage_clone(
    ovstage_data_plane_instance_t* data_plane, ovx_string_t source_path,
    const ovx_string_t* target_paths, size_t num_target_paths, ovstage_ordinal_t ordinal)
{
    return ovstage_clone_ex(data_plane, source_path, target_paths, num_target_paths, ordinal);
}

inline ovstage_api_status_t ovstage_fetch_clone_result(
    ovstage_data_plane_instance_t* data_plane, ovstage_op_id_t op_id,
    ovstage_timeout_ns_t timeout, ovstage_clone_result_t* out_result)
{
    return ovstage_fetch_clone_result_ex(data_plane, op_id, timeout, out_result);
}

inline ovstage_enqueue_result_t ovstage_get_hierarchy(
    ovstage_hierarchy_instance_t* hierarchy, ovx_primpath_list_t prim_paths,
    ovstage_ordinal_t ordinal, ovstage_hierarchy_relation_t relation,
    ovstage_hierarchy_handle_t* out_hierarchy_handle)
{
    return ovstage_get_hierarchy_ex(hierarchy, prim_paths, ordinal, relation, out_hierarchy_handle);
}

inline ovstage_api_status_t ovstage_fetch_hierarchy_result(
    ovstage_hierarchy_instance_t* hierarchy, ovstage_hierarchy_handle_t hierarchy_handle,
    ovstage_hierarchy_result_t* out_result)
{
    return ovstage_fetch_hierarchy_result_ex(hierarchy, hierarchy_handle, out_result);
}

inline ovstage_api_status_t ovstage_release_hierarchy_result(
    ovstage_hierarchy_instance_t* hierarchy, const ovstage_hierarchy_result_t* result)
{
    return ovstage_release_hierarchy_result_ex(hierarchy, result);
}

inline ovstage_enqueue_result_t ovstage_release_hierarchy(
    ovstage_hierarchy_instance_t* hierarchy, ovstage_hierarchy_handle_t handle)
{
    return ovstage_release_hierarchy_ex(hierarchy, handle);
}

inline ovstage_api_status_t ovstage_get_hierarchy_computation_models(
    ovstage_hierarchy_instance_t* hierarchy,
    const ovstage_hierarchy_computation_model_desc_t** out_models, size_t* out_model_count)
{
    return ovstage_get_hierarchy_computation_models_ex(hierarchy, out_models, out_model_count);
}

inline ovstage_enqueue_result_t ovstage_compute_hierarchy(
    ovstage_hierarchy_instance_t* hierarchy, ovstage_hierarchy_computation_model_id_t computation_model_id,
    ovstage_ordinal_t input_ordinal, ovstage_ordinal_t output_ordinal)
{
    return ovstage_compute_hierarchy_ex(hierarchy, computation_model_id, input_ordinal, output_ordinal);
}
#endif

#include "ovstage_instancing.h"
#include "ovstage_population.h"

#endif /* OVSTAGE_H */
