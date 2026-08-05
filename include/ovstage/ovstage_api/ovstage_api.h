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
 * - **Reads and queries are independent.** Reads (`read_attributes`) of
 *   recorded columns are validated against the effective write floor and never
 *   participate in write ordering. The floor is the ordinal through which data
 *   is sealed, and it starts at 0: a write is only readable once the caller
 *   advances the floor to cover its ordinal. An explicit ordinal range first
 *   selects the keys that changed in it. A selected change above the floor
 *   fails with OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION. If a selected public key
 *   also has a retained change after the range end, the latest-only
 *   implementation fails with OVSTAGE_ERROR_OUT_OF_RANGE because the payload
 *   for the fixed range is no longer available. Queries (`query`) resolve
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
 *   slots are released synchronously (`release_group`,
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
 * All slots are thread-safe when called on the same instance from multiple
 * threads. GPU synchronization between producer and consumer is the caller's
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
 * | `usd-path`       | path-id        | Prim path (e.g. "/World/Env")  |
 * | `usd-schemas`    | token-id array | Applied schemas list           |
 * | `usd-prim-type`  | token-id       | Prim type name                 |
 * | `usd-parent`     | path-id        | Parent prim path               |
 * | `usd-active`     | bool           | Prim active state (NOT supported — see below) |
 * | `usd-children`   | path-id array  | Child prim paths               |
 *
 * `usd-prim-type` and `usd-schemas` are recorded columns and writable through
 * the ordinary write contract. `usd-path`, `usd-parent`, and `usd-children`
 * are derived on demand from the stage's own state: a latest read synthesizes
 * point-in-time group(s) at `range.end_ordinal` (`usd-children` batches its
 * ragged rows like other array reads, so large queries can span several
 * groups — iterate `fetch_read_next` to `END_OF_ITERATION` as with any
 * read), and writing, deleting, or mapping any of the four derived names
 * (including `usd-active`) returns `OVSTAGE_ERROR_NOT_SUPPORTED`. Like query
 * membership itself, the synthesized values reflect the latest committed
 * structural state — they carry no ordinal-keyed payload history and are not
 * bounded by the write floor the way recorded-column reads are. They also
 * keep no per-write change stream, so a range ("since"/"between") read
 * reports them as never changed (zero groups) — poll current values with
 * latest reads instead. Prims without a value (a root prim's `usd-parent`,
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
 * @version 0.1.1
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
 * Single combined vtable for the ovstage_api contract. Every slot
 * takes `ovstage_context_t* instance` as the first argument. Return type
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
 *                                release_group, release_read
 * - Write copy-in          (2) : write_attribute, write_attributes
 * - Map/unmap zero-copy    (4) : map_attribute, fetch_map_next, unmap_group,
 *                                unmap_attribute
 * - Structural             (1) : delete_attributes
 * - Diagnostics            (3) : get_version, get_error_string,
 *                                get_last_op_error
 * - Resources              (1) : get_path_dictionary
 * - Extensions             (1) : query_extension
 * ═══════════════════════════════════════════════════════════════════════════════ */

struct ovstage_vtable_t
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
        ovstage_context_t*         instance,
        ovstage_op_id_t            op_id,
        ovstage_timeout_ns_t       timeout,
        ovstage_op_wait_result_t*  out_wait_result);

    /**
     * @brief Release tracking state associated with an op id (synchronous).
     *
     * Safe to call once the op is known to have completed. After this call
     * the op id may not be used again with wait_op or get_last_op_error.
     */
    ovstage_api_status_t (*release_op)(
        ovstage_context_t* instance,
        ovstage_op_id_t    op_id);

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
     * @param instance Implementation context.
     * @param desc Write-floor advance descriptor.
     * @return Enqueue result with status + op_index.
     */
    ovstage_enqueue_result_t (*advance_write_floor)(
        ovstage_context_t*                 instance,
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
     * change-membership frontier. Payload reads remain latest-snapshot and
     * return the latest retained payload or tombstone.
     */
    ovstage_enqueue_result_t (*get_oldest_preserved_ordinal)(
        ovstage_context_t*              instance,
        ovstage_ordinal_query_handle_t* out_handle);

    /**
     * @brief Enqueue a query for the write floor of a specific attribute.
     *
     * Pass an empty attribute (`{.token = 0, .string = {NULL, 0}}`) to query
     * the global write floor (minimum across all known attributes).
     */
    ovstage_enqueue_result_t (*get_attribute_write_floor)(
        ovstage_context_t*              instance,
        ovx_string_or_token_t           attribute,
        ovstage_ordinal_query_handle_t* out_handle);

    /**
     * @brief Fetch the scalar result of any enqueued ordinal query.
     *
     * @return OVSTAGE_OK on success,
     *         OVSTAGE_ERROR_TIMEOUT if not ready within timeout,
     *         OVSTAGE_ERROR_OP_FAILED if the underlying enqueue failed.
     */
    ovstage_api_status_t (*fetch_ordinal)(
        ovstage_context_t*             instance,
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
        ovstage_context_t*             instance,
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
     * fetch_query_result call.
     */
    ovstage_enqueue_result_t (*query)(
        ovstage_context_t*       instance,
        const ovstage_filter_t*  filter,
        const ovx_token_t*           attrs,
        size_t                       attr_count,
        ovstage_query_handle_t*  out_query_handle);

    /**
     * @brief Create a query handle from an explicit prim path list. Synchronous.
     *
     * The returned handle wraps a caller-owned, interned prim path list and
     * does not require evaluation against the stage. It may be used
     * immediately as input to any enqueue.
     */
    ovstage_api_status_t (*query_from_path_list)(
        ovstage_context_t*       instance,
        ovx_primpath_list_t          path_list,
        ovstage_query_handle_t*  out_handle);

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
        ovstage_context_t*       instance,
        ovstage_query_handle_t   query_handle,
        ovstage_timeout_ns_t     timeout,
        ovstage_query_result_t*  out_result);

    /**
     * @brief Release a query result payload (synchronous).
     *
     * After this call, the `attributes` array in `*result` is invalid. The
     * query handle itself remains valid and must be released separately via
     * release_query.
     */
    ovstage_api_status_t (*release_query_result)(
        ovstage_context_t*            instance,
        const ovstage_query_result_t* result);

    /**
     * @brief Enqueue release of a query handle.
     *
     * Per-handle ordered: the release waits for any in-flight read/write/
     * map/delete enqueued against the same handle to complete before
     * reclaiming resources.
     */
    ovstage_enqueue_result_t (*release_query)(
        ovstage_context_t*       instance,
        ovstage_query_handle_t   handle);

    /* ───────────────────────────────────────────────────────────────────────
     * Read
     *
     * Multi-attribute read: enqueue one read for multiple attributes, then
     * iterate groups via fetch_read_next. Reads target sealed data and never
     * participate in write ordering; reads also never order with each other.
     *
     * Validation is scoped to each selected (attribute, path), so an unrelated
     * path never vetoes a read. A selected change above the write floor fails
     * with OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION; a selected key with a retained
     * change after the range end fails with OVSTAGE_ERROR_OUT_OF_RANGE, because
     * this implementation retains only the latest payload. A range that selects
     * nothing remains a successful zero-group read.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a multi-attribute read.
     */
    ovstage_enqueue_result_t (*read_attributes)(
        ovstage_context_t*       instance,
        ovstage_query_handle_t   handle,
        const ovx_token_t*           attrs,
        size_t                       attr_count,
        ovstage_ordinal_range_t  range,
        ovstage_read_handle_t*   out_read_handle);

    /**
     * @brief Fetch the next read result group, with timeout.
     *
     * @return OVSTAGE_OK on success,
     *         OVSTAGE_ERROR_END_OF_ITERATION when no more groups,
     *         OVSTAGE_ERROR_TIMEOUT if the next group is not ready
     *         within timeout,
     *         OVSTAGE_ERROR_OP_FAILED if the underlying enqueue failed. When a
     *         deferred read is rejected, the producer's own status (for example
     *         OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION or
     *         OVSTAGE_ERROR_OUT_OF_RANGE) is reported once that producer has
     *         completed; a fetch that is itself waiting on the producer reports
     *         OVSTAGE_ERROR_OP_FAILED instead. Ready queries are validated at
     *         enqueue and report the typed status from read_attributes.
     *
     * @post On OVSTAGE_OK, the group is independently valid until
     *       release_group.
     */
    ovstage_api_status_t (*fetch_read_next)(
        ovstage_context_t*       instance,
        ovstage_read_handle_t    read_handle,
        ovstage_timeout_ns_t     timeout,
        ovstage_read_group_t*    out_group);

    /**
     * @brief Release a read group's pinned storage (synchronous).
     *
     * Thread-safe; groups can be released in any order from any thread.
     */
    ovstage_api_status_t (*release_group)(
        ovstage_context_t*           instance,
        const ovstage_read_group_t*  group);

    /**
     * @brief Enqueue release of a read handle.
     *
     * Per-handle ordered: the release waits for any in-flight fetch_read_next
     * call on the same handle to complete before reclaiming resources.
     */
    ovstage_enqueue_result_t (*release_read)(
        ovstage_context_t*       instance,
        ovstage_read_handle_t    read_handle);

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
     * first, then write it again with the new schema.
     *
     * Fixed-size values use a lane-canonical schema: each transported data row
     * holds one attribute tuple, with the complete tuple width in
     * `DLDataType.lanes`. Compact convenience inputs may put tuple components
     * in trailing tensor dimensions, but those dimensions are folded into
     * lanes and are not preserved. Read and map results always expose the
     * canonical 1-D data-row tensor. Logical prims select those rows directly
     * or through `data.index_map`; see ovstage_write_data_t and ovstage_data_t.
     * Without an index map, the leading dimension must equal the logical
     * element count; a flat component buffer is not inferred as wider rows.
     *
     * Reserved metadata follows the same attribute-write and row-coverage contract
     * as every other attribute. `usd-prim-type` requires `is_array == false`;
     * `usd-schemas` requires `is_array == true`. A mismatch is rejected, and
     * neither name enables implicit broadcasting. Use an explicit index_map
     * when multiple target rows intentionally share source data. Metadata rows
     * are assignments: later values replace earlier values, and an empty
     * `usd-schemas` row clears the applied-schema set.
     *
     * For zero-copy writes (GPU kernels writing directly), use Map/Unmap.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a write of attribute values for prims identified by a
     *        query handle.
     *
     * This is implemented as a one-entry write_attributes call.
     * Write-entry, tensor-layout, and attribute-schema
     * errors that can be proven during common preflight are therefore returned
     * synchronously with an invalid op_index; failures that require pending-query
     * resolution or execution remain observable through wait_op.
     * When multiple inputs are independently invalid, callers must not rely on
     * error precedence; common preflight currently validates prim_mode before
     * query-handle readiness and ordinal admission.
     *
     * @param instance Implementation context.
     * @param handle Query handle identifying target prims.
     * @param attribute Attribute to write.
     * @param ordinal Ordinal for this write (must be > write floor at
     *        execution time). Writes sharing this ordinal execute in
     *        submission order; writes at other ordinals are independent.
     * @param data Write payload (tensors + cuda_sync + mask/index_map).
     * @param prim_mode UPSERT (default) or INSERT.
     * @return Enqueue result with status + op_index. A synchronous preflight
     *         failure has an invalid op_index. After a successful enqueue,
     *         failures discovered only during pending-query resolution,
     *         state-race revalidation, or backend execution are surfaced
     *         through wait_op + get_last_op_error.
     */
    ovstage_enqueue_result_t (*write_attribute)(
        ovstage_context_t*           instance,
        ovstage_query_handle_t       handle,
        ovx_string_or_token_t            attribute,
        ovstage_ordinal_t            ordinal,
        ovstage_write_data_t         data,
        ovstage_prim_mode_t          prim_mode);

    /* ───────────────────────────────────────────────────────────────────────
     * Map / Unmap (Zero-Copy Write)
     *
     * Iterator pattern for direct-to-storage writes (GPU kernels, DMA).
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
     * Like write_attribute, map_attribute can *create* the target attribute
     * when it does not already exist. Because the map path hands the caller
     * writable storage before any tensor is produced, new attributes require a
     * descriptor dtype. If `desc.dtype` is zero-initialized, map_attribute uses
     * the existing attribute schema only when it is unambiguous for the target
     * prims. If the requested dtype or semantic would change an existing
     * prim/name, map_attribute fails; delete that attribute first, then map it
     * again with the new schema.
     *
     * For array attributes (variable-size per prim), the caller additionally
     * provides element_sizes at map-enqueue time so the implementation can
     * pre-allocate the ragged storage; see the parameter docs below.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a zero-copy map session for writing an attribute.
     *
     * The returned map handle is reserved synchronously and may immediately
     * be used with subsequent fetch_map_next / unmap_* calls.
     *
     * @param instance Implementation context.
     * @param handle Query handle identifying target prims.
     * @param desc Map descriptor (attribute, dtype, semantic, prim_mode). See
     *        ovstage_map_desc_t.
     * @param ordinal Ordinal for this map session (must be > write floor at
     *        execution time). Map commits sharing this ordinal execute in
     *        submission order; those at other ordinals are independent.
     * @param element_sizes For array (variable-size) attributes, the per-prim
     *        element count: `element_sizes[i]` is the number of array elements
     *        to allocate for the i-th prim in `handle`'s prim set. This lets
     *        the implementation pre-allocate the ragged backing storage before
     *        handing back writable groups. Pass NULL for fixed-size (scalar /
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
        ovstage_context_t*           instance,
        ovstage_query_handle_t       handle,
        const ovstage_map_desc_t*    desc,
        ovstage_ordinal_t            ordinal,
        const size_t*                    element_sizes,
        size_t                           element_count,
        ovstage_map_handle_t*        out_map_handle);

    /**
     * @brief Fetch the next writable map group, with timeout.
     */
    ovstage_api_status_t (*fetch_map_next)(
        ovstage_context_t*       instance,
        ovstage_map_handle_t     map_handle,
        ovstage_timeout_ns_t     timeout,
        ovstage_map_group_t*     out_group);

    /**
     * @brief Enqueue commit of a single map group (streaming commit).
     *
     * Executes in the session's ordinal bucket; ordered relative to other
     * writes/deletes/map commits at the session's `ordinal`.
     */
    ovstage_enqueue_result_t (*unmap_group)(
        ovstage_context_t*           instance,
        ovstage_map_handle_t         map_handle,
        const ovstage_map_group_t*   group,
        ovstage_cuda_sync_t          write_done_sync);

    /**
     * @brief Enqueue commit of all remaining groups and release of the map
     *        handle. After the enqueued op executes, the map handle is no
     *        longer valid.
     */
    ovstage_enqueue_result_t (*unmap_attribute)(
        ovstage_context_t*       instance,
        ovstage_map_handle_t     map_handle,
        ovstage_cuda_sync_t      write_done_sync);

    /* ───────────────────────────────────────────────────────────────────────
     * Delete
     *
     * Tombstone semantics: reads at ordinals >= the tombstone ordinal see
     * nothing for deleted attributes. Reads at earlier ordinals return
     * historical data unchanged.
     *
     * Pass an empty attribute list to delete entire prims (all attributes).
     * Deletes are ordinal-keyed in the same way as writes.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a delete (tombstone) of attributes on prims at a given
     *        ordinal.
     *
     * @param instance Implementation context.
     * @param handle Query handle identifying target prims.
     * @param attributes List of attributes to delete. Empty list = delete
     *        entire prims (all attributes).
     * @param attribute_count Number of attributes. 0 = delete entire prims.
     * @param ordinal Ordinal of the deletion.
     * @return Enqueue result with status + op_index.
     */
    ovstage_enqueue_result_t (*delete_attributes)(
        ovstage_context_t*           instance,
        ovstage_query_handle_t       handle,
        const ovx_string_or_token_t*     attributes,
        size_t                           attribute_count,
        ovstage_ordinal_t            ordinal);

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
        ovstage_context_t* instance,
        uint32_t*              out_major,
        uint32_t*              out_minor,
        uint32_t*              out_patch);

    /**
     * @brief Get a human-readable string for an error code.
     * @return Static string. Never NULL.
     */
    const char* (*get_error_string)(
        ovstage_context_t* instance,
        ovstage_api_status_t    error);

    /**
     * @brief Get the human-readable error string for a failed op id reported
     *        by the last wait_op call on this thread.
     *
     * The returned string is valid until the next wait_op call on the same
     * thread. Returns `{NULL, 0}` if the op id is not known or did not fail.
     */
    ovx_string_t (*get_last_op_error)(
        ovstage_context_t* instance,
        ovstage_op_id_t    op_id);

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
     * The returned pointer is valid for the lifetime of the dictionary. In
     * the current implementation, all live ovstage instances
     * share a single refcounted dictionary: it is constructed when the
     * first instance is created and destroyed when the last is destroyed.
     * While at least one instance is alive, the pointer returned by any
     * instance's slot is valid. Consumers MUST NOT destroy or otherwise
     * tear down the dictionary themselves.
     *
     * Handle invalidation
     * -------------------
     * When the dictionary is destroyed, every handle minted through it
     * (tokens, prim paths, and any path lists still holding references) is
     * invalidated simultaneously. Consumers that need to keep handles alive
     * across a "create new ovstage instance + drop old one" boundary MUST
     * overlap the two instances' lifetimes — construct the replacement
     * instance before destroying the previous one.
     *
     * Path-list borrow rule
     * ---------------------
     * Any `ovx_primpath_list_t` returned in a result struct
     * (`ovstage_read_group_t::prims.list`, `ovstage_map_group_t::prims.list`,
     * `ovstage_query_result_t::prims.list`) is a BORROW: the producing op
     * holds a reference and will release it on teardown of the producing
     * handle (`release_group` / `release_read` / `release_query` etc.). To
     * keep such a list usable beyond the producing handle, call
     * `path_dictionary_add_path_list_reference(get_path_dictionary(instance), list)`
     * BEFORE releasing the producer, and pair that add with a matching
     * `path_dictionary_release_path_list_reference` when finished. Tokens
     * and prim paths in result structs need no per-handle pinning — they
     * are dict-lifetime.
     *
     * Future-proofing
     * ---------------
     * Consumers MUST NOT rely on the singleton sharing across instances:
     * future implementations may publish per-instance dictionaries with
     * strictly per-instance handle validity.
     *
     * Create-failure / null-instance contract:
     *   - If `ovstage_create_instance` returns a non-OK status, no instance
     *     was constructed and the caller MUST NOT invoke this slot. Treat
     *     the dictionary as unavailable in that path; there is nothing to
     *     query.
     *   - When called on a live instance, the slot returns a non-NULL
     *     pointer for the current implementation. A NULL return is reserved
     *     for future implementations that may fail to publish a dictionary;
     *     check for NULL before dereferencing the result.
     *
     * @return The instance's path dictionary, or NULL if no dictionary is
     *         available for this instance.
     */
    path_dictionary_instance_t* (*get_path_dictionary)(
        ovstage_context_t* instance);

    /* -----------------------------------------------------------------------
     * Batched write
     * ----------------------------------------------------------------------- */

    /**
     * @brief Enqueue one operation that writes multiple attributes to the
     *        prims identified by a query handle.
     *
     * The entire write array is validated before the operation is queued.
     * Ordinary value columns and the reserved usd-prim-type/usd-schemas
     * entries participate in the same operation and structural precreate.
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
     * Non-reserved entries must name distinct logical attributes and must
     * also map to distinct canonical storage columns. For example, an API alias
     * and its canonical storage name cannot appear together in one batch.
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
     *
     * A successful enqueue does not promise transactional rollback if a later
     * asynchronous scatter fails after earlier columns were authored. Such an
     * execution failure is reported by wait_op/get_last_op_error and does not
     * publish a successful group completion. Successful structural precreate
     * may leave physical/default column storage for entries whose scatter did
     * not complete. Only successfully authored entries publish value and
     * dirty-membership records.
     *
     * @param instance Implementation context.
     * @param handle Query handle identifying target prims.
     * @param writes Array of named attribute writes.
     * @param write_count Number of entries in writes.
     * @param ordinal Ordinal shared by the operation.
     * @param prim_mode UPSERT (default) or INSERT.
     * @return Enqueue result with status and one op_index for the complete group.
     */
    ovstage_enqueue_result_t (*write_attributes)(
        ovstage_context_t*                 instance,
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
        ovstage_context_t* instance,
        const char*        name,
        const void**       out_extension);

};

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
