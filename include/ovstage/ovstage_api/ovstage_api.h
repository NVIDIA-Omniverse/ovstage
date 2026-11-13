/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage_api.h
 * @brief ovstage_api — vtable-based contract for asynchronous, ordinal-keyed
 *        zero-copy simulation data access.
 *
 * @details
 * ovstage_api provides a unified C API for reading, writing, and managing
 * simulation data (transforms, velocities, materials, metadata) across CPU
 * and GPU memory. It is the abstract contract that various stage
 * implementations can provide.
 *
 * Consumers receive an `ovstage_instance_t*` (vtable + context) and
 * invoke functions either directly through the vtable, via the core
 * convenience wrappers in `ovstage_api_utils.h`, or via sibling extension
 * APIs such as `ovstage_instancing.h`.
 *
 * ## Execution and Ordering Model
 *
 * Every operation that mutates the stage or produces stage data follows a
 * uniform asynchronous submit/observe pattern, but execution order across
 * operations is governed by ordinal-keyed rules rather than a single global
 * stream:
 *
 * - **Enqueue (synchronous).** Every state-mutating or stage-data-producing
 *   slot returns an `ovstage_enqueue_result_t` (status + `op_index`). The
 *   call returns immediately; the operation itself is queued and executed
 *   later, subject to the ordering rules below. Asynchronous calls that
 *   produce stage data also reserve a typed handle (`ovstage_query_handle_t`,
 *   `ovstage_read_handle_t`, `ovstage_map_handle_t`,
 *   `ovstage_ordinal_query_handle_t`) that may immediately be passed to
 *   subsequent enqueues as input.
 *
 * - **Ordinal-keyed write ordering.** Writes, deletes, and map/unmap commits
 *   carry an explicit `ordinal`. Two such ops with the same `ordinal`
 *   execute in submission order; ops at different ordinal values are
 *   independent and may execute concurrently.
 *
 * - **Canonical attribute identity.** Aliases of the same stored column share
 *   write floors, ordinal-based change tracking, and overlap checks.
 *
 * - **Ordinal admission.** The configured policy governs changes to stage values
 *   and prim lifecycles. Under the STRICTLY_INCREASING policy, a mutation
 *   must use an ordinal above the applicable floors and every retained committed
 *   change to each affected (prim path, canonical attribute) cell. Prim creation,
 *   adoption, and whole-prim deletion also check the affected prim's lifecycle
 *   history. Disjoint cells may share an ordinal; submission order does not
 *   permit rewriting the same cell at that ordinal. A failed operation that
 *   publishes nothing leaves that ordinal available, subject to current state.
 *   OVStage process configuration can select NONDECREASING to allow equal unsealed
 *   ordinals while rejecting older intersecting history, or FLOOR_ONLY to skip
 *   retained value history and separate prim-lifecycle admission. All policies
 *   preserve the operation's applicable write floors, change tracking and type
 *   checks. See ovstage_ordinal_admission_policy_t and each API's documentation
 *   for its specific guarantees and limitations.
 *
 * - **Reads and queries are independent.** Reads (`read_attributes`) of
 *   recorded columns are validated against the effective write floor and never
 *   participate in write ordering. The floor is the ordinal through which data
 *   is sealed, and it starts at 0: a write is only readable once the caller
 *   advances the floor to cover its ordinal. An explicit ordinal range first
 *   selects attribute/path pairs that changed in it; a snapshot checks all
 *   queried paths for each requested attribute.
 *   A selected change at or before the requested end and above the floor fails
 *   with OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION. A read whose requested values are
 *   no longer available fails with OVSTAGE_ERROR_OUT_OF_RANGE; see
 *   ovstage_ordinal_range_t for current storage limitations. Queries (`query`) resolve
 *   against the latest committed stage state at execution time and do not
 *   order with each other or with writes.
 *
 * - **Write-floor advances are ordinal-keyed.** Advancing the write floor
 *   (global or per-attribute) to ordinal N is implicitly ordered after all
 *   writes/deletes/map commits at ordinals <= N (within the affected
 *   attribute scope). Writes at ordinals > N remain independent of the
 *   advance.
 *
 * - **Fetch with timeout.** All stage data is obtained through
 *   `fetch_*(handle, timeout, out)` slots. `timeout == 0` is a non-blocking
 *   poll; `OVSTAGE_TIMEOUT_INFINITE` blocks until ready.
 *   `OVSTAGE_ERROR_TIMEOUT` is returned when the result is not yet
 *   available. Iterating-fetch slots (e.g. `fetch_read_next`) return
 *   `OVSTAGE_ERROR_END_OF_ITERATION` once all groups have been consumed.
 *
 * - **Operation synchronization.** Use `wait_op(op_id, timeout, ...)` to wait
 *   for completion of a specific enqueued operation (and any operations it
 *   transitively depends on under the ordinal-keyed rules above) and to collect
 *   per-op errors. `wait_op` does *not* imply waiting for all prior enqueues
 *   on the instance — operations in unrelated ordinal buckets may still be in
 *   flight.
 *
 * - **Result vs. handle lifetime.** Result payloads returned by `fetch_*`
 *   slots are released synchronously (`release_groups`,
 *   `release_query_result`) once the caller is done reading them.
 *   Persistent handles are released through per-handle-ordered `release_*`
 *   slots (those returning `ovstage_enqueue_result_t`): the release
 *   waits for in-flight operations referencing that handle to complete
 *   before reclaiming resources, but does not synchronize across unrelated
 *   handles.
 *
 * - **Zero-copy by default.** Read groups expose pointers into internal storage.
 * - **DLTensor interchange.** All tensor data uses DLPack's DLTensor for
 *   zero-copy interop with Python/ML ecosystems.
 *
 * ## Thread Safety
 *
 * All slots are thread-safe when called from multiple threads on the same live
 * instance or on different live instances in the same process, including GPU
 * hierarchy computation. Coordinate object lifetimes and sequence dependent
 * operations explicitly; thread safety does not imply parallel execution.
 * GPU synchronization between producer and consumer is the caller's
 * responsibility (cuda_sync-based coordination).
 *
 * ## Built-in Attributes (Metadata)
 *
 * Implementations are expected to populate the following metadata attributes
 * automatically and make them queryable/filterable. These can be used in
 * filter predicates and read like any other attribute:
 *
 * | Token Name       | Type           | Description                    |
 * |------------------|----------------|--------------------------------|
 * | `usd-path`          | path-id        | Prim path (e.g. "/World/Env")  |
 * | `usd-schemas`       | token-id array | Applied schemas list           |
 * | `usd-ancestor-types`| token-id array | Ancestor prim-type tags (schema inheritance, e.g. Imageable/Xformable/Boundable) |
 * | `usd-prim-type`     | token-id       | Prim type name                 |
 * | `usd-parent`        | path-id        | Parent prim path               |
 * | `usd-active`        | bool           | Prim active state (NOT supported — see below) |
 * | `usd-children`      | path-id array  | Child prim paths               |
 *
 * `usd-prim-type`, `usd-schemas`, and `usd-ancestor-types` are recorded columns
 * and writable through the ordinary write and map contracts (they read, filter with
 * `HAS`/`CONTAINS`, and support ordinal-range change reads like any recorded
 * column). A `usd-ancestor-types` (or `usd-schemas`) write replaces the complete
 * set of tokens for that column (adds the newly listed tokens, drops the rest).
 * `usd-ancestor-types` and `usd-prim-type` are **independent** columns:
 * neither write changes the other's values. The ancestor set is conceptually derived
 * from the prim type, so a caller that changes `usd-prim-type` is responsible for
 * writing the matching `usd-ancestor-types` (a stale ancestor set is not corrected
 * automatically). A typical replication writes both together. `usd-path`, `usd-parent`, and `usd-children`
 * are derived on demand from the stage's own state: a snapshot read returns
 * them in group(s) labeled with `range.end_ordinal` (`usd-children` batches its
 * ragged rows like other array reads, so large queries can span several
 * groups — iterate `fetch_read_next` to `END_OF_ITERATION` as with any
 * read), and writing, deleting, or mapping any of the four derived names
 * (including `usd-active`) returns `OVSTAGE_ERROR_NOT_SUPPORTED`. Whatever the
 * requested end, and like query membership itself, the synthesized values
 * reflect the latest committed
 * structural state — they carry no ordinal-keyed payload history and are not
 * bounded by the write floor the way recorded-column reads are. `usd-parent`
 * and `usd-children` also keep no per-write change stream, so a range
 * ("since"/"between") read reports them as never changed (zero groups) — poll
 * current values with snapshot reads instead. A range read of `usd-path` returns
 * one tombstone group (`is_delete`) for the query prims whose latest creation
 * or whole-prim delete in the range is a delete. A prim that is deleted and
 * then created again in the range gives no tombstone. Exception: this range
 * read is bounded by the write floor. It fails with
 * OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION if the effective write floor of
 * `usd-path` is less than `end_ordinal`, also when no prim changed above the
 * floor. Reason: the read cannot see an outstanding prim creation or delete,
 * so only the floor makes its result final. SCOPE_ALL, or SCOPE_INCLUDE that
 * lists `usd-path`, advances this floor. A
 * range read of `usd-path` does not return value groups for prims created in
 * the range. Prims without a value (a root prim's `usd-parent`,
 * a leaf prim's `usd-children`) contribute no row to the read group. Path-id
 * and token-id payloads cross as `uint64` handles resolvable through the
 * shared path dictionary. Reading an attribute that no queried prim carries
 * yields zero groups; it is not an error.
 *
 * `usd-active` is currently NOT supported: a live prim is always active, so
 * the attribute carries no information, and reads or filter predicates
 * naming it return `OVSTAGE_ERROR_NOT_SUPPORTED`. The name remains in the
 * table above for contract stability only and is subject to removal in a
 * future release.
 *
 * ## Attribute Selector Patterns and Reserved Names
 *
 * `read_attributes` accepts the selector patterns `*` and `prefix*` next to
 * attribute names (refer to `read_attributes`). An attribute name that
 * contains `*` is therefore reserved for read selection. Writes, maps,
 * deletes, query filter predicates, query attribute lists,
 * `advance_write_floor` (SCOPE_INCLUDE and SCOPE_EXCLUDE lists), and
 * `get_attribute_write_floor` reject such a name with
 * OVSTAGE_ERROR_INVALID_ARGUMENT. A rejected call does not change the stage.
 * The rejection does not change the ownership rules of write tensors (refer
 * to ovstage_write_data_t): write_attributes still takes the supplied
 * managed tensors and releases each one exactly once.
 * SCOPE_ALL does not use its attribute list, so it does not check the list.
 *
 * @version 0.4.0
 * @date 2026-05-27
 */

#ifndef OVSTAGE_API_H
#define OVSTAGE_API_H

#include "ovstage_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════════
 * Vtable
 *
 * Single combined vtable for the ovstage_api contract. Every slot takes
 * `ovstage_data_plane_context_t* context` as the first argument. Return type
 * is `ovstage_enqueue_result_t` for asynchronous enqueues, or
 * `ovstage_api_status_t` for synchronous calls. Diagnostics slots return
 * `void`, `const char*`, or `ovx_string_t` as noted on each slot.
 *
 * Slot groups:
 * - Op tracking            (2) : wait_op, release_op
 * - Write-floor advance    (1) : advance_write_floor
 * - Ordinal queries        (4) : get_oldest_preserved_ordinal,
 *                                get_attribute_write_floor,
 *                                fetch_ordinal,
 *                                release_ordinal_query
 * - Query                  (5) : query, query_from_path_list,
 *                                fetch_query_result, release_query_result,
 *                                release_query
 * - Read                   (4) : read_attributes, fetch_read_next,
 *                                release_groups, release_read
 * - Write copy-in          (2) : write_attribute, write_attributes
 * - Mapped writes         (4) : map_attribute, fetch_map_next, unmap_group,
 *                                unmap_attribute
 * - Structural             (1) : delete_attributes
 * - Diagnostics            (3) : get_version, get_error_string,
 *                                get_last_op_error
 * - Resources              (1) : get_path_dictionary
 * - Extensions             (1) : query_extension
 * - Subtree clone          (2) : clone, fetch_clone_result
 *
 * Device placement is not a vtable slot: it is the experimental
 * "ovstage.placement" extension, declared in
 * experimental/ovstage_placement_extension.h. The subtree clone slots are
 * appended after the released layout of 0.3: a clone is an ordinal-keyed
 * write of the data plane, awaited through wait_op like every operation.
 * ═══════════════════════════════════════════════════════════════════════════════ */

struct ovstage_data_plane_vtable_t
{
    /* ───────────────────────────────────────────────────────────────────────
     * Op tracking
     *
     * Every asynchronous slot (one returning ovstage_enqueue_result_t)
     * carries an op id. Use wait_op to wait for completion of a specific op
     * and any operations it transitively depends on under the ordinal-keyed
     * ordering rules. Because there is no global stream, wait_op does *not*
     * implicitly wait for all prior enqueues on the instance — operations
     * in unrelated ordinal buckets (or against unrelated handles) may still be
     * in flight.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Wait for completion of an enqueued operation, with timeout.
     *
     * Blocks until the operation identified by `op_id` (and any operations
     * it transitively depends on under the ordinal-keyed ordering rules) has
     * completed, or until `timeout` elapses. `timeout = 0` makes this a
     * non-blocking poll. `OVSTAGE_TIMEOUT_INFINITE` blocks indefinitely.
     * Operations in unrelated ordinal buckets may still be in flight when this
     * call returns OVSTAGE_OK.
     *
     * On return, `out_wait_result->error_op_ids` lists ops observed to have
     * failed since the previous wait_op call on this thread. For each id,
     * the error string can be retrieved via `get_last_op_error`. The list
     * and strings are transient thread-local data, invalidated by the next
     * wait_op call on the same thread.
     */
    ovstage_api_status_t (*wait_op)(
        ovstage_data_plane_context_t* context,
        ovstage_op_id_t               op_id,
        ovstage_timeout_ns_t          timeout,
        ovstage_op_wait_result_t*     out_wait_result);

    /**
     * @brief Release tracking state associated with an op id (synchronous).
     *
     * Safe to call once the op is known to have completed. After this call
     * the op id may not be used again with wait_op or get_last_op_error.
     */
    ovstage_api_status_t (*release_op)(
        ovstage_data_plane_context_t* context,
        ovstage_op_id_t               op_id);

    /* ───────────────────────────────────────────────────────────────────────
     * Write-floor advance
     *
     * Write-floor advancement is ordinal-keyed: a call advancing the write
     * floor (global or per-attribute) to N is implicitly ordered after all
     * writes/deletes/map commits at ordinals <= N (for the affected attribute
     * scope). Writes at ordinals > N are independent of the advance.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a write-floor advance with scope control.
     *
     * `desc->scope` selects the attributes affected:
     *  - SCOPE_ALL: advance the global write floor and every known attribute.
     *  - SCOPE_INCLUDE: advance only the listed attributes.
     *  - SCOPE_EXCLUDE: advance every known attribute EXCEPT the listed ones.
     *    With an empty attribute list, SCOPE_EXCLUDE behaves like SCOPE_ALL.
     *
     * Advances clamp to the current value (max), so they never regress and a
     * non-monotonic ordinal is a no-op rather than an error.
     *
     * With SCOPE_INCLUDE or SCOPE_EXCLUDE, a listed name that contains `*`
     * fails with OVSTAGE_ERROR_INVALID_ARGUMENT (reserved for read selection).
     *
     * @param context The context of the data plane.
     * @param desc Write-floor advance descriptor.
     * @return Enqueue result with status + op_index.
     */
    ovstage_enqueue_result_t (*advance_write_floor)(
        ovstage_data_plane_context_t*      context,
        const ovstage_write_floor_desc_t*  desc);

    /* ───────────────────────────────────────────────────────────────────────
     * Ordinal queries
     *
     * Reading the oldest-preserved ordinal or a per-attribute / global write
     * floor is a stage-state-producing operation. Each enqueue reserves an
     * ordinal-query handle; a single fetch_ordinal slot retrieves the scalar
     * result. Release the handle via release_ordinal_query.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a query for the inclusive retained-history frontier.
     *
     * Ordinal queries at or above the returned frontier are exact. Older
     * history may have been coalesced or discarded.
     *
     * @remark In the current implementation, this is the exact
     * change-membership frontier. Payload reads store only current values:
     * they return the current payload or tombstone, or
     * OVSTAGE_ERROR_OUT_OF_RANGE when a selected path changed after a
     * requested end below OVSTAGE_ORDINAL_MAX. The frontier also covers the retained
     * history of prim creations and whole-prim deletes that `usd-path` range
     * reads use, so these prim changes can raise it.
     */
    ovstage_enqueue_result_t (*get_oldest_preserved_ordinal)(
        ovstage_data_plane_context_t*   context,
        ovstage_ordinal_query_handle_t* out_handle);

    /**
     * @brief Enqueue a query for the write floor of a specific attribute.
     *
     * Pass an empty attribute (`{.token = 0, .string = {NULL, 0}}`) to query
     * the global write floor (minimum across all known attributes). A name
     * that contains `*` fails with OVSTAGE_ERROR_INVALID_ARGUMENT (reserved
     * for read selection).
     */
    ovstage_enqueue_result_t (*get_attribute_write_floor)(
        ovstage_data_plane_context_t*   context,
        ovx_token_or_string_t           attribute,
        ovstage_ordinal_query_handle_t* out_handle);

    /**
     * @brief Fetch the scalar result of any enqueued ordinal query.
     *
     * @return OVSTAGE_OK on success,
     *         OVSTAGE_ERROR_TIMEOUT if not ready within timeout,
     *         OVSTAGE_ERROR_OP_FAILED if the underlying enqueue failed.
     */
    ovstage_api_status_t (*fetch_ordinal)(
        ovstage_data_plane_context_t*  context,
        ovstage_ordinal_query_handle_t handle,
        ovstage_timeout_ns_t           timeout,
        ovstage_ordinal_t*             out_ordinal);

    /**
     * @brief Enqueue release of an ordinal-query handle.
     *
     * Per-handle ordered: the release waits for any in-flight fetch on the
     * same handle to complete before reclaiming resources.
     */
    ovstage_enqueue_result_t (*release_ordinal_query)(
        ovstage_data_plane_context_t*  context,
        ovstage_ordinal_query_handle_t handle);

    /* ───────────────────────────────────────────────────────────────────────
     * Query
     *
     * Query identifies prims matching a filter. The enqueue reserves a query
     * handle that is valid as soon as the call returns and may immediately
     * be used as input to other enqueues (reads, writes, deletes, maps).
     *
     * For targeting specific paths regardless of existence (UPSERT writes,
     * population) without a stage-side filter evaluation, use the
     * synchronous query_from_path_list slot.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a query of prims matching a filter, optionally
     *        discovering attributes for the matched set.
     *
     * The returned query handle is reserved synchronously. Reading the
     * discovered attribute list and total prim count requires a subsequent
     * fetch_query_result call. A filter predicate or an `attrs` entry with a
     * name that contains `*` fails with OVSTAGE_ERROR_INVALID_ARGUMENT
     * (reserved for read selection).
     */
    ovstage_enqueue_result_t (*query)(
        ovstage_data_plane_context_t* context,
        const ovstage_filter_t*       filter,
        const ovx_token_t*            attrs,
        size_t                        attr_count,
        ovstage_query_handle_t*       out_query_handle);

    /**
     * @brief Create a query handle from an explicit prim path list. Synchronous.
     *
     * The returned handle wraps a caller-owned, interned prim path list and
     * does not require evaluation against the stage. It may be used
     * immediately as input to any enqueue.
     */
    ovstage_api_status_t (*query_from_path_list)(
        ovstage_data_plane_context_t* context,
        ovx_primpath_list_t           path_list,
        ovstage_query_handle_t*       out_handle);

    /**
     * @brief Fetch the result (discovered attributes + summary) of a prior
     *        query enqueue. Pointers in `*out_result` are valid until
     *        release_query_result.
     *
     * @return OVSTAGE_OK on success,
     *         OVSTAGE_ERROR_TIMEOUT if not ready within timeout,
     *         OVSTAGE_ERROR_OP_FAILED if the underlying enqueue failed.
     */
    ovstage_api_status_t (*fetch_query_result)(
        ovstage_data_plane_context_t* context,
        ovstage_query_handle_t        query_handle,
        ovstage_timeout_ns_t          timeout,
        ovstage_query_result_t*       out_result);

    /**
     * @brief Release a query result payload (synchronous).
     *
     * After this call, the `attributes` array in `*result` is invalid. The
     * query handle itself remains valid and must be released separately via
     * release_query.
     */
    ovstage_api_status_t (*release_query_result)(
        ovstage_data_plane_context_t* context,
        const ovstage_query_result_t* result);

    /**
     * @brief Enqueue release of a query handle.
     *
     * Per-handle ordered: the release waits for any in-flight read/write/
     * map/delete enqueued against the same handle to complete before
     * reclaiming resources.
     */
    ovstage_enqueue_result_t (*release_query)(
        ovstage_data_plane_context_t* context,
        ovstage_query_handle_t        handle);

    /* ───────────────────────────────────────────────────────────────────────
     * Read
     *
     * Multi-attribute read: enqueue one read for multiple attributes, then
     * iterate groups via fetch_read_next. Reads target sealed data and never
     * participate in write ordering; reads also never order with each other.
     *
     * Only selected attribute/path pairs are checked. A selected change at or
     * before the requested end and above the write floor fails with
     * OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION. A read whose requested values are no
     * longer available fails with OVSTAGE_ERROR_OUT_OF_RANGE. A range that
     * selects nothing succeeds with no groups. See ovstage_ordinal_range_t.
     * A range read with a selector pattern (`*` or `prefix*`) has two more
     * checks that are not scoped to the selected keys: the global write floor
     * and the retention frontier (refer to read_attributes).
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a multi-attribute read.
     *
     * Each entry of `attrs` is an attribute selector:
     *  - an attribute name, for example `points`, selects that attribute;
     *  - `*` selects every attribute that the read can return, except the
     *    hidden attributes (see below);
     *  - a prefix followed by `*`, for example `primvars:*`, selects every
     *    attribute whose name starts with the prefix, with the same exception.
     *
     * A pattern has exactly one `*`, as its last character. The match is case
     * sensitive and uses the full attribute name. The `*` also matches an
     * empty suffix. A selector with a `*` in a different position, or with
     * more than one `*`, makes read_attributes return
     * OVSTAGE_ERROR_INVALID_ARGUMENT.
     *
     * Each read finds the matching names again. Thus a read includes names
     * that were written after the query was made, and the caller does not
     * need a new query. A snapshot read matches the names that the query
     * prims have now, and the names that have an unsealed change on the query
     * prims. Thus an unsealed delete of a name fails the read with
     * OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION until the floor of that name
     * reaches the delete. With an end below OVSTAGE_ORDINAL_MAX, a snapshot read also
     * matches the names that have a retained change or delete on the query
     * prims after the end. Thus a name that a later delete removed fails the
     * read with OVSTAGE_ERROR_OUT_OF_RANGE, as for an attribute name. The read
     * does not leave out a name that the prims had at the end.
     * A range read matches the names that have a retained change or delete
     * on the query prims in the range.
     *
     * Order and duplicates:
     *  - Explicit attribute names enter the selection first, in the order of
     *    `attrs` after exact-token deduplication. Repeated names add no columns.
     *  - Then come the names that the patterns add, sorted by name. The
     *    position of a pattern in `attrs` has no effect on this order.
     *  - A pattern does not add a name that the selection already has. Thus
     *    overlapping patterns, and a pattern that matches an attribute name in
     *    `attrs`, give each name one time.
     *
     * Hidden attributes: a name that starts with `_` or `usd-` is hidden,
     * except `usd-path`. A pattern selects a hidden name only when the prefix
     * of the pattern starts with the same namespace. Thus `*`, `u*`, and
     * `usd*` do not select `usd-schemas`, but `usd-*` and `usd-s*` do. `*`
     * does not select `_name`, but `_*` does. The rule applies to the
     * start of the name only: `*` selects `points:usd-metadata:interpolation`.
     * A read that gives a hidden name as an attribute name is not affected.
     *
     * Built-in attributes: patterns select them by name, with the rule above.
     *  - A pattern that matches `usd-path` selects it. A snapshot read gives
     *    the query prims. A range read gives the tombstones of the query prims
     *    deleted whole in the range.
     *  - A snapshot read with `usd-*` also selects `usd-parent`,
     *    `usd-children`, `usd-prim-type`, `usd-schemas`, and
     *    `usd-ancestor-types`. Each gives groups only for the query prims that
     *    have a value. `usd-children` groups are ragged arrays.
     *  - A range read with `usd-*` selects `usd-prim-type`, `usd-schemas`, and
     *    `usd-ancestor-types` when they changed in the range, as any recorded
     *    attribute. It never selects `usd-parent` or `usd-children`.
     *  - A longer prefix, such as `usd-p*`, selects the built-ins that it
     *    matches, with the same rules.
     *  - Patterns never select `usd-active`.
     *
     * A pattern that matches no name gives zero groups. An empty `attrs`, or
     * a query with no prims, also gives zero groups.
     *
     * A read with a pattern applies the checks of an attribute name to each
     * matched name. It also has these checks:
     *  - An outstanding write or delete on the query prims to a name that a
     *    pattern matches fails the read with OVSTAGE_ERROR_OP_FAILED. This
     *    includes a write that adds a new name.
     *  - A range read fails with OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION while the
     *    stage-wide floor is less than `end_ordinal`. This is true also when
     *    every matched name has a higher floor. The reason: a pattern can
     *    match a name that no prim has yet. That name has no floor of its
     *    own, so only the stage-wide floor seals it. Advance the floor with
     *    SCOPE_ALL before the read. SCOPE_INCLUDE, and SCOPE_EXCLUDE with a
     *    non-empty list, do not advance the stage-wide floor. Do not use
     *    get_attribute_write_floor with an empty attribute to check it: that
     *    call can report a higher value.
     *  - A range read fails with OVSTAGE_ERROR_OUT_OF_RANGE when
     *    `start_ordinal` is less than ovstage_get_oldest_preserved_ordinal.
     *    Records of deleted attributes below that value can be gone, so the
     *    pattern could miss a name.
     * The two range checks also apply when the query has no prims. When the
     * selection has patterns and attribute names, the checks apply to the
     * whole read.
     *
     * @remark This implementation stores only current values. Later recorded
     * changes can make a requested snapshot or change range unavailable;
     * see ovstage_ordinal_range_t for the checks and limitations.
     *
     * @remark Current limits of selector patterns:
     *  - A snapshot read with a pattern matches the names that the query prims
     *    have now. `end_ordinal` does not limit these names. A matched name
     *    with a recorded change or delete after an end below OVSTAGE_ORDINAL_MAX fails
     *    the read with OVSTAGE_ERROR_OUT_OF_RANGE, as for an attribute name.
     *  - `*` can return internal columns. It skips hidden names, but returns
     *    all other names that a query result reports in `attributes`. A later
     *    release can hide more names.
     *  - A range read with a pattern does not find attributes of the RENDERING
     *    population domain. The read finds names from change history, and
     *    these attributes have none. A range read of the attribute name also
     *    gives no groups for them.
     *  - A range read gives no `usd-path` tombstone for a prim that the
     *    RENDERING population domain removes.
     *  - A pattern sees the two alias names of one stored column as different
     *    names. After a write through one alias, a pattern that matches only
     *    the other alias does not find the change. A read of either alias name
     *    finds it. If both aliases changed in the range, a range read with a
     *    pattern that matches both gives the same rows under each name.
     */
    ovstage_enqueue_result_t (*read_attributes)(
        ovstage_data_plane_context_t* context,
        ovstage_query_handle_t        handle,
        const ovx_token_t*            attrs,
        size_t                        attr_count,
        ovstage_ordinal_range_t       range,
        ovstage_read_handle_t*        out_read_handle);

    /**
     * @brief Fetch up to capacity complete read groups.
     *
     * capacity must be positive; out_groups and out_count are required.
     * A valid call sets *out_count to zero before waiting. Timeout applies only
     * to the first group: return available groups without waiting to fill the
     * array. OVSTAGE_OK delivers 1..capacity groups; only those slots are valid.
     * A short batch does not imply completion. With no groups delivered, return
     * OVSTAGE_ERROR_END_OF_ITERATION, OVSTAGE_ERROR_TIMEOUT, or the read's error.
     * If group delivery fails after some groups have been delivered, return
     * OVSTAGE_OK; the next fetch reports that error. A group-delivery failure
     * is terminal for the read, including OUT_OF_MEMORY before any delivery:
     * every later fetch returns the same error, never END_OF_ITERATION.
     * release_read reclaims the undelivered groups. Timeouts and argument
     * validation errors do not latch a group-delivery failure.
     *
     * Deferred producer failures retain the single-group contract: an already
     * failed producer reports its own status; waiting on a producer that fails
     * reports OVSTAGE_ERROR_OP_FAILED.
     *
     * Concurrent fetches on one read handle are safe and return disjoint
     * groups. A short batch or deferred error may be observed on another
     * thread; serialize fetches when one consumer must observe every group
     * and the error. Each delivered group remains valid until release_groups,
     * independently of later fetches or release_read. Copying a descriptor
     * does not acquire another reference.
     */
    ovstage_api_status_t (*fetch_read_next)(
        ovstage_data_plane_context_t*       instance,
        ovstage_read_handle_t    read_handle,
        ovstage_timeout_ns_t     timeout,
        ovstage_read_group_t*    out_groups,
        size_t                   capacity,
        size_t*                  out_count);

    /**
     * @brief Release a batch of read groups' pinned storage (synchronous).
     *
     * Groups must be distinct live acquisitions from this instance, and may
     * come from different reads or fetches. Validate every ID before releasing
     * any: invalid, duplicate, or already released IDs release nothing.
     * A zero count succeeds and permits groups == NULL. A nonzero count
     * requires an array. Release each acquisition exactly once.
     *
     * Thread-safe; independent groups may be released in any order from any
     * thread. Do not concurrently release the same acquisition. The instance
     * must outlive its groups. Release does not wait for consumer GPU work;
     * callers must finish using all borrowed views before releasing them.
     */
    ovstage_api_status_t (*release_groups)(
        ovstage_data_plane_context_t*           instance,
        const ovstage_read_group_t*  groups,
        size_t                       group_count);

    /**
     * @brief Enqueue release of a read handle.
     *
     * Per-handle ordered: the release waits for any in-flight fetch_read_next
     * call on the same handle to complete before reclaiming resources.
     */
    ovstage_enqueue_result_t (*release_read)(
        ovstage_data_plane_context_t* context,
        ovstage_read_handle_t         read_handle);

    /* ───────────────────────────────────────────────────────────────────────
     * Write (Copy-In)
     *
     * Caller provides one or more tensors; the implementation copies/scatters
     * to internal storage. GPU synchronization (cuda_sync) and sparsity
     * (mask/index_map) are carried within ovstage_write_data_t itself.
     * Writes are ordinal-keyed.
     *
     * A prim may have at most one live attribute with a given name. The
     * attribute's schema is its logical kind (`data.is_array`), value layout,
     * and authored semantic. `data.is_array` is the sole authority for fixed
     * versus array storage; tensor count, shape, payload width, semantic,
     * attribute name, and existing storage do not infer or correct it. Writing
     * a different schema to an existing prim/name fails; delete that attribute
     * first and write it again with the new schema at a later ordinal, or
     * write it with `data.attribute_mode` OVSTAGE_ATTRIBUTE_MODE_FORCE.
     *
     * With OVSTAGE_ATTRIBUTE_MODE_FORCE, a target prim whose existing schema
     * differs from the written one has it replaced. A `NONE` semantic does not
     * request a role: on a prim whose storage is unchanged it keeps the
     * existing role, and on a prim whose storage is replaced the written
     * schema has no role.
     * Target prims whose schema already matches, and prims without the
     * attribute, are written as usual. The replacement is part of the write: it
     * is admitted once at the write's ordinal and is recorded there as the end
     * of the previous schema on the replaced prims together with the written
     * value. It is rejected while a replaced prim has an outstanding read or the
     * attribute an overlapping map; a rejected write leaves the previous schema
     * in place. A write that fails after the previous schema has ended on a prim
     * leaves that end recorded at the ordinal, and the prim then has no value for
     * the attribute; a corrected retry for that prim needs a later ordinal.
     * Other prims keep the schema they carry.
     * Reserved metadata attributes accept only OVSTAGE_ATTRIBUTE_MODE_REJECT.
     *
     * A write that would give `omni:fabric:worldMatrix` to a hierarchy parent or
     * ancestor of the written prims, or to a prim with a transform input between
     * a written prim and a descendant that carries it (see the transforms
     * documentation), is rejected while an outstanding read covers that prim,
     * like any structural write; the rejected write creates nothing (a write
     * with several entries keeps the partial-failure behavior documented for
     * write_attributes: entries applied before the rejected one remain
     * committed).
     *
     * Reads observe a replacement as the end of the previous schema and the
     * written value at the same ordinal. A change-range read that includes that
     * ordinal returns, for each replaced prim, a tombstone group
     * (`is_delete == true`) for the previous schema and a value group with the
     * written schema and value; the order of the two groups is unspecified. A
     * latest read returns only the written schema for a replaced prim. Prims
     * that still carry another schema under the same name are returned in
     * separate groups, one per schema.
     *
     * Fixed-size values use a lane-canonical schema: each transported data row
     * holds one attribute tuple, with the complete tuple width in
     * `DLDataType.lanes`. Compact convenience inputs may put tuple components
     * in trailing tensor dimensions, but those dimensions are folded into
     * lanes and are not retained as input shapes. Read and map results both
     * reconstruct scalar-lane component dimensions from the stored width and
     * semantic: [N] for scalars, [N, C] for tuples, and [N, S, S] for semantic
     * square matrices. Both expose explicit compact strides.
     * Logical prims select those rows directly
     * or through `data.index_map`; see ovstage_write_data_t and ovstage_data_t.
     * Without an index map, the leading dimension must equal the logical
     * element count; a flat component buffer is not inferred as wider rows.
     *
     * Reserved metadata follows the same attribute-write and row-coverage contract
     * as every other attribute. `usd-prim-type` requires `is_array == false`;
     * `usd-schemas` and `usd-ancestor-types` require `is_array == true`. A
     * mismatch is rejected, and none of them enables implicit broadcasting. Use
     * an explicit index_map when multiple target rows intentionally share
     * source data. Metadata rows are assignments: later values replace earlier
     * values, and an empty `usd-schemas` row clears the applied-schema set. A
     * write of any of the three is refused while any attribute of a target prim
     * has an outstanding read, because the change restructures the prim's
     * storage.
     *
     * To fill ovstage-owned writable buffers, use Map/Unmap.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a write of attribute values for prims identified by a
     *        query handle.
     *
     * This call follows the non-transactional write_attributes contract for a
     * single entry. A successfully applied entry commits its values, any new
     * prims or attributes, and their change records at the submitted ordinal.
     * A failed entry is not recorded as a completed write, although changes made
     * before the failure can remain visible to an attribute-presence (HAS)
     * query. Because this operation has only one entry, a failed application
     * leaves the submitted ordinal available; a corrected same-ordinal retry
     * remains admissible subject to current stage state. The exception is a
     * forcing write that ended a prim's previous schema before failing: that
     * end stays recorded, so a retry for that prim needs a later ordinal (see
     * OVSTAGE_ATTRIBUTE_MODE_FORCE above). A prim left by a failed
     * write counts as an existing target for INSERT and can make that retry fail
     * with OVSTAGE_ERROR_PRIM_NOT_FOUND; UPSERT can repair it. This call
     * also shares the recoverable-error and partial-failure behavior documented for
     * write_attributes.
     * Write-entry, tensor-layout, and attribute-schema
     * errors that can be proven during common preflight are therefore returned
     * synchronously with an invalid op_index; failures that require pending-query
     * resolution or execution remain observable through wait_op.
     * A successful enqueue can still fail ordinal admission during execution,
     * before this operation changes stage data.
     * When multiple inputs are independently invalid, callers must not rely on
     * error precedence; common preflight currently validates prim_mode before
     * query-handle readiness and enqueue-time ordinal checks.
     *
     * @param context The context of the data plane.
     * @param handle Query handle identifying target prims.
     * @param attribute Attribute to write. A name that contains `*` fails with
     *        OVSTAGE_ERROR_INVALID_ARGUMENT (reserved for read selection).
     * @param ordinal Must exceed applicable floors and satisfy the configured
     *        ordinal admission policy for affected cells and prim lifecycles at
     *        execution. The STRICTLY_INCREASING policy rejects equality
     *        with retained changes; NONDECREASING allows equality. Disjoint writes
     *        may share an ordinal and execute in submission order.
     * @param data Write payload (tensors + cuda_sync + mask/index_map).
     * @param prim_mode UPSERT (default) or INSERT.
     * @return Enqueue result with status + op_index. A synchronous preflight
     *         failure has an invalid op_index. After a successful enqueue,
     *         failures discovered only during pending-query resolution,
     *         state-race revalidation, or execution are surfaced
     *         through wait_op + get_last_op_error.
     */
    ovstage_enqueue_result_t (*write_attribute)(
        ovstage_data_plane_context_t* context,
        ovstage_query_handle_t        handle,
        ovx_token_or_string_t         attribute,
        ovstage_ordinal_t             ordinal,
        ovstage_write_data_t          data,
        ovstage_prim_mode_t           prim_mode);

    /* ───────────────────────────────────────────────────────────────────────
     * Map / Unmap (Mapped Writes)
     *
     * Iterator pattern for filling writable buffers and committing them.
     *
     * Flow:
     *   map_attribute → fetch_map_next (iterate, with timeout) → fill data
     *                → unmap_group [...] → unmap_attribute
     *
     * map_attribute, unmap_group, and unmap_attribute are asynchronous
     * enqueues; fetch_map_next is the synchronous fetch-with-timeout
     * iterator. All map/unmap operations in a session carry the session's
     * `ordinal` and are ordinal-keyed.
     *
     * Like write_attribute, a map session can *create* the target attribute
     * when it does not already exist. Creation takes effect after a successful
     * unmap, not by map setup. Because the map path hands the caller
     * writable storage before any tensor is produced, new attributes require a
     * descriptor dtype. If `desc.dtype` is zero-initialized, map_attribute uses
     * the existing attribute schema only when it is unambiguous for the target
     * prims. If the requested dtype conflicts with existing storage or
     * an explicit semantic differs from the existing semantic on a target
     * prim/name, including adding a role to a roleless attribute,
     * map_attribute fails; delete that attribute first, then map it
     * again with the new schema at a later ordinal. With `desc.attribute_mode`
     * OVSTAGE_ATTRIBUTE_MODE_FORCE and an explicit `desc.dtype`, setup accepts
     * those prims instead, and each successful group commit replaces their
     * schema with the mapped one at the session ordinal, under the same rules
     * as a forcing write (see write_attribute), including when a commit fails
     * after the previous schema has ended. Setup and each commit are
     * rejected while a prim they would replace has an outstanding read. A
     * session closed without committing leaves the existing schema in place.
     * An omitted semantic adopts an unambiguous compatible existing identity;
     * specify dtype and semantic when the target prims have multiple same-name
     * identities.
     *
     * The recorded reserved metadata (`usd-prim-type`, `usd-schemas`,
     * `usd-ancestor-types`) maps like any recorded column: the staging buffer
     * carries interned uint64 token ids, and each successful unmap applies them
     * to the prim's recorded metadata exactly as a copy-in write does (an empty
     * list row clears the set). Such a session is host-resident and canonical:
     * `desc.dtype` is zero or `{kDLUInt, 64, 1}`, `desc.semantic` is NONE,
     * `element_sizes` is required for the list forms and must be omitted for
     * `usd-prim-type`, a CUDA placement returns `OVSTAGE_ERROR_NOT_SUPPORTED`,
     * and a query that names a prim twice is rejected. Like a copy-in write of
     * these names, the commit is refused while any attribute of a mapped prim
     * has an outstanding read. A staged row whose ids do not name anything
     * fails the commit before any metadata changes; like every failed accepted
     * commit, that is terminal for the session.
     *
     * For array attributes (variable-size per prim), the caller additionally
     * provides element_sizes at map-enqueue time so the implementation can
     * size the per-prim writable staging buffers; see the parameter docs
     * below. Final backing storage is allocated when the group is committed.
     *
     * Successful map setup only makes writable staging buffers available. It
     * does not change stage values, create prims or attributes, modify schemas
     * or hierarchy, or record value or prim-lifecycle changes at the session's
     * ordinal. These effects commit only for groups whose unmap succeeds.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a map session for writing an attribute.
     *
     * The returned map handle is reserved synchronously and may immediately
     * be used with subsequent fetch_map_next / unmap_* calls.
     * A successful enqueue can still fail ordinal admission during setup,
     * before writable groups are made available. Observe setup failure through
     * wait_op or fetch_map_next.
     * Successful setup does not guarantee a later commit: unmap rechecks the
     * current layout, type/semantic compatibility, prim creation policy,
     * ordinal admission and read overlap before committing remaining data.
     * A commit that would give `omni:fabric:worldMatrix` to a hierarchy parent or
     * ancestor of the mapped prims, or to a prim with a transform input between
     * a mapped prim and a descendant that carries it (see the transforms
     * documentation), is rejected while an outstanding read covers that prim,
     * like any structural write; nothing is created.
     *
     * @remark Mapped buffers need not alias final backing storage; the current
     * implementation may copy or scatter them during unmap.
     *
     * @remark This slot maps on the host: ovstage_map_desc_t carries no device
     * field; map_attribute_on_device, in the experimental placement extension,
     * is the counterpart that names one.
     * unmap_group and unmap_attribute are shared by both, and their
     * `write_done_sync` is inbound -- the caller's "my kernel has finished
     * filling the buffer", which ovstage waits on before committing. It is
     * accepted only where there is device-side work to wait for: a fixed-size
     * session mapped on a CUDA device consumes it, while a host-mapped session
     * and any ragged payload commit reject a non-zero `stream` or `wait_event`.
     * Because that rejection is an accepted failure it is terminal for the
     * session. For a session from this slot, pass a zeroed
     * ovstage_cuda_sync_t. A cleanup-only close of unmap_attribute -- every
     * group already committed by unmap_group -- has no payload and never
     * inspects the sync, so this is not a general argument check.
     *
     * @param context The context of the data plane.
     * @param handle Query handle identifying target prims.
     * @param desc Map descriptor (attribute, dtype, semantic, prim_mode). See
     *        ovstage_map_desc_t. An attribute name that contains `*` fails with
     *        OVSTAGE_ERROR_INVALID_ARGUMENT (reserved for read selection).
     * @param ordinal Ordinal for this map session. Setup and publication must
     *        exceed applicable floors and satisfy the configured ordinal
     *        admission policy for affected cells and prim lifecycles. Disjoint
     *        maps may share an ordinal.
     * @param element_sizes For array (variable-size) attributes, the per-prim
     *        element count: `element_sizes[i]` is the number of array elements
     *        to allocate for the i-th prim in `handle`'s prim set. This sizes
     *        the per-prim writable staging handed back in the group; the final
     *        ragged backing storage is allocated at commit, per the @remark
     *        above. Pass NULL for fixed-size (scalar /
     *        fixed-tuple) attributes, whose per-element footprint comes from the
     *        existing column type, or from `desc.dtype` when the column is being
     *        created.
     * @param element_count Number of entries in `element_sizes`. When
     *        `element_sizes` is non-NULL it must equal the number of prims in
     *        `handle`'s prim set; pass 0 when `element_sizes` is NULL.
     * @param out_map_handle Receives the reserved map handle.
     * @return Enqueue result with status + op_index.
     */
    ovstage_enqueue_result_t (*map_attribute)(
        ovstage_data_plane_context_t* context,
        ovstage_query_handle_t        handle,
        const ovstage_map_desc_t*     desc,
        ovstage_ordinal_t             ordinal,
        const size_t*                 element_sizes,
        size_t                        element_count,
        ovstage_map_handle_t*         out_map_handle);

    /**
     * @brief Fetch the next writable map group, with timeout.
     * `out_group` and its pointers borrow map-owned storage.
     * Do not access the group's buffers after submitting its commit or
     * finalizing the session.
     */
    ovstage_api_status_t (*fetch_map_next)(
        ovstage_data_plane_context_t* context,
        ovstage_map_handle_t          map_handle,
        ovstage_timeout_ns_t          timeout,
        ovstage_map_group_t*          out_group);

    /**
     * @brief Enqueue commit of a single map group (streaming commit).
     *
     * Executes in the session's ordinal bucket; ordered relative to other
     * writes/deletes/map commits at the session's `ordinal`.
     *
     * A successful operation commits this group's values, any prim or attribute
     * creation, schema or hierarchy changes, and ordinal change records once.
     * A failed operation does not record these as completed changes and does not
     * undo groups committed earlier.
     *
     * Map commits are not transactional. A failed commit is not undone: the
     * implementation may have changed some or all of the group's stored rows.
     * Those changes can remain observable despite the failed operation;
     * no rollback or atomicity is guaranteed.
     *
     * If an accepted group commit fails during execution, the session becomes
     * terminal. Its groups cannot be retried. Call unmap_attribute to release
     * the session; it reports the retained failure without committing more
     * data. Recovery requires a new map session or another write operation.
     * The exception is a placed session whose availability event cannot be
     * recorded: that failure is reported after the commit, whose changes stay
     * recorded, and the session stays open (see ovstage_placement_t in
     * experimental/ovstage_placement_extension.h).
     */
    ovstage_enqueue_result_t (*unmap_group)(
        ovstage_data_plane_context_t* context,
        ovstage_map_handle_t          map_handle,
        const ovstage_map_group_t*    group,
        ovstage_cuda_sync_t           write_done_sync);

    /**
     * @brief Enqueue commit of all remaining groups and release of the
     *        map handle. After the enqueued op executes, the map handle is no
     *        longer valid.
     *
     * Successful finalization commits only groups not committed by earlier
     * unmap_group calls. If all groups were committed, finalization only
     * releases the session; it neither reapplies changes nor repeats commit
     * admission checks. If an earlier accepted group commit failed, it releases
     * the session and reports that failure without committing remaining data.
     *
     * An accepted finalization consumes the handle even if it fails during
     * execution, including when only its availability event cannot be
     * recorded. The failure and non-transactional rules of unmap_group also
     * apply to remaining groups committed here.
     */
    ovstage_enqueue_result_t (*unmap_attribute)(
        ovstage_data_plane_context_t* context,
        ovstage_map_handle_t          map_handle,
        ovstage_cuda_sync_t           write_done_sync);

    /* ───────────────────────────────────────────────────────────────────────
     * Delete
     *
     * Snapshot reads at or after a deletion omit the deleted attributes
     * unless later writes recreate them. Change-range reads can report
     * deletion tombstones.
     *
     * Reads before a deletion follow the ovstage_ordinal_range_t availability
     * contract: unavailable earlier payloads can cause
     * OVSTAGE_ERROR_OUT_OF_RANGE. Below the retention frontier, a successful
     * read does not guarantee the earlier state and may omit deleted rows.
     *
     * Pass an empty attribute list to delete entire prims (all attributes).
     * Deletes are ordinal-keyed in the same way as writes.
     *
     * The reserved `usd-prim-type` and `usd-schemas` are deletable like any other
     * attribute, whichever populator authored the prim: the prim loses its type,
     * or its whole applied-schema set, and the change feed reports the tombstone.
     * Deleting `usd-schemas` clears the set entirely. On a prototype root that
     * also removes what marks it as one, so the instancing queries no longer
     * report it; the prim carries no `usd-schemas` until population authors it
     * again.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a delete (tombstone) of attributes on prims at a given
     *        ordinal.
     *
     * @param context The context of the data plane.
     * @param handle Query handle identifying target prims.
     * @param attributes List of attributes to delete. Empty list = delete
     *        entire prims (all attributes). A name that contains `*` fails
     *        with OVSTAGE_ERROR_INVALID_ARGUMENT (reserved for read selection).
     * @param attribute_count Number of attributes. 0 = delete entire prims.
     * @param ordinal Ordinal of the deletion. Must exceed applicable floors and
     *        satisfy the configured ordinal admission policy for deleted cells
     *        and, for whole-prim deletion, the affected prim lifecycles.
     * @return Enqueue result with status + op_index.
     */
    ovstage_enqueue_result_t (*delete_attributes)(
        ovstage_data_plane_context_t* context,
        ovstage_query_handle_t        handle,
        const ovx_token_or_string_t*  attributes,
        size_t                        attribute_count,
        ovstage_ordinal_t             ordinal);

    /* ───────────────────────────────────────────────────────────────────────
     * Diagnostics
     *
     * Instance-bound so that error/version state is unambiguously sourced
     * from the implementation the caller is holding, even when multiple
     * ovstage_api implementations coexist in one process.
     * ─────────────────────────────────────────────────────────────────────── */

    /** @brief Get the implementation's version (SemVer 2.0).
     *
     * Returns the same values as the compile-time `OVSTAGE_VERSION_MAJOR` /
     * `OVSTAGE_VERSION_MINOR` / `OVSTAGE_VERSION_PATCH` macros, so a runtime
     * query and a compile-time `#if` check always agree. */
    void (*get_version)(
        ovstage_data_plane_context_t* context,
        uint32_t*                     out_major,
        uint32_t*                     out_minor,
        uint32_t*                     out_patch);

    /**
     * @brief Get a human-readable string for an error code.
     * @return Static string. Never NULL.
     */
    const char* (*get_error_string)(
        ovstage_data_plane_context_t* context,
        ovstage_api_status_t          error);

    /**
     * @brief Get the human-readable error string for a failed op id reported
     *        by the last wait_op call on this thread. Pass
     *        `OVSTAGE_INVALID_OP_ID` for the current thread-local diagnostic,
     *        which may be from another data-plane API call.
     *
     * The returned string is valid until the next wait_op call on the same
     * thread; the thread-local diagnostic is valid until another API call
     * replaces or clears it. Returns `{NULL, 0}` if the requested error is unavailable.
     */
    ovx_string_t (*get_last_op_error)(
        ovstage_data_plane_context_t* context,
        ovstage_op_id_t               op_id);

    /* ───────────────────────────────────────────────────────────────────────
     * Resources
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Get the path dictionary obtained from this ovstage instance.
     *
     * Returns a `path_dictionary_instance_t*` owned by the ovstage
     * subsystem. See <ovx/path_dictionary/path_dictionary.h> for the full
     * vtable contract, handle-lifetime regimes, refcount rules and thread
     * safety; the points below are ovstage-specific.
     *
     * Lifetime
     * --------
     * Each ovstage instance owns its dictionary. The returned pointer is
     * borrowed and becomes invalid when that instance is destroyed (after
     * its final owning handle is released). Consumers MUST NOT destroy the
     * dictionary themselves. Unrelated live instances do not extend its life.
     *
     * Handle invalidation
     * -------------------
     * Path lists belong to this dictionary and cannot be used with a different
     * instance. List references keep list storage alive only within the owning
     * instance's lifetime. Release them before destroying that instance; never
     * call through its dictionary pointer afterward. Recreate lists in a new
     * instance's dictionary when transferring selections across instances.
     * Tokens and prim paths require no per-handle release; their guaranteed
     * lifetime is the owning dictionary's lifetime.
     *
     * Path-list borrow rule
     * ---------------------
     * Any `ovx_primpath_list_t` returned in a result struct
     * (`ovstage_read_group_t::prims.list`, `ovstage_map_group_t::prims.list`,
     * `ovstage_query_result_t::prims.list`) is a BORROW: the producing op
     * holds a reference and will release it on teardown of the producing
     * handle (`release_groups` / `release_read` / `release_query` etc.). To
     * keep such a list usable beyond the producing handle, call
     * `path_dictionary_add_path_list_reference(ovstage_get_path_dictionary(instance), list)`
     * BEFORE releasing the producer, and pair that add with a matching
     * `path_dictionary_release_path_list_reference` when finished. Tokens
     * and prim paths in result structs need no per-handle pinning — they
     * are dict-lifetime.
     *
     * Create-failure / null-instance contract:
     *   - If `ovstage_create_instance` returns a non-OK status, no instance
     *     was constructed and the caller MUST NOT invoke this slot. Treat
     *     the dictionary as unavailable in that path; there is nothing to
     *     query.
     *   - When called on a live instance, the slot returns the dictionary of
     *     the instance, or NULL when the implementation has no dictionary.
     *     Check for NULL before dereferencing the result.
     *
     * @return The instance's path dictionary, or NULL if no dictionary is
     *         available for this instance.
     */
    path_dictionary_instance_t* (*get_path_dictionary)(
        ovstage_data_plane_context_t* context);

    /* -----------------------------------------------------------------------
     * Batched write
     * ----------------------------------------------------------------------- */

    /**
     * @brief Enqueue one operation that writes multiple attributes to the
     *        prims identified by a query handle.
     *
     * The write array undergoes common preflight before the operation is queued.
     * Errors established there, including invalid descriptors and known write-floor
     * violations, are returned synchronously. Successful enqueue can still fail
     * ordinal admission during execution. Admission is checked for the complete
     * batch before any entry changes stage data; rejection is reported through
     * wait_op and get_last_op_error.
     * Ordinary value columns and the reserved usd-prim-type/usd-schemas
     * entries participate in the same operation, including any prim or attribute
     * creation.
     * Attribute kind is declared by each entry's `data.is_array` with
     * exactly the same rules as write_attribute.
     *
     * One op_index groups completion and pending-write coverage; it does not make
     * the entries an atomic multi-attribute transaction or establish a global
     * snapshot barrier. Entries may be applied incrementally, descriptor order
     * does not define execution order, and operations that do not overlap the
     * batch may interleave before completion. Pending coverage rejects
     * read_attributes calls overlapping any batch entry until the operation
     * finishes.
     *
     * Non-reserved entries that resolve to the same canonical storage column
     * may appear together only when their selected prim coverage is disjoint;
     * repeating a concrete (prim, canonical-column) target is invalid. Such
     * entries must also declare compatible column identities: the same dtype,
     * row layout, array kind, and semantic.
     *
     * Every DLManagedTensorVersioned pointer across the complete write
     * array must be non-NULL and unique. Ownership of all supplied managed
     * tensors transfers at call entry; every unique pointer is released exactly
     * once, including when synchronous validation rejects the batch.
     *
     * If the query has no target prims, batch-level names, aliases, ordinal
     * admission, and managed-pointer uniqueness are still validated, but tensor
     * payload shape/device/type validation is skipped because no payload is
     * accessed. Transferred managed tensors are still released exactly once.
     * The placed form's check that an availability event on a default stream
     * does not span CUDA sources on two devices still applies (see
     * ovstage_placement_t in experimental/ovstage_placement_extension.h).
     *
     * A successful enqueue does not promise transactional rollback. If execution
     * returns a failure after storage mutation begins, wait_op still reports
     * failure and get_last_op_error supplies its diagnostic. Entries whose
     * application completed successfully before that failure remain committed,
     * including any new prims or attributes and their change records at the
     * submitted ordinal. The failed entry is not recorded as a completed write,
     * although changes made before the failure can remain visible to an
     * attribute-presence (HAS) query. Preparing storage alone does not count as a
     * completed write or record changes at the submitted ordinal. Reserved
     * metadata may complete before ordinary attribute values; it remains
     * committed if a later entry fails.
     *
     * The operation still has one failure result and does not expose a per-entry
     * completion mask. Repair a whole batch that may have completed an entry with a
     * newer ordinal: the STRICTLY_INCREASING policy rejects rewriting
     * completed entries at their recorded ordinal. If no entry completed,
     * including when the sole entry of a one-entry call did not complete, the
     * ordinal remains available. A corrected
     * same-ordinal retry remains admissible subject to current stage state,
     * except for a prim whose previous schema a forcing entry ended before the
     * failure: that end stays recorded, so its retry needs a later ordinal.
     * Allocation or size failures follow normal error reporting only while the
     * runtime can keep the stage and its ordinal change records consistent with
     * every previously completed entry.
     *
     * @remark Current implementation: a failed write can leave a prim visible to
     *         queries without recording its creation at the submitted ordinal.
     *         INSERT treats that prim as an existing target and rejects it with
     *         OVSTAGE_ERROR_PRIM_NOT_FOUND. A later successful UPSERT commits its
     *         attribute changes and their ordinal records but does not record a
     *         prim-creation event for that already-present prim.
     *
     * @remark Current implementation: after an entry is applied successfully,
     *         failure to record its prim, attribute and ordinal changes terminates
     *         the process rather than return an inconsistent instance.
     *         This condition is not surfaced through wait_op.
     *
     * @param context The context of the data plane.
     * @param handle Query handle identifying target prims.
     * @param writes Array of named attribute writes. An entry with a name that
     *        contains `*` fails the whole operation with
     *        OVSTAGE_ERROR_INVALID_ARGUMENT (reserved for read selection).
     * @param write_count Number of entries in writes.
     * @param ordinal Ordinal shared by the operation.
     * @param prim_mode UPSERT (default) or INSERT.
     * @return Enqueue result with status and one op_index for the complete group.
     */
    ovstage_enqueue_result_t (*write_attributes)(
        ovstage_data_plane_context_t*      context,
        ovstage_query_handle_t             handle,
        const ovstage_attribute_write_t*   writes,
        size_t                             write_count,
        ovstage_ordinal_t                  ordinal,
        ovstage_prim_mode_t                prim_mode);

    /* -----------------------------------------------------------------------
     * Extensions
     * ----------------------------------------------------------------------- */

    /**
     * @brief Query an optional implementation-specific extension table.
     *
     * Extension names are UTF-8, NUL-terminated identifiers. On success,
     * `*out_extension` receives a pointer to an immutable function table owned
     * by the implementation and valid for the lifetime of the process. The
     * table layout is defined by the extension owner.
     *
     * Returns OVSTAGE_OK when the extension exists,
     * OVSTAGE_ERROR_NOT_FOUND when it does not, and
     * OVSTAGE_ERROR_INVALID_ARGUMENT for NULL arguments.
     */
    ovstage_api_status_t (*query_extension)(
        ovstage_data_plane_context_t* context,
        const char*                   name,
        const void**                  out_extension);

    /* -----------------------------------------------------------------------
     * Subtree clone
     * ----------------------------------------------------------------------- */

    /**
     * @brief Enqueue a clone of the subtree under `source_path` to every path
     *        in `target_paths`, as an ordinal-keyed write at `ordinal`.
     *
     * The wrapper ovstage_clone in ovstage.h documents the contract of a
     * clone. The operation is awaited through wait_op and released through
     * release_op like every other operation of the data plane.
     */
    ovstage_enqueue_result_t (*clone)(
        ovstage_data_plane_context_t* context,
        ovx_string_t                  source_path,
        const ovx_string_t*           target_paths,
        size_t                        num_target_paths,
        ovstage_ordinal_t             ordinal);

    /**
     * @brief Fetch the completion report of a clone operation.
     *
     * The wrapper ovstage_fetch_clone_result in ovstage.h documents the
     * contract. The report is per-op completion state, available from the
     * completion of the clone until release_op.
     */
    ovstage_api_status_t (*fetch_clone_result)(
        ovstage_data_plane_context_t* context,
        ovstage_op_id_t               op_id,
        ovstage_timeout_ns_t          timeout,
        ovstage_clone_result_t*       out_result);
};

/* ═══════════════════════════════════════════════════════════════════════════════
 * Extensions
 *
 * Optional function tables reached through query_extension. An extension is
 * discoverable: query_extension returns OVSTAGE_ERROR_NOT_FOUND for a name the
 * implementation does not provide, so a caller can tell an older or different
 * implementation apart from one that has the feature, instead of calling
 * through a function it does not have.
 *
 * Each public extension is declared in its own header, which callers include
 * explicitly. Experimental ones live under experimental/: the device-placement
 * extension is in experimental/ovstage_placement_extension.h. Only the header
 * every extension table begins with is defined here.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Header that begins every ovstage extension table.
 *
 * `struct_size` is the size in bytes of the table the implementation provides,
 * and `version` its revision. Check both before using a field: a table smaller
 * than the struct a caller was compiled against does not have that struct's
 * trailing fields, and they must not be read. Size alone does not establish
 * compatibility; each extension's header states which versions it accepts.
 */
typedef struct ovstage_extension_header_t
{
    uint32_t struct_size;
    uint32_t version;
} ovstage_extension_header_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Diagnostics — free function
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Get the error string for the latest synchronous API call on the
 *        calling thread.
 *
 * Free function (no instance), because the error state is thread-local rather
 * than per-instance: each thread observes its own most-recent synchronous
 * error, regardless of which `ovstage_instance_t` produced it. If two instances
 * are driven from the same thread, the most recent failure wins. Crucially,
 * this can be called when `ovstage_create_instance` itself failed and no
 * instance exists. Valid until the next API call on the same thread. Returns
 * `{NULL, 0}` when no error has been recorded.
 *
 * Mirrors `ovstage_population_get_last_error(void)`.
 */
ovx_string_t ovstage_get_last_error(void);

#ifdef __cplusplus
}
#endif

#include "ovstage_api_utils.h"

#endif /* OVSTAGE_API_H */
