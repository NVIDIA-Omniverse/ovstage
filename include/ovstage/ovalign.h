/**
 * @file ovalign.h
 * @brief OVAlign — composable cross-attribute group alignment library.
 *
 * @details
 * OVAlign normalizes the group decomposition across multiple attributes so that
 * consumers can zip-iterate attributes for the same prim in a single loop.
 *
 * ## When to Use
 *
 * Alignment is ONLY needed when attribute values must be zipped for processing
 * (e.g., computing view-projection requires world_xform + fov + near/far for the
 * same camera in one computation).
 *
 * When processing attributes independently, alignment is unnecessary — each
 * attribute's groups are self-consistent.
 *
 * ## Design Principles
 *
 * - **Same type in, same type out:** Input and output both use ovstage_read_group_t.
 *   Consumer code works unchanged after inserting ovalign_align().
 * - **Zero-copy fast path:** When already aligned, skipped=true and pointers
 *   reference original data (no allocation).
 * - **GPU-native:** When device=CUDA, intersection and gather run as GPU kernels.
 * - **No ovstage runtime dependency:** Operates purely on struct data. No instance needed.
 *
 * ## Post-Alignment Invariant
 *
 * All attributes have the same group_count with matching prim structure per group:
 * - Same prims.offset, prims.count, prims.index_map across all attrs within a group index.
 * - data (tensors, count, index_map, mask) remains per-attribute (different storage density).
 *
 * @version 0.1.0
 * @date 2026-05-23
 */

#ifndef OVALIGN_H
#define OVALIGN_H

#include <stddef.h>
#include <stdbool.h>
#include "ovstage.h"  /* ovstage_read_group_t, ovx_token_t, ovx_primpath_list_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════════
 * Types
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Target device for alignment computation.
 */
typedef enum {
    OVALIGN_DEVICE_CPU  = 0,  /**< Alignment runs on CPU. */
    OVALIGN_DEVICE_CUDA = 1,  /**< Alignment runs on GPU (intersection + gather as kernels). */
} ovalign_device_t;

/**
 * @brief Alignment request — which groups to align, for which attributes.
 *
 * Input uses ovstage_read_group_t (the same struct ovstage_fetch_read_next produces).
 * Groups may be from multiple attributes, mixed together.
 */
typedef struct {
    const ovstage_read_group_t* groups;           /**< All input groups (mixed attributes). */
    size_t                      group_count;      /**< Total input group count. */
    const ovx_token_t*          attributes;       /**< Which attribute tokens to align. */
    size_t                      attr_count;       /**< Number of attributes. */
    ovx_primpath_list_t         prim_list;        /**< The query's prim list (groups index into this). */
    const uint32_t*             prim_order;       /**< NULL = preserve natural order; non-NULL = reorder. */
    size_t                      prim_order_count; /**< Length of prim_order (ignored if NULL). */
    ovalign_device_t            device;           /**< Where to run alignment computation. */
} ovalign_request_t;

/**
 * @brief Per-attribute result after alignment.
 *
 * Contains an array of ovstage_read_group_t — same type as ovstage produces.
 * Consumer code that processes groups works unchanged.
 */
typedef struct {
    const ovstage_read_group_t* groups;      /**< Aligned groups for this attribute. [group_count] */
    size_t                      group_count; /**< Same for all attrs in result (uniform). */
} ovalign_attr_result_t;

/**
 * @brief Alignment result — per-attribute group arrays + metadata.
 *
 * @invariant All attrs have the same group_count (uniform decomposition).
 * @invariant Within each group index g:
 *   result.attrs[a].groups[g].prims.offset == result.attrs[b].groups[g].prims.offset
 *   result.attrs[a].groups[g].prims.count  == result.attrs[b].groups[g].prims.count
 *   (prim structure matches; data/mask/index_map are per-attribute)
 */
typedef struct {
    const ovalign_attr_result_t* attrs;       /**< Per-attribute results. [attr_count] */
    size_t                       attr_count;  /**< Number of attributes. */
    size_t                       group_count; /**< Uniform group count across all attrs. */
    bool                         skipped;     /**< true = already aligned, zero-copy (no alloc). */
} ovalign_result_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Functions
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Align groups across attributes into a uniform decomposition.
 *
 * After alignment, all attributes have matching prim structure per group index,
 * enabling direct zip-iteration without index translation.
 *
 * @param request Alignment request (input groups + configuration).
 * @param[out] out_result Receives aligned result.
 * @return 0 on success, non-zero on error.
 *
 * @post If already aligned: out_result.skipped=true, group pointers reference
 *       original input data (zero-copy).
 * @post If alignment needed: out_result contains newly allocated groups with
 *       gathered/intersected data.
 * @post Caller must call ovalign_release(out_result) when done.
 *
 * @note No ovstage instance required — operates purely on data structs.
 *
 * Behavior by input state:
 * | Input state                    | Action                        | Cost                    |
 * |-------------------------------|-------------------------------|-------------------------|
 * | Same decomposition            | No-op, skipped=true           | O(groups × attrs) cmp  |
 * | Different group counts/shapes | Intersect + gather            | One alloc + gather/attr |
 * | prim_order provided           | Reorder into specified order  | One alloc + gather/attr |
 * | prim_order + already ordered  | No-op, skipped=true           | O(prims) comparison    |
 */
int ovalign_align(
    const ovalign_request_t* request,
    ovalign_result_t*        out_result);

/**
 * @brief Release memory allocated by ovalign_align.
 *
 * Safe to call on results where skipped=true (no-op in that case).
 * After release, all pointers in the result are invalid.
 *
 * @param result Result to release. May be NULL (no-op).
 */
void ovalign_release(ovalign_result_t* result);

#ifdef __cplusplus
}
#endif

#endif /* OVALIGN_H */
