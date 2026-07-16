/* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * ovstage population: populates an ovstage instance from USD.
 *
 * ovstage itself has no USD dependency; the population API is the bridge that
 * reads USD (files or inline USDA) and mirrors it into an ovstage instance
 * so renderers like ovrtx (via ovrtx_attach_ovstage) can consume it.
 *
 * Ordinal ownership. The application — typically a frame coordinator —
 * owns ordinal lifecycle: it advances the write floor for each tick, and
 * hands the current ordinal to the population API. The population API
 * never opens or commits an ordinal of its own; each call is invoked
 * against a caller-provided ordinal so multiple producers can fold into
 * one write floor per tick.
 *
 * Stage info. Regardless of `domains`, the stage units (metersPerUnit,
 * kilogramsPerUnit, upAxis) are authored onto the `/__ovstage_population_stage_info__`
 * prim.
 *
 * ovrtx pairing contract. An ovstage_population_open_usd_* call or
 * ovstage_population_apply_usd_changes call performs a wholesale or
 * structural stage mutation. A consumer attached in borrow mode
 * (for example, a renderer attached with `ovrtx_attach_ovstage`) must re-anchor
 * its derived state from the shared stage before its next render: the
 * canonical sequence is
 *     `ovstage_population_open_usd_*` /
 *     `ovstage_population_apply_usd_changes` -> wait_op ->
 *     `ovstage_advance_write_floor` -> `ovrtx_update_from_stage` ->
 *     `ovrtx_step_with_stage`.
 * `ovstage_population_apply_usd_time` is time-only and does not require
 * the consumer to rebuild; the consumer's incremental update path is
 * sufficient. No additional ovstage_population_* C entry point is needed
 * for this pairing. No additional public ovstage symbols are required for
 * this integration path.
 */

#ifndef OVSTAGE_POPULATION_H
#define OVSTAGE_POPULATION_H

#include "ovstage_api/ovstage_api.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** Coarse population domains for the canonical population entry points.
     *  Bitmask — OR values together.
     *
     *  `OVSTAGE_POPULATION_DOMAIN_NONE` (0) selects no data domain: a `domains`
     *  argument of 0 populates the stage units (metersPerUnit, kilogramsPerUnit,
     *  upAxis) only — those are authored regardless of `domains` — and nothing
     *  else. Pass `OVSTAGE_POPULATION_DOMAIN_ALL` to populate everything. */
    typedef enum
    {
        OVSTAGE_POPULATION_DOMAIN_NONE      = 0u,
        /** Meshes, lights, materials, and cameras. */
        OVSTAGE_POPULATION_DOMAIN_RENDERING = 1u << 0,
        /** Colliders, rigid bodies, joints, articulations, and the physics
         *  schema attributes/relationships authored on them. */
        OVSTAGE_POPULATION_DOMAIN_PHYSICS   = 1u << 1,
        OVSTAGE_POPULATION_DOMAIN_ALL       = OVSTAGE_POPULATION_DOMAIN_RENDERING |
                                              OVSTAGE_POPULATION_DOMAIN_PHYSICS,
    } ovstage_population_domain_t;

    /** Opaque id of an enqueued population operation, returned by the async
     *  entry points and passed to `ovstage_population_wait_op`. Zero is never a
     *  live op id. */
    typedef uint64_t ovstage_population_op_id_t;

#define OVSTAGE_POPULATION_INVALID_OP_ID ((ovstage_population_op_id_t)0)

    /** Opaque handle to a USD reference added by
     *  `ovstage_population_add_usd_reference_from_file` / `_from_string`, passed to
     *  `ovstage_population_remove_usd_reference` to take it back out. Reserved synchronously
     *  by the add call (so it is valid immediately, before the op runs) and freed
     *  by `ovstage_population_remove_usd_reference`, `_reset_usd`, or a fresh `_open_usd_*`.
     *  Zero is never a live handle. */
    typedef uint64_t ovstage_population_usd_reference_handle_t;

#define OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE ((ovstage_population_usd_reference_handle_t)0)

    /** Result of enqueuing a population operation.
     *
     *  Population runs asynchronously: `status` reports only that the operation
     *  was accepted (`OVSTAGE_OK`) and the work runs on a background worker —
     *  call `ovstage_population_wait_op(op_id)` to await completion and obtain
     *  the outcome (with detail via `ovstage_population_get_last_error`).
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

    /** The reserved prim path that population authors stage units onto
     *  (metersPerUnit, kilogramsPerUnit, upAxis, etc.) — see "Stage info" above —
     *  regardless of `domains`. Consumers read those units off this prim.
     *
     *  Returned as an `ovx_string_t` view over a static, null-terminated literal:
     *  `.ptr` is a valid C string and `.length` its byte length. */
    static inline ovx_string_t ovstage_population_stage_info_path(void)
    {
        static const char path[] = "/__ovstage_population_stage_info__";
        ovx_string_t result;
        result.ptr = path;
        result.length = sizeof(path) - 1;
        return result;
    }

    /** The reserved `usd-prim-type` value population authors onto prims that have
     *  no USD type (typeless `def "Foo"` containers). USD allows a defined prim to
     *  carry no type name, but ovstage rows are keyed/created through their prim
     *  type; authoring this synthetic type makes a typeless prim a first-class,
     *  queryable row so prim-tree/hierarchy walks can cross it (e.g. a typeless
     *  `/World` between `/` and its populated descendants). It is namespaced to
     *  never collide with a real USD type, and consumers can filter `usd-prim-type`
     *  against this value to recognize originally-typeless prims.
     *
     *  Returned as an `ovx_string_t` view over a static, null-terminated literal:
     *  `.ptr` is a valid C string and `.length` its byte length. */
    static inline ovx_string_t ovstage_population_untyped_type_name(void)
    {
        static const char name[] = "__ovstage_population_untyped__";
        ovx_string_t result;
        result.ptr = name;
        result.length = sizeof(name) - 1;
        return result;
    }

    /** @defgroup ovstage_population_lifecycle Lifecycle and population
     *
     *  Per-stage population state is created lazily on first use of any populate
     *  entry point and released automatically when the ovstage instance is
     *  destroyed via `ovstage_destroy_instance`.
     *  @{
     */

    /**
     * Enqueue an asynchronous operation to populate an ovstage instance from a
     * USD file.
     *
     * Replaces any USD content previously loaded into this ovstage with the
     * contents of @p path. This both loads the USD and populates the ovstage in
     * one operation. Initial population is one-shot; live edits
     * made afterwards are picked up by `ovstage_population_apply_usd_changes` /
     * `ovstage_population_apply_usd_time`.
     *
     * Per-stage population state is created lazily on first use and released
     * automatically when @p stage is destroyed.
     *
     * @param stage      ovstage instance to populate into.
     * @param path       Filesystem path to a `.usda` / `.usdc` / `.usd` file, or
     *                   an Omniverse Nucleus URL (`omniverse://server/path.usd`),
     *                   as an `ovx_string_t` view.
     * @param ordinal    Accumulating ordinal owned by the caller.
     * @param time       Time in seconds at which to evaluate attributes;
     *                   converted to a USD time code via the stage's
     *                   timeCodesPerSecond.
     * @param domains    Bitmask of `ovstage_population_domain_t` selecting which
     *                   data domains to populate. `0` (DOMAIN_NONE) populates the
     *                   stage units only; pass `OVSTAGE_POPULATION_DOMAIN_ALL` for
     *                   everything.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the stage cannot be populated, the file cannot be opened, or population
     *  failed — are surfaced by `ovstage_population_wait_op`, with detail in
     *  `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_open_usd_from_file(
        ovstage_instance_t* stage,
        ovx_string_t        path,
        ovstage_ordinal_t   ordinal,
        double              time,
        uint32_t            domains);

    /**
     * Enqueue an asynchronous operation to populate an ovstage instance from an
     * inline USDA string.
     *
     * Same semantics as `ovstage_population_open_usd_from_file` but the USD content is
     * provided directly as text instead of loaded from disk.
     *
     * @param stage      ovstage instance to populate into.
     * @param usda       USDA content (e.g. "#usda 1.0\n...") as an `ovx_string_t` view.
     * @param ordinal    Accumulating ordinal owned by the caller.
     * @param time       Time in seconds; converted to a USD time code via the
     *                   stage's timeCodesPerSecond.
     * @param domains    Bitmask of `ovstage_population_domain_t` selecting which
     *                   data domains to populate. `0` (DOMAIN_NONE) populates the
     *                   stage units only; pass `OVSTAGE_POPULATION_DOMAIN_ALL` for
     *                   everything.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the stage cannot be populated, the USDA content fails to parse, or
     *  population failed — are surfaced by `ovstage_population_wait_op`, with
     *  detail in `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_open_usd_from_string(
        ovstage_instance_t* stage,
        ovx_string_t        usda,
        ovstage_ordinal_t   ordinal,
        double              time,
        uint32_t            domains);

    /**
     * Enqueue an asynchronous operation to add a USD file as a reference at
     * @p target_path.
     *
     * This edits only the USD source, additively — existing USD content is left
     * untouched. The shape of the merge depends on @p target_path (see below): a new
     * prim is defined and @p ref_file_path referenced on it, the reference is added
     * onto an existing prim, or — for the root "/" — the content is merged in at the
     * top level. Like `ovstage_population_reset_usd`, it does not itself touch the
     * ovstage; call `ovstage_population_apply_usd_changes` afterwards to propagate the
     * added subtree into the ovstage (at the time and domains already in effect from
     * the initial `_open_usd_*`).
     *
     * Requires existing population state: call one of the `_open_usd_*` entry
     * points first. The enqueue is rejected (`status`) if this stage has never been
     * populated.
     *
     * @param stage         ovstage instance.
     * @param ref_file_path Filesystem path to a `.usda` / `.usdc` / `.usd` file, or an
     *                    Omniverse Nucleus URL (`omniverse://server/path.usd`), as an
     *                    `ovx_string_t` view — the layer to reference.
     * @param target_path Absolute path (starting with '/') selecting how the content
     *                    is merged, additively in every case:
     *                    - the root "/" — the content is merged in at the top level
     *                      (its top-level prims are added to the stage without
     *                      clobbering the existing stage's units/upAxis/defaultPrim);
     *                    - an existing prim path — the reference is added onto that
     *                      prim and the referenced subtree composes in beneath it; the
     *                      prim and its prior content are left in place;
     *                    - a not-yet-existing prim path — a new prim is defined there
     *                      (any missing ancestors are defined) and the layer referenced on it.
     * @param out_handle  [out, optional] Receives a handle for the added reference,
     *                    reserved synchronously (valid before the op runs) for a
     *                    later `ovstage_population_remove_usd_reference`. Set to
     *                    `OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE` if the enqueue is
     *                    rejected. A non-invalid handle does not by itself mean the
     *                    reference was added — await the op to confirm.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the file cannot be opened, or the target path is not a valid absolute path —
     *  are surfaced by `ovstage_population_wait_op`, with detail in
     *  `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_add_usd_reference_from_file(
        ovstage_instance_t*              stage,
        ovx_string_t                     ref_file_path,
        ovx_string_t                     target_path,
        ovstage_population_usd_reference_handle_t* out_handle);

    /**
     * Enqueue an asynchronous operation to add inline USDA content as a reference
     * at @p target_path.
     *
     * Same semantics as `ovstage_population_add_usd_reference_from_file` but the
     * USD content is provided directly as text instead of loaded from disk. The
     * inline layer is held alive for as long as the reference exists (until
     * `ovstage_population_remove_usd_reference` / `_reset_usd` / a fresh `_open_usd_*`).
     *
     * @param stage       ovstage instance.
     * @param ref_str     USDA content (e.g. "#usda 1.0\n...") as an `ovx_string_t`
     *                    view — the layer to reference.
     * @param target_path Absolute path (starting with '/') selecting how the content
     *                    is merged, additively in every case — the root "/" merges the
     *                    content at the top level, an existing prim path adds the
     *                    reference onto that prim, a not-yet-existing prim path defines
     *                    a new prim there. See
     *                    `ovstage_population_add_usd_reference_from_file`.
     * @param out_handle  [out, optional] Receives a handle for the added reference,
     *                    reserved synchronously, for a later
     *                    `ovstage_population_remove_usd_reference`. See
     *                    `ovstage_population_add_usd_reference_from_file`.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the USDA content fails to parse, or the target path is not a valid absolute
     *  path — are surfaced by `ovstage_population_wait_op`, with detail in
     *  `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_add_usd_reference_from_string(
        ovstage_instance_t*              stage,
        ovx_string_t                     ref_str,
        ovx_string_t                     target_path,
        ovstage_population_usd_reference_handle_t* out_handle);

    /**
     * Enqueue an asynchronous operation to remove a USD reference previously added
     * by `ovstage_population_add_usd_reference_from_file` / `_from_string`.
     *
     * This edits only the USD source, undoing exactly what the matching add
     * introduced: a prim the add defined is removed whole (with its referenced
     * subtree); a reference the add placed onto a pre-existing prim is removed on its
     * own, leaving that prim and its prior content in place; a root "/" merge removes
     * just the sublayer the add inserted. Like the add, it does not itself touch the
     * ovstage; call `ovstage_population_apply_usd_changes` afterwards to propagate the
     * removal (the affected prims are tombstoned out of the ovstage). For an inline
     * (`_from_string`) reference this also releases the held layer. Carries no ordinal.
     *
     * @param stage   ovstage instance.
     * @param handle  Handle returned by an add-reference call. Removing the same
     *                handle twice, or a handle whose add never completed, fails
     *                when the op runs.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue (invalid handle, or no population state) is reported in
     *  `status`; a run-time failure (unknown/already-removed handle, or the prim
     *  could not be removed) is surfaced by `ovstage_population_wait_op`.
     */
    ovstage_population_enqueue_result_t ovstage_population_remove_usd_reference(
        ovstage_instance_t*             stage,
        ovstage_population_usd_reference_handle_t handle);

    /**
     * Enqueue an asynchronous operation to reset (clear) all USD source content
     * from this stage.
     *
     * Unlike the open entry points, which load and populate in one step, this
     * edits only the USD source: call `ovstage_population_apply_usd_changes`
     * afterwards to propagate the cleared state into the ovstage, or repopulate
     * with `ovstage_population_open_usd_from_file` / `_open_usd_from_string`.
     * Carries no ordinal.
     *
     * @param stage  ovstage instance.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue (no population state) is reported in `status`; a run-time
     *  failure is surfaced by `ovstage_population_wait_op`.
     */
    ovstage_population_enqueue_result_t ovstage_population_reset_usd(
        ovstage_instance_t* stage);

    /**
     * Enqueue an asynchronous operation to advance the time and propagate
     * time-sampled attribute changes into the ovstage. Call once per simulation
     * tick when playing back time-sampled USD content.
     *
     * Samples time-sampled attributes from the latest USD state at @p time: their
     * values are re-sampled from the current USD, so edits to an attribute's time
     * samples are reflected at the new time even if
     * `ovstage_population_apply_usd_changes` has not been called since they were made.
     *
     * Honors the data domains selected by the initial `_open_usd_*`: only those
     * domains are re-evaluated at the new time.
     *
     * RENDERING — additional behavior: for the RENDERING domain this call also
     * reflects pending *structural* USD edits (prim add/remove, and attribute or
     * relationship changes that are not time samples) at the new time, even if
     * `ovstage_population_apply_usd_changes` has not been called. Other domains advance
     * time for time-sampled values only and leave such structural edits to be applied
     * by `ovstage_population_apply_usd_changes`. This RENDERING-specific behavior is
     * expected to converge with the other domains over time.
     *
     * @param stage      ovstage instance.
     * @param ordinal    Accumulating ordinal owned by the caller.
     * @param time       New time in seconds; converted to a USD time code via
     *                   the stage's timeCodesPerSecond.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue (no population state) is reported in `status`; a run-time
     *  failure is surfaced by `ovstage_population_wait_op`.
     */
    ovstage_population_enqueue_result_t ovstage_population_apply_usd_time(
        ovstage_instance_t* stage,
        ovstage_ordinal_t   ordinal,
        double              time);

    /**
     * Enqueue an asynchronous operation to propagate any USD edits accumulated
     * since the last call (including content emptied by
     * `ovstage_population_reset_usd`) into the ovstage.
     *
     * Intended for frame-coordinator pre-commit hooks: call once per tick
     * before consumers read from the ovstage so any pending USD-side edits
     * are visible. A no-op (returns success) when nothing has changed.
     *
     * Honors the data domains selected by the initial `_open_usd_*`: only those
     * domains' edits are propagated.
     *
     * @param stage    ovstage instance.
     * @param ordinal  Accumulating ordinal owned by the caller.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue (no population state) is reported in `status`; a run-time
     *  failure is surfaced by `ovstage_population_wait_op`. A no-op (nothing
     *  changed) returns `OVSTAGE_OK`.
     */
    ovstage_population_enqueue_result_t ovstage_population_apply_usd_changes(
        ovstage_instance_t* stage,
        ovstage_ordinal_t   ordinal);

    /**
     * Wait for completion of all operations up to and including @p op_id, or until
     * @p timeout_ns elapses. Because the wait covers earlier operations too, any of
     * them may be the one that failed — see @p out_wait_result.
     *
     * `timeout_ns = 0` polls; `OVSTAGE_TIMEOUT_INFINITE` blocks indefinitely.
     *
     * @param stage            ovstage instance.
     * @param op_id            Operation id from a population enqueue result.
     * @param timeout_ns       Nanoseconds to wait; `OVSTAGE_TIMEOUT_INFINITE` blocks.
     * @param out_wait_result  [out, optional] Receives the ids of every op that
     *                         failed within the awaited range and, on timeout, the
     *                         lowest still-pending op id. May be `NULL` if the
     *                         caller only needs the return code. See
     *                         `ovstage_population_op_wait_result_t`.
     * @return
     *  - `OVSTAGE_OK` if the awaited op (and all ops before it) completed
     *    successfully.
     *  - `OVSTAGE_ERROR_TIMEOUT` if the range did not complete within @p timeout_ns.
     *  - `OVSTAGE_ERROR_OP_FAILED` if any op in the range failed; the failing ids
     *    are reported in @p out_wait_result, with per-op detail available from
     *    `ovstage_population_get_last_op_error` and the primary failure mirrored
     *    into `ovstage_population_get_last_error`.
     *  - `OVSTAGE_ERROR_INVALID_ARGUMENT` for an unknown op id (zero, or never
     *    enqueued), or when @p stage has no population state.
     */
    ovstage_api_status_t ovstage_population_wait_op(
        ovstage_instance_t*                  stage,
        ovstage_population_op_id_t           op_id,
        ovstage_timeout_ns_t                 timeout_ns,
        ovstage_population_op_wait_result_t* out_wait_result);

    /** @} */ // end of ovstage_population_lifecycle

    /** @defgroup ovstage_population_diagnostics Diagnostics
     *  @{
     */

    /** Returns the thread-local error string for the latest population call on
     *  this thread, regardless of which ovstage instance produced it. For an
     *  op that failed asynchronously, this holds the primary failure only after
     *  `ovstage_population_wait_op` has observed it on this thread; for a rejected
     *  enqueue it is set on return. Valid until the next population call on the
     *  same thread. Returns `{NULL, 0}` when no error has been recorded. */
    ovx_string_t ovstage_population_get_last_error(void);

    /** Returns the human-readable error string for @p op_id, as reported by the
     *  last `ovstage_population_wait_op` call on this thread (i.e. an op id listed
     *  in that call's `ovstage_population_op_wait_result_t::error_op_ids`). Valid
     *  until the next `ovstage_population_wait_op` call on the same thread. Returns
     *  `{NULL, 0}` if @p op_id is not known to have failed on this thread. */
    ovx_string_t ovstage_population_get_last_op_error(ovstage_population_op_id_t op_id);

    /** @} */ // end of ovstage_population_diagnostics

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_POPULATION_H */
