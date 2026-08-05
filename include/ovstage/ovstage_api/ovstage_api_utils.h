/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage_api_utils.h
 * @brief Convenience `static inline` wrappers around the core ovstage_api slots.
 *
 * @details
 * This header is included by `ovstage_api.h` after the vtable struct is
 * defined. Callers should `#include "ovstage_api.h"` and then call the
 * wrappers directly:
 *
 *     ovstage_query_handle_t qh;
 *     ovstage_enqueue_result_t er = ovstage_query(instance, &filter,
 *                                                 NULL, 0, &qh);
 *
 * The core data-plane slots have wrappers here, including the diagnostics
 * slots (`ovstage_get_version`, `ovstage_get_error_string`,
 * `ovstage_get_last_op_error`) and the resources
 * slot (`ovstage_get_path_dictionary`). Higher-level extension APIs live in
 * sibling headers such as `ovstage_instancing.h`.
 */

#ifndef OVSTAGE_API_UTILS_H
#define OVSTAGE_API_UTILS_H

#include "ovstage_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────────────────────────────────────────────────────────
 * Op tracking
 * ─────────────────────────────────────────────────────────────────────────── */

static inline ovstage_api_status_t ovstage_wait_op(
    ovstage_instance_t*        instance,
    ovstage_op_id_t            op_id,
    ovstage_timeout_ns_t       timeout,
    ovstage_op_wait_result_t*  out_wait_result)
{
    return instance->vtable->wait_op(
        instance->context, op_id, timeout, out_wait_result);
}

static inline ovstage_api_status_t ovstage_release_op(
    ovstage_instance_t* instance,
    ovstage_op_id_t     op_id)
{
    return instance->vtable->release_op(
        instance->context, op_id);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Write-floor advance
 * ─────────────────────────────────────────────────────────────────────────── */

static inline ovstage_enqueue_result_t ovstage_advance_write_floor(
    ovstage_instance_t*                instance,
    const ovstage_write_floor_desc_t*  desc)
{
    return instance->vtable->advance_write_floor(
        instance->context, desc);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Ordinal queries
 * ─────────────────────────────────────────────────────────────────────────── */

static inline ovstage_enqueue_result_t ovstage_get_oldest_preserved_ordinal(
    ovstage_instance_t*             instance,
    ovstage_ordinal_query_handle_t* out_handle)
{
    return instance->vtable->get_oldest_preserved_ordinal(
        instance->context, out_handle);
}

static inline ovstage_enqueue_result_t ovstage_get_attribute_write_floor(
    ovstage_instance_t*             instance,
    ovx_string_or_token_t           attribute,
    ovstage_ordinal_query_handle_t* out_handle)
{
    return instance->vtable->get_attribute_write_floor(
        instance->context, attribute, out_handle);
}

static inline ovstage_api_status_t ovstage_fetch_ordinal(
    ovstage_instance_t*             instance,
    ovstage_ordinal_query_handle_t  handle,
    ovstage_timeout_ns_t            timeout,
    ovstage_ordinal_t*              out_ordinal)
{
    return instance->vtable->fetch_ordinal(
        instance->context, handle, timeout, out_ordinal);
}

static inline ovstage_enqueue_result_t ovstage_release_ordinal_query(
    ovstage_instance_t*             instance,
    ovstage_ordinal_query_handle_t  handle)
{
    return instance->vtable->release_ordinal_query(
        instance->context, handle);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Query
 * ─────────────────────────────────────────────────────────────────────────── */

static inline ovstage_enqueue_result_t ovstage_query(
    ovstage_instance_t*       instance,
    const ovstage_filter_t*   filter,
    const ovx_token_t*            attrs,
    size_t                        attr_count,
    ovstage_query_handle_t*   out_query_handle)
{
    return instance->vtable->query(
        instance->context, filter, attrs, attr_count, out_query_handle);
}

static inline ovstage_api_status_t ovstage_query_from_path_list(
    ovstage_instance_t*       instance,
    ovx_primpath_list_t           path_list,
    ovstage_query_handle_t*   out_handle)
{
    return instance->vtable->query_from_path_list(
        instance->context, path_list, out_handle);
}

static inline ovstage_api_status_t ovstage_fetch_query_result(
    ovstage_instance_t*       instance,
    ovstage_query_handle_t    query_handle,
    ovstage_timeout_ns_t      timeout,
    ovstage_query_result_t*   out_result)
{
    return instance->vtable->fetch_query_result(
        instance->context, query_handle, timeout, out_result);
}

static inline ovstage_api_status_t ovstage_release_query_result(
    ovstage_instance_t*             instance,
    const ovstage_query_result_t*   result)
{
    return instance->vtable->release_query_result(
        instance->context, result);
}

static inline ovstage_enqueue_result_t ovstage_release_query(
    ovstage_instance_t*       instance,
    ovstage_query_handle_t    handle)
{
    return instance->vtable->release_query(
        instance->context, handle);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Read
 * ─────────────────────────────────────────────────────────────────────────── */

static inline ovstage_enqueue_result_t ovstage_read_attributes(
    ovstage_instance_t*       instance,
    ovstage_query_handle_t    handle,
    const ovx_token_t*            attrs,
    size_t                        attr_count,
    ovstage_ordinal_range_t   range,
    ovstage_read_handle_t*    out_read_handle)
{
    return instance->vtable->read_attributes(
        instance->context, handle, attrs, attr_count, range, out_read_handle);
}

static inline ovstage_api_status_t ovstage_fetch_read_next(
    ovstage_instance_t*       instance,
    ovstage_read_handle_t     read_handle,
    ovstage_timeout_ns_t      timeout,
    ovstage_read_group_t*     out_group)
{
    return instance->vtable->fetch_read_next(
        instance->context, read_handle, timeout, out_group);
}

static inline ovstage_api_status_t ovstage_release_group(
    ovstage_instance_t*           instance,
    const ovstage_read_group_t*   group)
{
    return instance->vtable->release_group(
        instance->context, group);
}

static inline ovstage_enqueue_result_t ovstage_release_read(
    ovstage_instance_t*       instance,
    ovstage_read_handle_t     read_handle)
{
    return instance->vtable->release_read(
        instance->context, read_handle);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Write copy-in
 * ─────────────────────────────────────────────────────────────────────────── */

static inline ovstage_enqueue_result_t ovstage_write_attribute(
    ovstage_instance_t*           instance,
    ovstage_query_handle_t        handle,
    ovx_string_or_token_t             attribute,
    ovstage_ordinal_t             ordinal,
    ovstage_write_data_t          data,
    ovstage_prim_mode_t           prim_mode)
{
    return instance->vtable->write_attribute(
        instance->context, handle, attribute, ordinal, data, prim_mode);
}

static inline ovstage_enqueue_result_t ovstage_write_attributes(
    ovstage_instance_t*                 instance,
    ovstage_query_handle_t              handle,
    const ovstage_attribute_write_t*    writes,
    size_t                              write_count,
    ovstage_ordinal_t                   ordinal,
    ovstage_prim_mode_t                 prim_mode)
{
    return instance->vtable->write_attributes(
        instance->context, handle, writes, write_count, ordinal, prim_mode);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Map / unmap zero-copy write
 * ─────────────────────────────────────────────────────────────────────────── */

static inline ovstage_enqueue_result_t ovstage_map_attribute(
    ovstage_instance_t*           instance,
    ovstage_query_handle_t        handle,
    const ovstage_map_desc_t*     desc,
    ovstage_ordinal_t             ordinal,
    const size_t*                     element_sizes,
    size_t                            element_count,
    ovstage_map_handle_t*         out_map_handle)
{
    return instance->vtable->map_attribute(
        instance->context, handle, desc, ordinal,
        element_sizes, element_count, out_map_handle);
}

static inline ovstage_api_status_t ovstage_fetch_map_next(
    ovstage_instance_t*       instance,
    ovstage_map_handle_t      map_handle,
    ovstage_timeout_ns_t      timeout,
    ovstage_map_group_t*      out_group)
{
    return instance->vtable->fetch_map_next(
        instance->context, map_handle, timeout, out_group);
}

static inline ovstage_enqueue_result_t ovstage_unmap_group(
    ovstage_instance_t*           instance,
    ovstage_map_handle_t          map_handle,
    const ovstage_map_group_t*    group,
    ovstage_cuda_sync_t           write_done_sync)
{
    return instance->vtable->unmap_group(
        instance->context, map_handle, group, write_done_sync);
}

static inline ovstage_enqueue_result_t ovstage_unmap_attribute(
    ovstage_instance_t*       instance,
    ovstage_map_handle_t      map_handle,
    ovstage_cuda_sync_t       write_done_sync)
{
    return instance->vtable->unmap_attribute(
        instance->context, map_handle, write_done_sync);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Delete
 * ─────────────────────────────────────────────────────────────────────────── */

static inline ovstage_enqueue_result_t ovstage_delete_attributes(
    ovstage_instance_t*           instance,
    ovstage_query_handle_t        handle,
    const ovx_string_or_token_t*      attributes,
    size_t                            attribute_count,
    ovstage_ordinal_t             ordinal)
{
    return instance->vtable->delete_attributes(
        instance->context, handle, attributes, attribute_count, ordinal);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Diagnostics
 * ─────────────────────────────────────────────────────────────────────────── */

static inline void ovstage_get_version(
    ovstage_instance_t* instance,
    uint32_t*               out_major,
    uint32_t*               out_minor,
    uint32_t*               out_patch)
{
    instance->vtable->get_version(
        instance->context, out_major, out_minor, out_patch);
}

static inline const char* ovstage_get_error_string(
    ovstage_instance_t* instance,
    ovstage_api_status_t     error)
{
    return instance->vtable->get_error_string(
        instance->context, error);
}

static inline ovx_string_t ovstage_get_last_op_error(
    ovstage_instance_t* instance,
    ovstage_op_id_t     op_id)
{
    return instance->vtable->get_last_op_error(
        instance->context, op_id);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Resources
 * ─────────────────────────────────────────────────────────────────────────── */

static inline path_dictionary_instance_t* ovstage_get_path_dictionary(
    ovstage_instance_t* instance)
{
    if (!instance)
        return NULL;
    return instance->vtable->get_path_dictionary(
        instance->context);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Extensions
 * ─────────────────────────────────────────────────────────────────────────── */

static inline ovstage_api_status_t ovstage_query_extension(
    ovstage_instance_t* instance,
    const char*         name,
    const void**        out_extension)
{
    return instance->vtable->query_extension(
        instance->context, name, out_extension);
}

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_API_UTILS_H */
