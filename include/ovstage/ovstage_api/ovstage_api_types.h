/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage_api_types.h
 * @brief ovstage_api — shared types, handles, and instance bundle.
 *
 * @details
 * ovstage_api is a vtable-based contract for asynchronous, ordinal-keyed
 * zero-copy simulation data access. It is the abstract API that various
 * stage implementations can provide.
 *
 * This header defines the data types (handles, error codes, enums, payload
 * structs), the forward declarations for the vtable and context, and the
 * `ovstage_instance_t` bundle that pairs them.
 *
 * The vtable itself is defined in `ovstage_api.h`. Convenience
 * `static inline` wrappers around the core data-plane slots live in
 * `ovstage_api_utils.h`; higher-level extension APIs may live in sibling
 * headers.
 *
 * ## Lifecycle policy
 *
 * The vtable owns *resource* lifecycles (queries, reads, maps, ops, groups,
 * query results, ordinal queries) but **not** the instance's own lifecycle.
 * Each backend provides its own factory function and its own destructor —
 * those are intentionally not part of this contract.
 *
 * The execution-model documentation lives at the top of `ovstage_api.h`
 * alongside the vtable definition.
 *
 * @version 0.1.1
 * @date 2026-05-28
 */

#ifndef OVSTAGE_API_TYPES_H
#define OVSTAGE_API_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <dlpack/dlpack.h>              /* DLTensor, DLDataType, DLManagedTensorVersioned */
/* The path dictionary types header drags in the canonical <ovx/types.h> +
 * <ovx/string_types.h>, giving us ovx_token_t, ovx_primpath_t,
 * ovx_primpath_list_t, ovx_string_or_token_t, ovx_api_result_t, and the
 * opaque path_dictionary_instance_t bundle used by the get_path_dictionary
 * vtable slot. */
#include <ovx/path_dictionary/path_dictionary_types.h>

/* Compile-time API version (SemVer). The runtime get_version slot returns
 * exactly these values, so a compile-time `#if` check and a runtime query
 * always agree. */
#define OVSTAGE_VERSION_MAJOR 0
#define OVSTAGE_VERSION_MINOR 1
#define OVSTAGE_VERSION_PATCH 1

/* Marks a deprecated public entry point: OVSTAGE_DEPRECATED("Use <new> instead")
 * on the old declaration. Emits a compiler warning at the call site where
 * supported. A locally-prefixed stand-in for the not-yet-published shared
 * OV_DEPRECATED; it migrates to that (mechanical rename) once the shared ovx_
 * utility header lands. */
#ifndef OVSTAGE_DEPRECATED
#    if defined(__cplusplus) && __cplusplus >= 201402L
#        define OVSTAGE_DEPRECATED(msg) [[deprecated(msg)]]
#    elif defined(__GNUC__) || defined(__clang__)
#        define OVSTAGE_DEPRECATED(msg) __attribute__((deprecated(msg)))
#    elif defined(_MSC_VER)
#        define OVSTAGE_DEPRECATED(msg) __declspec(deprecated(msg))
#    else
#        define OVSTAGE_DEPRECATED(msg)
#    endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════════
 * Forward Declarations / Opaque Types
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Opaque per-implementation context, stored in ovstage_instance_t. */
typedef struct ovstage_context_t ovstage_context_t;

/** @brief Vtable defined in ovstage_api.h. */
typedef struct ovstage_vtable_t  ovstage_vtable_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Error Codes
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief API status / error code returned by ovstage calls (synchronous
 *        returns and the `status` field of enqueue results).
 *
 * A typed enum rather than a bare integer: the fine-grained codes are retained,
 * and a human-readable detail string for the most recent failure is available
 * via `ovstage_get_last_error()` (or `ovstage_get_last_op_error()` for a failed
 * op). The numeric values are stable ABI.
 */
typedef enum ovstage_api_status_t
{
    OVSTAGE_OK                          = 0,
    OVSTAGE_ERROR_INVALID_ARGUMENT      = 1,
    OVSTAGE_ERROR_INVALID_HANDLE        = 2,  /**< Stale/invalid handle of any kind. */
    OVSTAGE_ERROR_NOT_FOUND             = 3,
    OVSTAGE_ERROR_PRIM_NOT_FOUND        = 4,  /**< INSERT mode and prim already exists. */
    OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION = 5,  /**< Write at/below effective write floor; snapshot read with current state above it; range read with a selected change above it. */
    OVSTAGE_ERROR_NOT_SUPPORTED         = 6,
    OVSTAGE_ERROR_QUEUE_FULL            = 7,  /**< Backpressure: submit queue full. */
    OVSTAGE_ERROR_END_OF_ITERATION      = 8,  /**< No more groups to iterate. */
    OVSTAGE_ERROR_OUT_OF_MEMORY         = 9,
    OVSTAGE_ERROR_LAYOUT_CHANGED        = 10, /**< Layout changed during map. */
    OVSTAGE_ERROR_TIMEOUT               = 11, /**< Fetch/wait did not complete within timeout. */
    OVSTAGE_ERROR_OP_FAILED             = 12, /**< An enqueued op failed; see get_last_op_error. */
    OVSTAGE_ERROR_OUT_OF_RANGE          = 13, /**< Requested ordinal range cannot be materialized from retained payloads. */
    OVSTAGE_ERROR_INTERNAL              = 99
} ovstage_api_status_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Scalar Types
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Application/simulation ordinal used as the API's ordering key.
 *
 * The ordinal is not necessarily physical simulation time in nanoseconds. It is
 * the scalar key used for write ordering, sealing/write floors, point/range
 * change-membership reads, and retention frontiers. Applications that need
 * physical time should carry an explicit ordinal-to-time mapping outside this
 * scalar or in metadata.
 *
 * Valid range: any uint64_t value. There is no sentinel; see
 * ovstage_ordinal_range_t for how "latest" reads are expressed.
 */
#ifndef OVSTAGE_ORDINAL_T_DECLARED
#define OVSTAGE_ORDINAL_T_DECLARED
typedef uint64_t ovstage_ordinal_t;
#endif

/**
 * @brief Bitmask pointer. NULL = all valid. Bit i set = element i is valid.
 *
 * Element `i` is bit `i % 64` of word `i / 64`, least significant bit first.
 * A non-NULL mask spans at least `ceil(count / 64)` `uint64_t` words, where
 * `count` is the logical element count of the structure carrying it; exactly
 * that many words are read. Any bits above `count` in the final word are
 * ignored.
 */
typedef const uint64_t* ovstage_mask_t;

/**
 * @brief CUDA synchronization for a GPU data handoff.
 *
 * The two fields are independent knobs — set either, both, or neither. Each
 * non-zero field adds its own synchronization before the op:
 *  - `stream`     — `0` = none; `1` = the default stream; `>1` = a specific
 *                   `cudaStream_t`. When set, all work currently queued on that
 *                   stream is drained.
 *  - `wait_event` — `0` = none; otherwise a `cudaEvent_t` to wait on.
 *
 * So `{ 0, event }` waits on the event only, `{ stream, 0 }` drains the stream
 * only, `{ stream, event }` does both, and `{ 0, 0 }` is no synchronization
 * (CPU-resident or already-synced data).
 */
typedef struct {
    uintptr_t stream;     /**< CUDA stream to synchronize: 0 = none, 1 = default stream, >1 = a specific `cudaStream_t`. */
    uintptr_t wait_event; /**< CUDA event to wait on: 0 = none, else a `cudaEvent_t`. */
} ovstage_cuda_sync_t;

/**
 * @brief Timeout for fetch/wait calls, in nanoseconds.
 *
 * - 0                                 : non-blocking poll.
 * - OVSTAGE_TIMEOUT_INFINITE      : block until the result is ready.
 * - any other value                   : block for up to that many nanoseconds
 *                                       before returning OVSTAGE_ERROR_TIMEOUT.
 */
typedef uint64_t ovstage_timeout_ns_t;
#define OVSTAGE_TIMEOUT_INFINITE ((ovstage_timeout_ns_t)~0ULL)

/* ═══════════════════════════════════════════════════════════════════════════════
 * Op Id and Enqueue Result
 *
 * Every asynchronous enqueue call returns an ovstage_enqueue_result_t
 * carrying a status (whether the operation was accepted) and a monotonically
 * increasing ovstage_op_id_t identifying the enqueued op.
 *
 * The op id can be passed to the vtable's wait_op slot to wait for that
 * specific op (and any operations it transitively depends on under the
 * ordinal-keyed ordering rules) and to retrieve per-op error information.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Monotonic per-instance operation identifier. */
typedef uint64_t ovstage_op_id_t;
#define OVSTAGE_INVALID_OP_ID ((ovstage_op_id_t)0)

/**
 * @brief Result of an asynchronous enqueue call.
 *
 * - `status` is the enqueue status. OVSTAGE_OK means the operation was
 *   accepted; the operation itself has not necessarily executed. Use the
 *   wait_op slot or a corresponding fetch_* call to observe completion/results.
 * - `op_index` is OVSTAGE_INVALID_OP_ID when `status != OVSTAGE_OK`.
 */
typedef struct {
    ovstage_api_status_t status;
    ovstage_op_id_t op_index;
} ovstage_enqueue_result_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Handle Types
 *
 * Stage-data-producing enqueues reserve a typed handle that is valid as soon
 * as the enqueue returns. The handle may be passed to subsequent enqueues as
 * input before the underlying op has executed; consumers see the producer's
 * effect once the ordering rules (ordinal-keyed for writes/advances, per-handle
 * for releases) place them after it.
 *
 * Results are obtained via the corresponding fetch_* call(s) that take the
 * handle and a timeout.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Query handle — identifies a logical set of prims. */
typedef uint64_t ovstage_query_handle_t;
#define OVSTAGE_INVALID_QUERY_HANDLE ((ovstage_query_handle_t)0)

/** @brief Read handle — identifies an enqueued multi-attribute read. */
typedef uint64_t ovstage_read_handle_t;
#define OVSTAGE_INVALID_READ_HANDLE ((ovstage_read_handle_t)0)

/** @brief Map handle — identifies an enqueued zero-copy write session. */
typedef uint64_t ovstage_map_handle_t;
#define OVSTAGE_INVALID_MAP_HANDLE ((ovstage_map_handle_t)0)

/**
 * @brief Ordinal-query handle — identifies an enqueued read of an ordinal
 *        scalar, such as the global or per-attribute write floor.
 */
typedef uint64_t ovstage_ordinal_query_handle_t;
#define OVSTAGE_INVALID_ORDINAL_QUERY_HANDLE ((ovstage_ordinal_query_handle_t)0)

/** @brief Opaque identity for a fetched read group payload. */
typedef uint64_t ovstage_read_group_id_t;
#define OVSTAGE_INVALID_READ_GROUP_ID ((ovstage_read_group_id_t)0)

/** @brief Opaque identity for a fetched query result payload. */
typedef uint64_t ovstage_query_result_id_t;
#define OVSTAGE_INVALID_QUERY_RESULT_ID ((ovstage_query_result_id_t)0)

/* ═══════════════════════════════════════════════════════════════════════════════
 * Wait Result
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Output of the wait_op slot.
 *
 * - `error_op_ids` lists op ids observed to have failed since the previous
 *   wait_op call on this thread. For each id, call the get_last_op_error
 *   slot to retrieve the human-readable error string. The array is transient
 *   thread-local memory and is invalidated by the next wait_op call on the
 *   same thread.
 * - `lowest_pending_op_id` is meaningful only on OVSTAGE_ERROR_TIMEOUT:
 *   it is the lowest still-pending op id within the dependency chain of the
 *   waited op (not a global lowest across the instance). Useful for
 *   partial-progress reporting.
 */
typedef struct {
    const ovstage_op_id_t* error_op_ids;
    size_t                     error_op_id_count;
    ovstage_op_id_t        lowest_pending_op_id;
} ovstage_op_wait_result_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Enumerations
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Filter predicate operators for query matching.
 *
 * Notes:
 * - CONTAINS tests whether a composite value (array, string) contains at least
 *   one of the provided match values. For "contains all", use multiple CONTAINS
 *   predicates (one per required value).
 * - CONTAINS works on string attributes (substring match), array attributes
 *   (element membership), and any other composite data types.
 * - Comparison operators (LT, LE, GT, GE) work on numeric and string attributes.
 *   String comparison is binary (byte-wise).
 */
typedef enum {
    OVSTAGE_FILTER_OP_HAS      = 0,  /**< Attribute exists on prim (no value test). */
    OVSTAGE_FILTER_OP_IN       = 1,  /**< Attribute value ∈ {values} (single value = equals). */
    OVSTAGE_FILTER_OP_CONTAINS = 2,  /**< Composite attribute contains one of {values}. */
    OVSTAGE_FILTER_OP_PREFIX   = 3,  /**< String attribute starts with one of {values}. */
    OVSTAGE_FILTER_OP_LT       = 4,  /**< Attribute value < value. */
    OVSTAGE_FILTER_OP_LE       = 5,  /**< Attribute value <= value. */
    OVSTAGE_FILTER_OP_GT       = 6,  /**< Attribute value > value. */
    OVSTAGE_FILTER_OP_GE       = 7,  /**< Attribute value >= value. */
} ovstage_filter_op_t;

/**
 * @brief Write prim creation mode.
 *
 * - UPSERT (default): Create prims that don't exist; update prims that do.
 * - INSERT: Only create new prims; error if prim already exists.
 */
typedef enum {
    OVSTAGE_PRIM_MODE_UPSERT = 0,  /**< Create if absent, update if present (default). */
    OVSTAGE_PRIM_MODE_INSERT = 1,  /**< Create only; OVSTAGE_ERROR_PRIM_NOT_FOUND if exists. */
} ovstage_prim_mode_t;

/** @brief Write-floor advance scope. */
typedef enum {
    OVSTAGE_SCOPE_ALL     = 0,  /**< Advance all attributes (and the global write floor). */
    OVSTAGE_SCOPE_INCLUDE = 1,  /**< Advance only listed attributes. */
    OVSTAGE_SCOPE_EXCLUDE = 2,  /**< Advance all EXCEPT listed attributes. */
} ovstage_scope_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Data Structures — Tensor and Data Representation
 *
 * A single data type, ovstage_data_t, carries an array of DLTensors plus
 * sparsity (index_map / mask) and GPU sync (cuda_sync) metadata. Used by read
 * groups and map (write iterator) groups.
 *
 * Tensor transport layout:
 * - Stacked rows use tensor_count = 1; one tensor describes all transported
 *   data rows along its leading dimension.
 * - Per-row transport uses tensor_count = N; each tensor describes one
 *   transported data row's variable-length payload.
 * - A fixed-size value tensor returned by read or map is canonical: `ndim == 1`,
 *   `shape[0]` is the number of transported data rows, and `dtype.lanes`
 *   is the complete tuple width (for example, point3f uses 3 lanes and
 *   matrix4d uses 16). Write-side convenience trailing dimensions are not
 *   retained or reconstructed.
 *
 * Tensor count is not an attribute-kind signal. Read results report logical
 * kind in ovstage_read_group_t::is_array. Copy-in writes declare it in
 * ovstage_write_data_t::is_array.
 *
 * index_map usage (applies across the logical element axis):
 * - Sparse/gather access: logical element i selects data row index_map[i]
 *   (`tensors[0]` row index_map[i] for stacked fixed-size transport, or
 *   `tensors[index_map[i]]` for per-row array transport)
 * - Reordering: tensors stored in different order than prims
 * - Deduplication/broadcast: many prims map to few unique values
 *
 * mask and index_map are mutually exclusive (at most one non-NULL).
 *
 * For the write (copy-in) path, ovstage_write_data_t mirrors this shape
 * but additionally allows storage-managed tensor lifetime; see the Write
 * section of ovstage_api.h.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Data payload — array of tensors plus sparsity and GPU sync metadata.
 *
 * Used for both read results (caller reads from `tensors[i].data`) and the map
 * iterator (caller writes into `tensors[i].data`). The DLTensor metadata is
 * owned by the implementation and treated as read-only by the caller.
 *
 * @invariant For value groups, `tensor_count >= 1`. For fixed-size attributes
 *            it is 1; for array attributes it equals the number of logical prim
 *            elements.
 * @invariant A fixed-size value tensor has `ndim == 1`; its leading shape is
 *            the transported data-row count and `dtype.lanes` carries the
 *            tuple width. Logical element `i` selects row `i` when `index_map`
 *            is NULL, or row `index_map[i]` otherwise.
 * @invariant For delete/tombstone read groups (`is_delete == true`),
 *            `tensor_count == 0`, `tensors == NULL`, and `count == 0`.
 * @invariant `index_map` and `mask` are mutually exclusive.
 * @invariant `count` is set (non-zero) when index_map or mask is non-NULL.
 *            When both are NULL, logical elements select data rows by identity.
 * @invariant `cuda_sync` is `{0, 0}` for CPU-resident or already-synchronized data.
 */
typedef struct {
    const DLTensor*           tensors;       /**< Array of tensors (1 for fixed-size attrs; per-prim for array attrs). */
    uint32_t                  tensor_count;  /**< Number of tensors in `tensors`. */

    uint32_t                  count;       /**< Logical element count when index_map/mask present. */
    const uint32_t*           index_map;   /**< NULL = identity; non-NULL = gather/reorder/dedup. */
    ovstage_mask_t        mask;        /**< NULL = all valid; non-NULL = bitmask over elements. */
    ovstage_cuda_sync_t   cuda_sync;   /**< GPU sync applied before access; `{0,0}` = none. See ovstage_cuda_sync_t. */
} ovstage_data_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Shared Sub-Structures
 *
 * These are factored out and reused across read groups and map groups to ensure
 * consistent prim identification and attribute metadata.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Prim group — identifies which prims a group covers.
 *
 * Uses four mutually exclusive representations:
 * 1. Full match: list == query list, offset=0, count=total, index_map=NULL
 * 2. Contiguous sub-range: same list, offset/count subset, index_map=NULL
 * 3. Sparse subset: same list, index_map non-NULL (gather indices into list)
 * 4. Different set: different list handle (resolve via path dictionary)
 *
 * Handle comparison (list == other.list) is O(1) pointer equality.
 */
typedef struct {
    ovx_primpath_list_t  list;       /**< Comparable prim list handle (== for identity). */
    uint32_t             offset;     /**< Start index within list (0 if full match). */
    uint32_t             count;      /**< Number of prims in this group. */
    const uint32_t*      index_map;  /**< NULL = contiguous from offset; non-NULL = sparse indices. */
} ovstage_prim_group_t;

/**
 * @brief Attribute metadata — write floor and layout generation.
 *
 * Shared across read groups and map groups.
 *
 * - `attribute_write_floor_ordinal`: The write floor in effect for this
 *   attribute when the group was produced. Data at ordinals <=
 *   attribute_write_floor_ordinal is sealed (will never change).
 * - `layout_generation`: Monotonically increasing counter scoped to the
 *   attribute. Bumps on structural changes (prim add/remove, index_map shape
 *   change). Stable when only values update. Enables consumers to skip
 *   re-setup on hot path.
 */
typedef struct {
    ovstage_ordinal_t attribute_write_floor_ordinal; /**< Write floor for this attribute. */
    uint64_t          layout_generation;             /**< Structural version (opaque). */
} ovstage_attribute_meta_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Ordinal Range
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Ordinal range for read operations.
 *
 * - Latest snapshot request: has_start_ordinal = false, end_ordinal = N.
 *   In the current implementation, recorded columns return their latest
 *   committed payload, not a historical payload selected at or before N.
 * - Change range [start, end]: has_start_ordinal = true,
 *   start_ordinal <= end_ordinal. Selects keys with retained changes in the
 *   inclusive interval [start_ordinal, end_ordinal]. As a limitation of the
 *   current implementation, this interval selects keys only; it does not select
 *   a historical payload version. A selected change above the effective write
 *   floor fails with OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION. If a selected public
 *   key (attribute and path) also has a retained change after end_ordinal, the
 *   read fails with OVSTAGE_ERROR_OUT_OF_RANGE because the payload for the fixed
 *   range is no longer available, whether or not the later change is sealed.
 *   Otherwise, the selected key returns its latest committed payload or
 *   tombstone.
 *
 * Implementations may coalesce or discard history below their inclusive
 * retention frontier. Query ovstage_get_oldest_preserved_ordinal before relying
 * on an old interval.
 *
 * @remark The current implementation retains bounded exact
 * change membership at or above the frontier reported by
 * ovstage_get_oldest_preserved_ordinal. Callers must query the frontier rather
 * than assume a fixed retention depth. Ordinal ranges are therefore not a
 * historical-payload event log. Historical-payload support must first return a
 * retained in-range payload when one is available before reporting
 * OVSTAGE_ERROR_OUT_OF_RANGE.
 *
 * 0 is a valid ordinal (no sentinel values).
 */
typedef struct {
    ovstage_ordinal_t start_ordinal;     /**< Range start (only when has_start_ordinal). */
    ovstage_ordinal_t end_ordinal;       /**< Upper-bound ordinal. */
    bool              has_start_ordinal; /**< false = latest snapshot; true = range [start, end]. */
} ovstage_ordinal_range_t;

/** Per-attribute USD semantic — the authored *interpretation* of a column's
    *  bytes, orthogonal to its `DLDataType` storage shape. ovpopulation derives
    *  this from the USD `SdfValueTypeName` (role + scalar type) and carries it
    *  through writes; consumers (e.g. runtime→USD export) use it to recover the
    *  authored USD type (point3f vs float3, asset vs string, etc.). It is NOT
    *  part of the bucket key — `float3` and `point3f` share bytes and a column.
    *  Tuple width comes from `dtype.lanes`, precision from `dtype.bits`; these
    *  roles deliberately do not encode dimensional suffixes such as 3f or 4d.
    *
    *  The semantic round-trips through the attribute column: write records it on
    *  the column at creation, read recovers it by decoding the column.
    *
    *    - Geometric semantics (POINT, VECTOR, NORMAL, COLOR, QUATERNION,
    *      MATRIX, TEXTURE_COORDINATE) record a geometric role on the column.
    *      Storage stays in the requested numeric `dtype`.
    *
    *    - ID semantics select the corresponding ID storage type and
    *      require pre-interned id payloads (producers — e.g. ovpopulation —
    *      must intern via the path or token dictionary before
    *      writing; ovstage does not stringify or resolve):
    *          * TOKEN_ID             → `dtype = {kDLUInt, 64, 1}`
    *                                   (one 64-bit token id per row)
    *          * RELATIONSHIP_PATH_ID → `dtype = {kDLUInt, 64, 1}`
    *                                   (one 64-bit path id per row)
    *          * CONNECTION_PATH_ID   → `dtype = {kDLUInt, 64, 2}`
    *                                   (one `(path_id, token_id)` pair per row,
    *                                    using one 16-byte element per row)
    *
    *    - Byte-string semantics (ASSET_STRING, PATH_EXPRESSION_STRING) keep
    *      ragged `kDLUInt,8,1` byte-row storage with NUL-separated, authored
    *      sub-values.
    *
    *    - STRING carries a plain USD `string` value as raw UTF-8 bytes. The
    *      payload is a ragged `kDLUInt,8,1` byte array (one string per row,
    *      `is_array = true`) — NOT a pre-interned token id. It stamps the
    *      USD-string role on the column, so the resulting column is the
    *      canonical USD-string representation. Read recovers the semantic by
    *      decoding that role.
    *
    *  Either way, borrow and replicate modes converge on the same authored
    *  interpretation because they consult the same attribute column. */
typedef enum
{
    OVSTAGE_SEMANTIC_NONE                   = 0,
    OVSTAGE_SEMANTIC_ASSET_STRING           = 1,
    OVSTAGE_SEMANTIC_TOKEN_ID               = 2,
    OVSTAGE_SEMANTIC_PATH_EXPRESSION_STRING = 3,
    OVSTAGE_SEMANTIC_RELATIONSHIP_PATH_ID   = 4,
    OVSTAGE_SEMANTIC_POINT                  = 5,
    OVSTAGE_SEMANTIC_VECTOR                 = 6,
    OVSTAGE_SEMANTIC_NORMAL                 = 7,
    OVSTAGE_SEMANTIC_COLOR                  = 8,
    OVSTAGE_SEMANTIC_QUATERNION             = 9,
    OVSTAGE_SEMANTIC_MATRIX                 = 10,
    OVSTAGE_SEMANTIC_TEXTURE_COORDINATE     = 11,
    OVSTAGE_SEMANTIC_CONNECTION_PATH_ID     = 12,
    OVSTAGE_SEMANTIC_STRING                 = 13,
} ovstage_attribute_semantic_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Read Group
 *
 * Each group represents attribute data for a subset of prims at a single
 * ordinal. Groups are independently allocated and released.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief A single read result group.
 */
typedef struct {
    ovstage_read_group_id_t read_group_id; /**< Opaque payload identity for release_group. */
    ovx_token_t              attribute; /**< Which attribute this group contains. */
    ovstage_ordinal_t        ordinal;   /**< Data ordinal. */
    bool                     is_delete; /**< true = tombstone group (deletions). */
    bool                     is_array;   /**< true = source is a ragged (variable-length / USD-array or byte-string) column; false = fixed scalar. The data is CSR either way, so this is the only signal that distinguishes e.g. a 1-byte string from a scalar uint8. */
    ovstage_attribute_semantic_t semantic; /**< Authored USD interpretation of the column's bytes (point vs float3, asset vs string, …). NONE when unspecified. Orthogonal to dtype/is_array; see ovstage_attribute_semantic_t. */
    ovstage_prim_group_t     prims;     /**< Which prims this group covers. */
    ovstage_data_t           data;      /**< Attribute data (array of tensors). */
    ovstage_attribute_meta_t meta;      /**< Write floor + layout generation. */
} ovstage_read_group_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Map Group (Write Iterator)
 *
 * Mirrors ovstage_read_group_t but with writable data. Returned by the
 * fetch_map_next slot. Caller fills data, then commits via the unmap_group
 * slot (or unmap_attribute at the end).
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief A writable group returned by the map iterator.
 */
typedef struct {
    /* --- Prim identification --- */
    ovstage_prim_group_t         prims;  /**< Which prims this group covers. */

    /* --- Writable data (caller fills this) --- */
    ovstage_data_t               data;   /**< Writable tensor(s). Caller writes values here. */

    /* --- Metadata --- */
    ovstage_attribute_meta_t     meta;   /**< Layout generation (detect structural changes). */
} ovstage_map_group_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Map Descriptor
 *
 * Input descriptor for the map_attribute slot. The create-vs-existing-type
 * contract is documented on the map_attribute slot in ovstage_api.h.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Descriptor for a zero-copy map session (map_attribute).
 */
typedef struct {
    ovx_string_or_token_t        attribute;  /**< Attribute to map (name or interned token). */
    DLDataType                   dtype;      /**< Requested element type. Required to create a new mapped attribute. Leave zero-initialized only when the target prims already have one unambiguous schema for this attribute name. `dtype.lanes` carries the tuple width. */
    ovstage_attribute_semantic_t semantic;   /**< Authored USD interpretation (point vs float3, asset vs string, ...). NONE = unspecified. Must match an existing prim/name unless the attribute is being created. */
    ovstage_prim_mode_t          prim_mode;  /**< UPSERT (create absent prims, update present) or INSERT (create-only; OVSTAGE_ERROR_PRIM_NOT_FOUND if a prim already exists). */
} ovstage_map_desc_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Filter Types
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Single filter predicate (tests one attribute).
 *
 * - HAS: attribute exists (values/value_count ignored).
 * - IN: attribute value matches one of the provided values.
 * - CONTAINS: composite attribute contains at least one of the values.
 *   Works on arrays (element membership), strings (substring), and composites.
 *   For "contains ALL", use multiple CONTAINS predicates.
 * - PREFIX: string attribute starts with one of the values.
 * - LT/LE/GT/GE: numeric or binary (byte-wise) comparison (single value expected).
 */
typedef struct {
    ovx_string_or_token_t       attribute;     /**< Which attribute to test. */
    ovstage_filter_op_t     op;            /**< Comparison operator. */
    const ovx_string_t*         values;        /**< Match values (NULL for HAS). */
    size_t                      value_count;   /**< Number of values (0 for HAS). */
} ovstage_predicate_t;

/**
 * @brief Query filter — conjunction (AND) of all predicates.
 * All predicates must match for a prim to be included.
 *
 * ## Built-in Attributes for Filtering
 *
 * These metadata attributes are always available for filter predicates:
 *
 * - `"usd-path"` (string): Prim path. Useful with PREFIX to select subtrees.
 *   PREFIX is byte-prefix matching: PREFIX "/World" also matches "/Worldwide".
 *   Append a trailing '/' to scope to the subtree (PREFIX "/World/" → under /World/).
 *
 * - `"usd-schemas"` (string array): Applied schemas. Use CONTAINS to find prims
 *   with a specific schema. Example: CONTAINS "UsdGeomMesh" → all meshes.
 *
 * - `"usd-prim-type"` (string): Prim type name. Use IN for exact match.
 *   Example: IN {"Xform", "Mesh"} → all Xforms and Meshes.
 *
 * - `"usd-parent"` (string): Parent prim path. Use IN for direct children.
 *
 * - `"usd-active"` (bool): Currently NOT supported — any predicate naming it
 *   is rejected with OVSTAGE_ERROR_NOT_SUPPORTED (a live prim is always
 *   active, so the attribute carries no information). Subject to removal in
 *   a future release.
 *
 * - `"usd-children"` (string array): Child prim paths. Use CONTAINS to find
 *   prims with a specific child.
 */
typedef struct {
    const ovstage_predicate_t* predicates;  /**< Array of predicates (AND). */
    size_t                         count;       /**< Number of predicates. */
} ovstage_filter_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Query Result Type
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Result of the fetch_query_result slot — discovered attributes + summary.
 *
 * `all_handle` is the same query handle reserved by the query slot (or built
 * by query_from_path_list). It is included for convenience so consumers
 * receiving only this struct still have the handle in hand.
 */
typedef struct {
    ovstage_query_result_id_t query_result_id; /**< Opaque payload identity for release_query_result. */
    const ovx_token_t*         attributes;        /**< Discovered attribute tokens. */
    size_t                     attribute_count;   /**< Number of distinct attributes. */
    ovstage_query_handle_t all_handle;        /**< Handle covering all matched prims. */
    size_t                     total_prim_count;  /**< Total prims matching the filter. */
} ovstage_query_result_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Write-Floor Descriptor
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Descriptor for advancing the per-attribute and/or global write floor.
 *
 * Usage patterns:
 * - SCOPE_ALL: advance the global write floor and every known attribute (end-of-frame signal).
 * - SCOPE_INCLUDE {"transforms"}: advance only transforms after xform writer completes.
 * - SCOPE_EXCLUDE {"points", "velocities"}: advance all except physics attrs still being written.
 *   With an empty attribute list, SCOPE_EXCLUDE behaves like SCOPE_ALL.
 *
 * To query the global write floor, call the get_attribute_write_floor slot
 * with attribute = {.token = 0, .string = {NULL, 0}}.
 *
 * To advance the global write floor (all attributes), use SCOPE_ALL.
 */
typedef struct {
    ovstage_ordinal_t  ordinal;         /**< New write-floor ordinal. */
    ovstage_scope_t    scope;           /**< ALL, INCLUDE, or EXCLUDE. */
    const ovx_token_t* attributes;      /**< Attribute list (for INCLUDE/EXCLUDE). */
    size_t             attribute_count; /**< Number of attributes in list. */
} ovstage_write_floor_desc_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Write Data
 *
 * Used by the write_attribute and write_attributes slots (copy-in path).
 * Tensor lifetime is selected per attribute write via two mutually exclusive arrays:
 * - `tensors`         : client-managed DLTensors. Caller guarantees they remain
 *                       valid until the corresponding op completes (observable
 *                       via the wait_op slot on the enqueue op_id).
 * - `managed_tensors` : storage-managed DLManagedTensorVersioned. Ownership
 *                       transfers to the implementation, which invokes each
 *                       tensor's deleter when its storage is no longer needed.
 *
 * Tensor transport layout matches ovstage_data_t (see above). Logical
 * attribute kind is declared independently by `is_array`.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Write payload — tensor(s) plus sparsity and GPU sync metadata.
 *
 * Exactly one of `tensors` and `managed_tensors` is non-NULL, selecting the
 * lifetime model for the supplied tensors. Both arrays, when present, have
 * length `tensor_count`.
 *
 * @invariant Exactly one of `tensors` / `managed_tensors` is non-NULL.
 * @invariant `tensor_count >= 1`.
 * @invariant When `is_array == false`, `tensor_count == 1` and the tensor
 *            contains the fixed-size rows stacked along its leading dimension.
 *            The canonical input is `ndim == 1`, `shape == {source_rows}`, with
 *            the complete per-row tuple width in `dtype.lanes`. A compact
 *            convenience input may instead place tuple components in trailing
 *            shape dimensions (for example `(N, 3)` with `lanes == 1`, or
 *            `(N, 4)` with `lanes == 4`). Those per-row dimensions are folded
 *            into `dtype.lanes`; their rank and shape are not preserved. Reads
 *            and maps expose the canonical 1-D form (`(N,)`, lanes 3 or 16 in
 *            these examples). Without `index_map`, `shape[0]` must equal the
 *            logical element count. A flat `(N * L,)`, `lanes == 1` tensor is
 *            not an encoding of `N` rows of width `L`.
 * @invariant When `is_array == true`, `tensor_count == 1` selects packed
 *            uniform-row transport and `tensor_count > 1` selects one tensor
 *            per source row. Neither form changes the declared logical kind.
 *            Packed transport carries no explicit row capacity -- one flat
 *            tensor holds every row back to back -- so its row count is
 *            inferred: the highest `index_map` entry plus one, or the logical
 *            element count when `index_map` is NULL. The uniform row width is
 *            the payload size divided by that inferred count -- not by the
 *            logical element count -- so an `index_map` declares the partition
 *            as well as selecting from it, and may declare more rows than the
 *            logical element count. A partition the payload cannot support is
 *            rejected: the payload must divide evenly across the inferred count,
 *            and the resulting row width must be a whole number of `dtype`
 *            elements. A packed payload cannot carry trailing rows above the
 *            highest `index_map` entry; they would change the inferred width of
 *            every row. Use `tensor_count > 1` when rows must be described
 *            individually.
 * @invariant `index_map` and `mask` are mutually exclusive.
 * @invariant `count` is set (non-zero) when index_map or mask is non-NULL.
 *            Otherwise `count == 0` means the write addresses every prim the
 *            query covers. A non-zero `count` addresses the leading `count`
 *            prims in query order and may not exceed the query's prim count.
 *            The *logical element count* is `count`, or the query's prim count
 *            when `count == 0`.
 * @invariant `index_map` has `count` entries and holds source row indices, not
 *            target prim indices: logical element `i` reads transported row
 *            `index_map[i]`. Where the payload declares a transported row count
 *            -- `shape[0]` when `is_array == false`, `tensor_count` for per-row
 *            array transport -- every entry is less than it, and an out-of-range
 *            entry is rejected rather than reinterpreted. Rows no entry
 *            references are left unused, so the payload may carry more rows than
 *            the query has prims. Packed array transport declares no row count;
 *            there the map defines one, as described above.
 * @invariant `mask` selects which of the `count` logical elements are written;
 *            unselected prims are left untouched. It does not change how the
 *            payload is cut into rows, so the payload still carries a row for
 *            every logical element, including the unselected ones. A non-NULL
 *            `mask` addresses at least `ceil(count / 64)` `uint64_t` words --
 *            the implementation reads that many -- with element `i` at bit
 *            `i % 64` of word `i / 64`.
 * @invariant `cuda_sync` is `{0, 0}` for CPU-resident or already-synchronized data.
 *
 * Every attribute, including reserved metadata, follows this contract.
 * `usd-prim-type` has the canonical fixed token-id schema (`is_array ==
 * false`); `usd-schemas` has the canonical token-id-array schema (`is_array
 * == true`). A conflicting declaration is rejected. Both attributes use the
 * normal row/count/mask/index_map coverage rules; their names do not trigger
 * implicit broadcasting or kind correction.
 */
typedef struct {
    const DLTensor*                        tensors;          /**< Client-managed tensors (read-only to the implementation). Must remain valid until op completes. NULL when managed_tensors is provided. */
    const DLManagedTensorVersioned* const* managed_tensors;  /**< Storage-managed tensors (read-only to the implementation). Deleter invoked when storage no longer needs them. NULL when tensors is provided. */
    uint32_t                               tensor_count;     /**< Number of tensors in tensors/managed_tensors; selects transport layout, not attribute kind. */

    uint32_t                               count;       /**< Logical element count; addresses the leading `count` prims in query order. Required (non-zero) when index_map/mask present; `0` = every prim the query covers. */
    const uint32_t*                        index_map;   /**< NULL = identity; non-NULL = gather/reorder/dedup. `count` source row indices, each below the transported row count. */
    ovstage_mask_t                         mask;        /**< NULL = all valid; non-NULL = bitmask over elements, at least `ceil(count / 64)` words long. Selects target elements; it does not change how the payload is cut into rows. */
    ovstage_cuda_sync_t                    cuda_sync;   /**< GPU sync applied before access; `{0,0}` = none. See ovstage_cuda_sync_t. */
    ovstage_attribute_semantic_t           semantic;    /**< Authored USD interpretation for this write (NONE = unspecified). Must match an existing prim/name unless the attribute is being created. Geometric semantics are recorded at creation and surfaced back on read; see ovstage_attribute_semantic_t. */
    bool                                   is_array;    /**< Logical attribute kind. Sole authority for fixed (`false`) versus array (`true`) storage. Kept last to minimize padding; it occupies existing tail padding on 64-bit ABIs. */
} ovstage_write_data_t;

/**
 * @brief One named attribute write passed to write_attributes.
 *
 * Attribute kind is declared by data.is_array with the same contract as
 * write_attribute. Tensor count, tensor shape, payload width, attribute name,
 * semantic, and existing storage never substitute for that declaration.
 */
typedef struct {
    ovx_string_or_token_t attribute; /**< Attribute to write (name or interned token). */
    ovstage_write_data_t  data;      /**< Write payload for this attribute. */
} ovstage_attribute_write_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Instance Bundle
 *
 * Pairs a per-implementation context with the vtable that operates on it.
 * Consumers receive an ovstage_instance_t* and either call
 * `instance->vtable->slot(instance->context, ...)` directly or use the
 * convenience wrappers in `ovstage_api_utils.h` and sibling extension APIs.
 *
 * Instance creation and destruction are NOT part of the contract. Each
 * backend (e.g. ovstage) provides its own factory function.
 * ═══════════════════════════════════════════════════════════════════════════════ */

#ifndef OVSTAGE_INSTANCE_T_DECLARED
#define OVSTAGE_INSTANCE_T_DECLARED
typedef struct ovstage_instance_t {
    const ovstage_vtable_t* vtable;  /**< Vtable pointer (see ovstage_api.h). */
    ovstage_context_t*      context; /**< Implementation-specific context. */
} ovstage_instance_t;
#else
struct ovstage_instance_t {
    const ovstage_vtable_t* vtable;  /**< Vtable pointer (see ovstage_api.h). */
    ovstage_context_t*      context; /**< Implementation-specific context. */
};
#endif

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_API_TYPES_H */
