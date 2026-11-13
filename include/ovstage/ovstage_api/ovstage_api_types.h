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
 * @brief ovstage_api — shared types, handles, and the instance.
 *
 * @details
 * ovstage_api is a vtable-based contract for asynchronous, ordinal-keyed
 * zero-copy simulation data access. It is the abstract API that various
 * stage implementations can provide.
 *
 * This header defines the data types (handles, error codes, enums, payload
 * structs), the forward declarations for the vtables and the context, and the
 * `ovstage_instance_t` struct that holds them.
 *
 * The vtable itself is defined in `ovstage_api.h`. Convenience
 * `static inline` wrappers around the core data-plane slots live in
 * `ovstage_api_utils.h`; higher-level extension APIs may live in sibling
 * headers.
 *
 * ## Lifecycle policy
 *
 * The vtable owns *resource* lifecycles (queries, reads, maps, ops, groups,
 * query results, ordinal queries) but **not** the creation of the instance.
 * Each implementation provides its own factory function, which is not part of
 * this contract. The destruction of an instance is the destroy_instance member
 * of the lifecycle family vtable (ovstage_ext_vtables.h), which the factory
 * fills with the destructor of its implementation.
 *
 * The execution-model documentation lives at the top of `ovstage_api.h`
 * alongside the vtable definition.
 *
 * @version 0.4.0
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
 * ovx_primpath_list_t, ovx_token_or_string_t, ovx_api_result_t, and the
 * opaque path_dictionary_instance_t bundle used by the get_path_dictionary
 * vtable slot. */
#include <ovx/path_dictionary/path_dictionary_types.h>

/* Compile-time API version (SemVer). The runtime get_version slot returns
 * exactly these values, so a compile-time `#if` check and a runtime query
 * always agree. */
#define OVSTAGE_VERSION_MAJOR 0
#define OVSTAGE_VERSION_MINOR 4
#define OVSTAGE_VERSION_PATCH 0

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

/** @brief The opaque context of the data plane, stored in the data-plane
 *         instance. A slot of the vtable takes it first. */
typedef struct ovstage_data_plane_context_t ovstage_data_plane_context_t;

/** @brief Vtable defined in ovstage_api.h. */
typedef struct ovstage_data_plane_vtable_t  ovstage_data_plane_vtable_t;

/** @brief The family vtables of the extension instances, defined in
 *         ovstage_ext_vtables.h. The extension instances below point at them. */
typedef struct ovstage_lifecycle_vtable_t  ovstage_lifecycle_vtable_t;
typedef struct ovstage_hierarchy_vtable_t  ovstage_hierarchy_vtable_t;
typedef struct ovstage_population_vtable_t ovstage_population_vtable_t;
typedef struct ovstage_usd_vtable_t        ovstage_usd_vtable_t;
typedef struct ovstage_instancing_vtable_t ovstage_instancing_vtable_t;

/** @brief The opaque context of each family, stored in the extension instance
 *         of the family. An incomplete type like ovstage_data_plane_context_t:
 *         the bodies of the family vtable cast it to the state type of their
 *         implementation, and a member of the family vtable takes it first. */
typedef struct ovstage_lifecycle_context_t  ovstage_lifecycle_context_t;
typedef struct ovstage_hierarchy_context_t  ovstage_hierarchy_context_t;
typedef struct ovstage_population_context_t ovstage_population_context_t;
typedef struct ovstage_usd_context_t        ovstage_usd_context_t;
typedef struct ovstage_instancing_context_t ovstage_instancing_context_t;

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
    OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION = 5,  /**< Mutation blocked by an effective floor or retained value/prim-lifecycle change; read selects an unsealed change at/before its end. */
    OVSTAGE_ERROR_NOT_SUPPORTED         = 6,
    OVSTAGE_ERROR_QUEUE_FULL            = 7,  /**< The call cannot be admitted now; retry it after the operation that blocks it returns. The implementation remarks name the cases. */
    OVSTAGE_ERROR_END_OF_ITERATION      = 8,  /**< No more groups to iterate. */
    OVSTAGE_ERROR_OUT_OF_MEMORY         = 9,
    OVSTAGE_ERROR_LAYOUT_CHANGED        = 10, /**< Layout changed during map. */
    OVSTAGE_ERROR_TIMEOUT               = 11, /**< Fetch/wait did not complete within timeout. */
    OVSTAGE_ERROR_OP_FAILED             = 12, /**< An enqueued op failed; see get_last_op_error. */
    OVSTAGE_ERROR_OUT_OF_RANGE          = 13, /**< Values needed for the requested snapshot or change range are no longer available. */
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
 * Signed int64_t ordering. Every stage starts sealed and empty through
 * OVSTAGE_ORDINAL_MIN (inclusive), so writes at that minimum are rejected.
 * Writable ordinals are OVSTAGE_ORDINAL_MIN + 1 through OVSTAGE_ORDINAL_MAX,
 * including negative values and zero.
 * Sealing through zero rejects subsequent writes at or below zero.
 * See ovstage_ordinal_range_t for how "latest" reads are expressed.
 */
#ifndef OVSTAGE_ORDINAL_T_DECLARED
#define OVSTAGE_ORDINAL_T_DECLARED
typedef int64_t ovstage_ordinal_t;
#endif

/** Smallest representable ordinal; the initial sealed empty floor, not writable. */
#ifndef OVSTAGE_ORDINAL_MIN
#define OVSTAGE_ORDINAL_MIN ((ovstage_ordinal_t)INT64_MIN)
#endif
/** Largest representable ordinal; also the current-value read end. */
#ifndef OVSTAGE_ORDINAL_MAX
#define OVSTAGE_ORDINAL_MAX ((ovstage_ordinal_t)INT64_MAX)
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
typedef struct ovstage_op_id_t
{
    uint64_t raw_id;
} ovstage_op_id_t;
#ifdef __cplusplus
#define OVSTAGE_INVALID_OP_ID ovstage_op_id_t{0}
#else
#define OVSTAGE_INVALID_OP_ID ((ovstage_op_id_t){0})
#endif

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
 * Handles are distinct C-compatible structs containing uint64_t raw_id.
 * Use .raw_id only at numeric boundaries; keep API arguments and storage typed.
 * Types distinguish kinds, not owners or resource liveness. Copying a handle
 * does not retain its resource.
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
typedef struct ovstage_query_handle_t
{
    uint64_t raw_id;
} ovstage_query_handle_t;
#ifdef __cplusplus
#define OVSTAGE_INVALID_QUERY_HANDLE ovstage_query_handle_t{0}
#else
#define OVSTAGE_INVALID_QUERY_HANDLE ((ovstage_query_handle_t){0})
#endif

/** @brief Read handle — identifies an enqueued multi-attribute read. */
typedef struct ovstage_read_handle_t
{
    uint64_t raw_id;
} ovstage_read_handle_t;
#ifdef __cplusplus
#define OVSTAGE_INVALID_READ_HANDLE ovstage_read_handle_t{0}
#else
#define OVSTAGE_INVALID_READ_HANDLE ((ovstage_read_handle_t){0})
#endif

/** @brief Map handle — identifies an enqueued mapped-write session. */
typedef struct ovstage_map_handle_t
{
    uint64_t raw_id;
} ovstage_map_handle_t;
#ifdef __cplusplus
#define OVSTAGE_INVALID_MAP_HANDLE ovstage_map_handle_t{0}
#else
#define OVSTAGE_INVALID_MAP_HANDLE ((ovstage_map_handle_t){0})
#endif

/**
 * @brief Ordinal-query handle — identifies an enqueued read of an ordinal
 *        scalar, such as the global or per-attribute write floor.
 */
typedef struct ovstage_ordinal_query_handle_t
{
    uint64_t raw_id;
} ovstage_ordinal_query_handle_t;
#ifdef __cplusplus
#define OVSTAGE_INVALID_ORDINAL_QUERY_HANDLE ovstage_ordinal_query_handle_t{0}
#else
#define OVSTAGE_INVALID_ORDINAL_QUERY_HANDLE ((ovstage_ordinal_query_handle_t){0})
#endif

/** @brief Opaque identity for a fetched read group payload. */
typedef struct ovstage_read_group_id_t
{
    uint64_t raw_id;
} ovstage_read_group_id_t;
#ifdef __cplusplus
#define OVSTAGE_INVALID_READ_GROUP_ID ovstage_read_group_id_t{0}
#else
#define OVSTAGE_INVALID_READ_GROUP_ID ((ovstage_read_group_id_t){0})
#endif

/** @brief Opaque identity for a fetched query result payload. */
typedef struct ovstage_query_result_id_t
{
    uint64_t raw_id;
} ovstage_query_result_id_t;
#ifdef __cplusplus
#define OVSTAGE_INVALID_QUERY_RESULT_ID ovstage_query_result_id_t{0}
#else
#define OVSTAGE_INVALID_QUERY_RESULT_ID ((ovstage_query_result_id_t){0})
#endif

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
 * Clone Result
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Completion report of a finished clone operation (the `clone` slot).
 *
 * Plain values copied out by the `fetch_clone_result` slot; nothing to release.
 * `prims_cloned` sums the sizes of the targets' scene hierarchy subtrees after
 * cloning, including each target root. It counts prims created or merged into and
 * any prims already attached beneath them, including children entered in the
 * hierarchy from a deleted root that the clone re-attaches under its new root. A
 * prim whose path lies beneath a target but remains detached from its scene
 * hierarchy is not counted. For disjoint targets that merge into no existing
 * prims, it equals
 * `source_prim_count * target_count`. A zero-target clone reports
 * `{ordinal, 0, source_prim_count, 0}`.
 *
 * @remark Target paths are expected to be disjoint and outside the source
 *         subtree; a target nested under another target is counted as enumerated.
 */
typedef struct {
    ovstage_ordinal_t ordinal;           /**< Ordinal the clone was recorded at. */
    size_t            target_count;      /**< `num_target_paths` as submitted. */
    size_t            source_prim_count; /**< Prims in the source subtree, root inclusive. */
    size_t            prims_cloned;      /**< Prims in all target scene hierarchy subtrees after cloning. */
} ovstage_clone_result_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Enumerations
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Filter predicate operators for query matching.
 *
 * Notes:
 * - Only a closed list of (attribute, operator) pairs is supported; every other pair is rejected synchronously by the query call with OVSTAGE_ERROR_NOT_SUPPORTED. Supported pairs: HAS and NOT_HAS on any attribute name except "usd-active"; IN on "usd-path", "usd-parent", "usd-prim-type"; CONTAINS on "usd-children", "usd-schemas", "usd-ancestor-types"; PREFIX on "usd-path". See ovstage_filter_t.
 * - NOT_HAS is the complement of HAS on the same attribute: it accepts the same attributes, and a live prim matches exactly one of the two.
 * - CONTAINS matches at least one of the provided values. For "contains all", use multiple CONTAINS predicates (one per required value).
 * - IN, CONTAINS, and PREFIX are not general value operators: they do not apply to ordinary (non-built-in) attributes.
 * - LT, LE, GT, and GE are reserved: they are not supported on any attribute in this release and return OVSTAGE_ERROR_NOT_SUPPORTED.
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
    OVSTAGE_FILTER_OP_NOT_HAS  = 8,  /**< Attribute does not exist on prim (no value test). */
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

/**
 * @brief What a write or map does when a target prim already has the attribute
 *        with a different schema.
 *
 * An attribute's schema is its storage -- dtype, row layout, and fixed versus
 * array kind -- and its authored semantic.
 *
 * - REJECT (default): the write or map fails.
 * - FORCE: the target prim's existing schema is replaced by the written one.
 *
 * See write_attribute and map_attribute for how a replacement is applied.
 */
typedef enum {
    OVSTAGE_ATTRIBUTE_MODE_REJECT = 0,  /**< A differing existing schema fails the write or map (default). */
    OVSTAGE_ATTRIBUTE_MODE_FORCE  = 1,  /**< A differing existing schema is replaced by the written one. */
} ovstage_attribute_mode_t;

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
 * - Read and map tensors expose scalar-lane, row-major shapes. The leading dimension
 *   is the transported data-row count for fixed-size values, or the element
 *   count for each array-value tensor. Multi-component values add a trailing
 *   component dimension (for example, point3f is `[N, 3]`); a MATRIX semantic
 *   with a supported square component count adds two dimensions instead
 *   (matrix2d, matrix3d and matrix4d are `[N, 2, 2]`, `[N, 3, 3]` and
 *   `[N, 4, 4]`). `dtype.lanes` is always 1. Scalars have shape `[N]`.
 *   Generic tuples retain a single component axis, including non-square
 *   tuples; their dimensions are not inferred as matrices. The original
 *   write shape is not retained: read shape is reconstructed from the stored
 *   component count and attribute semantic.
 * - Reads and maps share this layout. Read payloads are read-only; map
 *   payloads are writable until accepted unmap submission. Descriptor
 *   metadata remains implementation-owned and read-only in both cases.
 * - Every returned tensor carries explicit compact strides: `strides` is
 *   non-NULL and describes row-major scalar elements.
 *   DLPack v1.2+ forbids NULL strides when `ndim != 0`, and OVStage conforms
 *   on output; NULL strides remain accepted on write-side input as compact
 *   row-major.
 *
 * Tensor count is not an attribute-kind signal. Read results report logical
 * kind in ovstage_read_column_t::is_array. Copy-in writes declare it in
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
 * but uses its documented canonical input layout and additionally allows
 * storage-managed tensor lifetime; see the Write
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
 * @invariant A fixed-size value tensor has leading shape equal to the
 *            transported data-row count. Read and map tensors have
 *            `dtype.lanes == 1` and type-aware trailing dimensions. Logical element `i`
 *            selects row `i` when `index_map` is NULL, or row `index_map[i]`
 *            otherwise.
 * @invariant Every published tensor has non-NULL `strides` with
 *            compact row-major strides; NULL strides are never produced
 *            (DLPack v1.2+ forbids NULL strides when `ndim != 0`).
 * @invariant For delete/tombstone read columns (`is_delete == true`),
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
 * - Snapshot: has_start_ordinal = false, end_ordinal = N. Requests values at N
 *   for each requested attribute on all queried paths. A zero-initialized
 *   range is therefore a snapshot at ordinal 0, not a request for current values.
 * - Change range [start, end]: has_start_ordinal = true,
 *   start_ordinal <= end_ordinal. Selects queried attribute/path pairs with
 *   recorded changes in [start_ordinal, end_ordinal], including both ends.
 *
 * A selected change at or before the end and above the effective write floor
 * fails with OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION. If the read detects that the
 * requested values are no longer available, it fails with OVSTAGE_ERROR_OUT_OF_RANGE.
 * Use OVSTAGE_ORDINAL_MAX as the end to request current values; a selected unsealed
 * change still fails with OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION.
 *
 * The requested end need not be at or below the attribute's write floor
 * (ovstage_get_attribute_write_floor). While the end is above the floor,
 * changes in (floor, end] can still be admitted and, once sealed, change what
 * the same request returns. Once the end is at or below the floor, later
 * writes, deletes and maps cannot change the recorded values that a successful
 * read of the same request returns for paths the query still selects. This
 * does not cover derived attributes or query membership, which reflect current
 * state, changes that ovstage_compute_hierarchy records at or below the floor,
 * or history below the retention frontier (see below).
 *
 * A returned column's ordinal (ovstage_read_column_t::ordinal) is informational.
 * It may be above or below the requested end, and it is not, by itself, proof
 * of which state the payload represents.
 *
 * A selected key whose schema was replaced in the change interval (a write or
 * map commit with OVSTAGE_ATTRIBUTE_MODE_FORCE) returns both a tombstone for the
 * previous schema and its payload in the written schema; see write_attribute.
 *
 * Implementations may coalesce or discard history below their inclusive
 * retention frontier. Query ovstage_get_oldest_preserved_ordinal before relying
 * on an old snapshot or interval. Below the frontier, history may be insufficient
 * to detect unavailable values; success does not guarantee the requested earlier state.
 *
 * @remark This implementation stores only the current values. For each
 * attribute, it first checks for the write-floor violation described above.
 * Otherwise, a recorded change after end_ordinal on a selected path causes
 * OVSTAGE_ERROR_OUT_OF_RANGE, whether that later change is sealed or not.
 * Sealing the later change does not restore the older values. A read stops
 * at the first attribute that fails either check. An end of OVSTAGE_ORDINAL_MAX skips
 * this later-change check. Because a later change after the end makes a
 * bounded request fail with OVSTAGE_ERROR_OUT_OF_RANGE, a bounded read is not
 * a replay point.
 *
 * These checks use recorded change history. Snapshot reads of values without
 * such history, including derived attributes such as usd-path, return current
 * state. Queries select current paths, not the paths present at N.
 * After an attribute's deletion history is discarded, a snapshot from before
 * that deletion may succeed without a row for that attribute.
 *
 * @remark Under OVSTAGE_ORDINAL_ADMISSION_FLOOR_ONLY, a write or delete above
 * the applicable floors can be admitted even when the same attribute and path
 * already has a change at a higher ordinal. STRICTLY_INCREASING and the default
 * NONDECREASING policy reject such older changes against retained history.
 * Under FLOOR_ONLY, reads return the most recently applied change rather than
 * the one at the highest ordinal, and a read whose end covers both changes can
 * succeed with it. A group's ordinal then need not match the change whose
 * value is returned: depending on the operations that changed the attribute,
 * it can be the most recently applied ordinal or the highest one recorded, and
 * it may come from a prim outside the query. Groups of derived attributes such
 * as usd-path report the requested end.
 *
 * @remark The current implementation retains bounded exact change membership
 * at or above the frontier reported by
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
    ovstage_ordinal_t end_ordinal;       /**< Upper-bound ordinal; OVSTAGE_ORDINAL_MAX requests current values. */
    bool              has_start_ordinal; /**< false = snapshot at end; true = changes in [start, end]. */
} ovstage_ordinal_range_t;

/** Per-attribute USD semantic — the authored *interpretation* of a column's
    *  bytes, orthogonal to its `DLDataType` storage shape. ovpopulation derives
    *  this from the USD `SdfValueTypeName` (role + scalar type) and carries it
    *  through writes; consumers (e.g. runtime→USD export) use it to recover the
    *  authored USD type (point3f vs float3, asset vs string, etc.).
    *  Tuple width comes from `dtype.lanes`, precision from `dtype.bits`; these
    *  roles deliberately do not encode dimensional suffixes such as 3f or 4d.
    *
    *  Attributes with the same name and numeric layout may have different
    *  semantics on different prims. The semantic is part of each attribute's
    *  schema. Changing an existing target's schema requires
    *  `OVSTAGE_ATTRIBUTE_MODE_FORCE`, or deletion followed by recreation at a
    *  later ordinal. `OVSTAGE_SEMANTIC_NONE` requests no role; it preserves the
    *  existing role when the attribute's storage is unchanged. Read results
    *  report the attribute's semantic.
    *
    *    - Geometric semantics (POINT, VECTOR, NORMAL, COLOR, QUATERNION,
    *      MATRIX, FRAME, TEXTURE_COORDINATE) record a geometric role on the
    *      column. Storage stays in the requested numeric `dtype`.
    *
    *    - TIME_CODE marks a time code — a unitless time value — again with
    *      storage in the requested numeric `dtype`. Only the plain numeric
    *      value is carried: sentinel time codes have no portable numeric
    *      encoding, so they are not representable.
    *
    *    - ID semantics select the corresponding ID storage type and
    *      require pre-interned id payloads (producers — e.g. ovpopulation —
    *      must intern via the path or token dictionary before
    *      writing; ovstage does not stringify or resolve):
    *          * TOKEN_ID             → `dtype = {kDLUInt, 64, 1}`
    *                                   (one 64-bit token id per row; id 0 is
    *                                    the empty token)
    *          * RELATIONSHIP_PATH_ID → `dtype = {kDLUInt, 64, 1}`
    *                                   (one 64-bit path id per element;
    *                                    `is_array = true` is a
    *                                    relationship's target list,
    *                                    `is_array = false` holds exactly
    *                                    one path per row)
    *          * CONNECTION_PATH_ID   → `dtype = {kDLUInt, 64, 2}`
    *                                   (one `(path_id, token_id)` pair per row,
    *                                    using one 16-byte element per row)
    *          * ASSET_PATH_ID        → `dtype = {kDLUInt, 64, 2}`
    *                                   (one `(authored_token, resolved_token)`
    *                                    pair per row, using one 16-byte element
    *                                    per row)
    *
    *      An asset has two paths: the path as authored, and the path after
    *      resolution. ASSET_PATH_ID carries both as token ids. A token id of 0
    *      means no path, so an unresolved asset has a resolved token id of 0.
    *      CONNECTION_PATH_ID writes are singular: they require exactly one
    *      pair per target prim. Extra pairs are rejected.
    *
    *    - PATH_EXPRESSION_STRING carries the expression text as one interned
    *      token id, dtype `{kDLUInt, 64, 1}`. `is_array = false` is a scalar
    *      `pathExpression`; `true` is `pathExpression[]`, one id per element.
    *      Other layouts return `OVSTAGE_ERROR_INVALID_ARGUMENT`. A token id of
    *      0 means no expression. ovstage does not evaluate the expression; it
    *      stores the id of the authored text.
    *
    *    - STRING carries a plain USD `string` value as raw UTF-8 bytes. The
    *      payload is a ragged `kDLUInt,8,1` byte array (one string per row,
    *      `is_array = true`) — NOT a pre-interned token id. It stamps the
    *      USD-string role on the column, so the resulting column is the
    *      canonical USD-string representation. Read recovers the semantic by
    *      decoding that role. The row is length-delimited: every byte,
    *      including a trailing NUL, is part of the value, so do not append a
    *      C-style terminator. USD cannot persist a NUL byte inside a string,
    *      so export refuses a STRING row that contains one and reports it as
    *      unsupported. Only an untyped (SEMANTIC_NONE) byte row keeps the
    *      legacy reading where one trailing NUL is an optional terminator.
    */
typedef enum
{
    OVSTAGE_SEMANTIC_NONE                   = 0,
    OVSTAGE_SEMANTIC_ASSET_PATH_ID          = 1,
    OVSTAGE_SEMANTIC_TOKEN_ID               = 2,
    OVSTAGE_SEMANTIC_PATH_EXPRESSION_STRING = 3,
    OVSTAGE_SEMANTIC_RELATIONSHIP_PATH_ID   = 4,
    OVSTAGE_SEMANTIC_POINT                  = 5,
    OVSTAGE_SEMANTIC_VECTOR                 = 6,
    OVSTAGE_SEMANTIC_NORMAL                 = 7,
    OVSTAGE_SEMANTIC_COLOR                  = 8,
    /** Four floating-point lanes in (x, y, z, w) order: imaginary part first,
     *  real part last. */
    OVSTAGE_SEMANTIC_QUATERNION             = 9,
    OVSTAGE_SEMANTIC_MATRIX                 = 10,
    OVSTAGE_SEMANTIC_TEXTURE_COORDINATE     = 11,
    OVSTAGE_SEMANTIC_CONNECTION_PATH_ID     = 12,
    OVSTAGE_SEMANTIC_STRING                 = 13,
    OVSTAGE_SEMANTIC_TIME_CODE              = 14,
    OVSTAGE_SEMANTIC_FRAME                  = 15,
} ovstage_attribute_semantic_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Read Groups and Columns
 *
 * A group owns one logical prim-row layout shared by one or more independently
 * mapped attribute columns. Groups are independently acquired and released.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief One attribute column in a read result group.
 */
typedef struct {
    ovx_token_t attribute;       /**< Attribute represented by this column. */
    ovstage_ordinal_t ordinal;   /**< Informational ordinal; may differ from the requested end and does not by itself prove which state the payload represents. See ovstage_ordinal_range_t. */
    bool is_delete;              /**< true = tombstone column (deletions). */
    bool is_array;               /**< true = ragged (variable-length / USD-array or byte-string) values; false =
                                      fixed-size values. This is the attribute-kind signal regardless of transport. */
    ovstage_attribute_semantic_t semantic; /**< Authored interpretation of the column's bytes (point vs float3,
                                                asset vs string, …). NONE when unspecified. Orthogonal to
                                                dtype/is_array; see ovstage_attribute_semantic_t. */
    ovstage_data_t data;                  /**< Attribute values and their mapping to the group's logical rows. */
    ovstage_attribute_meta_t meta;        /**< Write floor + attribute layout generation. */
} ovstage_read_column_t;

/**
 * @brief A read result group containing aligned attribute columns.
 *
 * Logical row i identifies the same prim for every column. Each column resolves
 * its value for that row independently through its ovstage_data_t mapping.
 * Columns are ordered by the caller's requested attribute order after duplicate
 * attribute tokens are removed.
 *
 * OVStage combines only backend fragments that already have the same logical
 * prim-row layout. It does not intersect, reorder, repartition, copy, or
 * synthesize values to force alignment. An attribute absent from every group row
 * is omitted; a read whose requested attributes are absent everywhere returns no
 * groups. Partial presence is represented by the column's data mapping or by a
 * separate group. Single-column groups therefore remain valid.
 *
 * No ordering, disjointness, or stable-partition guarantee applies across
 * different groups. The group owns every borrowed prim, column, tensor, mapping,
 * and metadata view until ovstage_release_groups() is called. Releasing the read
 * handle does not release an already fetched group.
 */
typedef struct {
    ovstage_read_group_id_t read_group_id; /**< Opaque payload identity for ovstage_release_groups(). */
    ovstage_prim_group_t prims;            /**< Shared logical prim rows for every column. */
    const ovstage_read_column_t* columns;  /**< Ordered borrowed columns; non-NULL when column_count is nonzero. */
    size_t column_count;                   /**< Number of columns; always at least one for a returned group. */
} ovstage_read_group_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Map Group (Write Iterator)
 *
 * Map groups retain the single-attribute writable result model; unlike a read
 * group, a map group has one data payload rather than an array of columns.
 * Returned by the fetch_map_next slot. Caller fills data, then commits via the
 * unmap_group slot (or unmap_attribute at the end).
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
 * @brief Descriptor for a map session (map_attribute).
 */
typedef struct {
    ovx_token_or_string_t        attribute;  /**< Attribute to map (name or interned token). */
    DLDataType                   dtype;      /**< Requested element type. Required to create a new mapped attribute. Leave zero-initialized only when the target prims already have one unambiguous schema for this attribute name. `dtype.lanes` carries the tuple width. */
    ovstage_attribute_semantic_t semantic;   /**< Authored USD interpretation (point vs float3, asset vs string, ...). NONE = unspecified; adopts an unambiguous compatible existing identity. Conflicting authored roles on a target prim/name are rejected unless attribute_mode is FORCE. Takes effect only when unmap succeeds. */
    ovstage_prim_mode_t          prim_mode;  /**< UPSERT (create absent prims, update present) or INSERT (create-only; OVSTAGE_ERROR_PRIM_NOT_FOUND if a prim already exists). */
    ovstage_attribute_mode_t     attribute_mode; /**< REJECT (default) fails setup when a target prim has the attribute with a different schema; FORCE replaces that schema when each group commits. FORCE requires `dtype`. See ovstage_attribute_mode_t. */
} ovstage_map_desc_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Filter Types
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Single filter predicate (tests one attribute).
 *
 * - HAS: attribute exists (values/value_count ignored).
 * - NOT_HAS: attribute does not exist (values/value_count ignored). Several
 *   NOT_HAS predicates leave out every prim that has any of their attributes.
 * - IN: attribute value matches one of the provided values.
 * - CONTAINS: composite attribute contains at least one of the values.
 *   Works on arrays (element membership), strings (substring), and composites.
 *   For "contains ALL", use multiple CONTAINS predicates.
 * - PREFIX: string attribute starts with one of the values.
 * - LT/LE/GT/GE: numeric or binary (byte-wise) comparison (single value expected).
 */
typedef struct {
    ovx_token_or_string_t       attribute;     /**< Which attribute to test. */
    ovstage_filter_op_t     op;            /**< Comparison operator. */
    const ovx_string_t*         values;        /**< Match values (NULL for HAS and NOT_HAS). */
    size_t                      value_count;   /**< Number of values (0 for HAS and NOT_HAS). */
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
 *
 * - `"usd-ancestor-types"` (string array): Ancestor prim type names. Use CONTAINS to find prims by a base type.
 *
 * Supported (attribute, operator) pairs: HAS and NOT_HAS on any attribute name except "usd-active"; IN on "usd-path", "usd-parent", "usd-prim-type"; CONTAINS on "usd-children", "usd-schemas", "usd-ancestor-types"; PREFIX on "usd-path". Any other pair is rejected synchronously with OVSTAGE_ERROR_NOT_SUPPORTED.
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
 *                       via the wait_op slot on the enqueue op_id). A placed
 *                       write with an `availability_event` (see the
 *                       experimental ovstage_placement_t) may still read them
 *                       after that, so there they remain valid until the event
 *                       completes, or, if it could not be recorded, until the
 *                       synchronization ovstage_placement_t names for that case.
 * - `managed_tensors` : storage-managed DLManagedTensorVersioned. Ownership
 *                       transfers to the implementation, which invokes each
 *                       tensor's deleter once no work of the op reads its
 *                       storage. With an `availability_event` that can be after
 *                       the op completes, on an implementation-owned thread, and
 *                       at the latest while the instance is destroyed.
 *
 * Tensor selection matches ovstage_data_t (see above); write tensors use
 * the canonical input layout documented below. Logical
 * attribute kind is declared independently by `is_array`.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Write payload — tensor(s) plus sparsity and GPU sync metadata.
 *
 * Exactly one of `tensors` and `managed_tensors` is non-NULL, selecting the
 * lifetime model for the supplied tensors. Both arrays, when present, have
 * length `tensor_count`.
 *
 * @remark The caller must ensure that destination writes do not overwrite or
 * invalidate source memory before it is consumed. Keeping a source alive,
 * including through `managed_tensors`, does not by itself make
 * source/destination aliasing safe. Do not rely on an implicit input snapshot.
 * If input may overlap storage affected by the write, copy its values into
 * independent memory before submission and retain that copy for the required
 * input lifetime. Reusing an independent source row for multiple destinations
 * through `index_map` remains supported.
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
 *            and maps instead expose scalar lanes with component axes derived
 *            from the stored width and semantic: `(N, 3)` for a three-component
 *            tuple, `(N, 4, 4)` for a 4x4 MATRIX, and `(N, 16)` for a generic
 *            16-component tuple. Without `index_map`, `shape[0]` must equal the
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
    const DLTensor*                        tensors;          /**< Client-managed tensors (read-only to the implementation). Must remain valid until op completes, or until the placement's availability_event has completed when one is supplied (longer if that event could not be recorded; see ovstage_placement_t). NULL when managed_tensors is provided. */
    const DLManagedTensorVersioned* const* managed_tensors;  /**< Storage-managed tensors (read-only to the implementation). Deleter invoked once no work of the op reads them, which can be after op completion when an availability_event is supplied. NULL when tensors is provided. */
    uint32_t                               tensor_count;     /**< Number of tensors in tensors/managed_tensors; selects transport layout, not attribute kind. */

    uint32_t                               count;       /**< Logical element count; addresses the leading `count` prims in query order. Required (non-zero) when index_map/mask present; `0` = every prim the query covers. */
    const uint32_t*                        index_map;   /**< NULL = identity; non-NULL = gather/reorder/dedup. `count` source row indices, each below the transported row count. */
    ovstage_mask_t                         mask;        /**< NULL = all valid; non-NULL = bitmask over elements, at least `ceil(count / 64)` words long. Selects target elements; it does not change how the payload is cut into rows. */
    ovstage_cuda_sync_t                    cuda_sync;   /**< GPU sync applied before access; `{0,0}` = none. See ovstage_cuda_sync_t. */
    ovstage_attribute_semantic_t           semantic;    /**< Authored USD interpretation for this write (NONE = unspecified). Must match an existing prim/name unless the attribute is being created or attribute_mode is FORCE. Geometric semantics are recorded at creation and surfaced back on read; see ovstage_attribute_semantic_t. */
    bool                                   is_array;    /**< Logical attribute kind. Sole authority for fixed (`false`) versus array (`true`) storage. */
    ovstage_attribute_mode_t               attribute_mode; /**< REJECT (default) fails the write when a target prim has the attribute with a different schema; FORCE replaces that schema. See ovstage_attribute_mode_t. Last field: the fields before it keep their earlier offsets. */
} ovstage_write_data_t;

/**
 * @brief One named attribute write passed to write_attributes.
 *
 * Attribute kind is declared by data.is_array with the same contract as
 * write_attribute. Tensor count, tensor shape, payload width, attribute name,
 * semantic, and existing storage never substitute for that declaration.
 */
typedef struct {
    ovx_token_or_string_t attribute; /**< Attribute to write (name or interned token). */
    ovstage_write_data_t  data;      /**< Write payload for this attribute. */
} ovstage_attribute_write_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Instance
 *
 * An ovstage_instance_t holds the data-plane instance, that is the public
 * vtable with its context, and the extension instances, one per family. Every
 * instance of the API has the same form: a vtable pointer and the context its
 * bodies read. Consumers receive an ovstage_instance_t* and either call
 * `instance->data_plane.vtable->slot(instance->data_plane.context, ...)`
 * directly or use the wrappers in `ovstage_api_utils.h` and the clone
 * wrappers in ovstage.h. The functions of the hierarchy, population, usd and
 * instancing families are inline wrappers in ovstage.h, ovstage_population.h,
 * ovstage_population_export.h and ovstage_instancing.h. Every wrapper has two
 * forms. The pair form, `ovstage_<name>_ex`, takes the data-plane instance,
 * `&instance->data_plane`, or the extension instance of its family,
 * `&instance->ext_instances.<family>`. It calls
 * `<pair>->vtable-><member>(<pair>->context, ...)`. A member of a family
 * vtable takes the context of its own pair first, as a data-plane slot takes
 * the data-plane context. The instance form, `ovstage_<name>`, takes the
 * ovstage_instance_t* and calls the pair form on that pair. C++ overloads
 * `ovstage_<name>` for both receivers. The lifecycle functions
 * `ovstage_get_config` and `ovstage_destroy_instance` are exported functions
 * whose bodies call the lifecycle instance with the instance itself, because
 * destroy_instance frees the instance struct. ovstage_ext_vtables.h defines
 * the family vtables and states the rules of the extension instances.
 *
 * `data_plane` is the first member, so the layout `{vtable, context}` of the
 * data-plane instance is a prefix of the instance. An instance of a family is
 * a vtable pointer and a context pointer and never grows: new per-instance
 * state goes behind a context. The context of a family has the type of its
 * family, ovstage_<family>_context_t, so a body cannot take the context of
 * another family without a cast.
 * A new function appends a member to its family vtable. A new family appends
 * a member to ovstage_ext_instances_t. Each such change is a new revision of
 * the library version, the one version of the API. An appended member or
 * family promises no compatibility between a binary of the old revision and
 * a binary of the new one: the consumer, the provider of an instance and the
 * library share one revision. A consumer checks the library version against
 * the headers it compiled with, through ovstage_get_build_info, before it
 * reads an instance or a vtable layout.
 *
 * Instance creation is NOT part of the contract. Each implementation (e.g.
 * ovstage) provides its own factory function, which fills every member.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief The data-plane instance: the public vtable and its context. */
typedef struct ovstage_data_plane_instance_t {
    const ovstage_data_plane_vtable_t* vtable;  /**< The public vtable (see ovstage_api.h). */
    ovstage_data_plane_context_t*      context; /**< The context of the data plane, the first argument of every slot. */
} ovstage_data_plane_instance_t;

/** @brief The lifecycle instance: the lifecycle family vtable and its context. */
typedef struct ovstage_lifecycle_instance_t {
    const ovstage_lifecycle_vtable_t* vtable;
    ovstage_lifecycle_context_t*      context;
} ovstage_lifecycle_instance_t;

/** @brief The hierarchy instance: the hierarchy family vtable and its context. */
typedef struct ovstage_hierarchy_instance_t {
    const ovstage_hierarchy_vtable_t* vtable;
    ovstage_hierarchy_context_t*      context;
} ovstage_hierarchy_instance_t;

/** @brief The population instance: the population family vtable and its context. */
typedef struct ovstage_population_instance_t {
    const ovstage_population_vtable_t* vtable;
    ovstage_population_context_t*      context;
} ovstage_population_instance_t;

/** @brief The usd instance: the usd family vtable and its context. */
typedef struct ovstage_usd_instance_t {
    const ovstage_usd_vtable_t* vtable;
    ovstage_usd_context_t*      context;
} ovstage_usd_instance_t;

/** @brief The instancing instance: the instancing family vtable and its context. */
typedef struct ovstage_instancing_instance_t {
    const ovstage_instancing_vtable_t* vtable;
    ovstage_instancing_context_t*      context;
} ovstage_instancing_instance_t;

/**
 * @brief The extension instances of an instance, one per family.
 *
 * Every member is filled before the instance is public: the library writes
 * the stub pairs and its lifecycle pair, and the factory of the selected
 * implementation writes the pairs it serves. The vtable of a member points at
 * a complete family vtable of ovstage_ext_vtables.h, or at a vtable of stubs
 * for a family the implementation does not serve. The context of a member is
 * the pointer the bodies of that vtable read. A stub reads no context, and
 * its member carries NULL. Whoever writes a member owns its context.
 * ovstage_ext_vtables.h states the rules in full.
 */
typedef struct ovstage_ext_instances_t {
    ovstage_lifecycle_instance_t  lifecycle;
    ovstage_hierarchy_instance_t  hierarchy;
    ovstage_population_instance_t population;
    ovstage_usd_instance_t        usd;
    ovstage_instancing_instance_t instancing;
} ovstage_ext_instances_t;

#ifndef OVSTAGE_INSTANCE_T_DECLARED
#define OVSTAGE_INSTANCE_T_DECLARED
typedef struct ovstage_instance_t {
    ovstage_data_plane_instance_t data_plane;    /**< The data plane: the public vtable and its context. */
    ovstage_ext_instances_t       ext_instances; /**< One instance per family (see ovstage_ext_vtables.h). */
} ovstage_instance_t;
#else
struct ovstage_instance_t {
    ovstage_data_plane_instance_t data_plane;    /**< The data plane: the public vtable and its context. */
    ovstage_ext_instances_t       ext_instances; /**< One instance per family (see ovstage_ext_vtables.h). */
};
#endif

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C++"
{
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_op_id_t)
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_query_handle_t)
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_read_handle_t)
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_map_handle_t)
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_ordinal_query_handle_t)
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_read_group_id_t)
OVX_DEFINE_HANDLE_CPP_OPERATORS(ovstage_query_result_id_t)
}
#endif

#endif /* OVSTAGE_API_TYPES_H */
