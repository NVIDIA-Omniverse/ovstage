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
 * Each wrapper has two forms: the instance form,
 * `ovstage_<slot>(ovstage_instance_t* instance, ...)`, calls the pair form,
 * `ovstage_<slot>_ex(ovstage_data_plane_instance_t* data_plane, ...)`, on
 * `&instance->data_plane`. In C++ the name of the instance form also
 * overloads the pair form over the data-plane instance.
 *
 * The core data-plane slots have wrappers here, including the diagnostics
 * slots (`ovstage_get_version`, `ovstage_get_error_string`,
 * `ovstage_get_last_op_error`) and the resources
 * slot (`ovstage_get_path_dictionary`). Higher-level extension APIs live in
 * sibling headers such as `ovstage_instancing.h`, and the wrappers for the
 * experimental device-placement extension in
 * `experimental/ovstage_placement_extension.h`.
 */

#ifndef OVSTAGE_API_UTILS_H
#define OVSTAGE_API_UTILS_H

#include "ovstage_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────────────────────────────────────────────────────────
 * Op tracking
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_wait_op: the same call on the data-plane
 *         instance, `&instance->data_plane`. C++ overloads ovstage_wait_op
 *         for this receiver. */
static inline ovstage_api_status_t ovstage_wait_op_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_op_id_t                op_id,
    ovstage_timeout_ns_t           timeout,
    ovstage_op_wait_result_t*      out_wait_result)
{
    return data_plane->vtable->wait_op(
        data_plane->context, op_id, timeout, out_wait_result);
}

static inline ovstage_api_status_t ovstage_wait_op(
    ovstage_instance_t*        instance,
    ovstage_op_id_t            op_id,
    ovstage_timeout_ns_t       timeout,
    ovstage_op_wait_result_t*  out_wait_result)
{
    return ovstage_wait_op_ex(
        &instance->data_plane, op_id, timeout, out_wait_result);
}

/** @brief The pair form of ovstage_release_op: the same call on the data-plane
 *         instance, `&instance->data_plane`. C++ overloads ovstage_release_op
 *         for this receiver. */
static inline ovstage_api_status_t ovstage_release_op_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_op_id_t                op_id)
{
    return data_plane->vtable->release_op(data_plane->context, op_id);
}

static inline ovstage_api_status_t ovstage_release_op(
    ovstage_instance_t* instance,
    ovstage_op_id_t     op_id)
{
    return ovstage_release_op_ex(&instance->data_plane, op_id);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Write-floor advance
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_advance_write_floor: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_advance_write_floor for this receiver. */
static inline ovstage_enqueue_result_t ovstage_advance_write_floor_ex(
    ovstage_data_plane_instance_t*    data_plane,
    const ovstage_write_floor_desc_t* desc)
{
    return data_plane->vtable->advance_write_floor(data_plane->context, desc);
}

static inline ovstage_enqueue_result_t ovstage_advance_write_floor(
    ovstage_instance_t*                instance,
    const ovstage_write_floor_desc_t*  desc)
{
    return ovstage_advance_write_floor_ex(&instance->data_plane, desc);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Ordinal queries
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_get_oldest_preserved_ordinal: the same call
 *         on the data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_get_oldest_preserved_ordinal for this receiver. */
static inline ovstage_enqueue_result_t ovstage_get_oldest_preserved_ordinal_ex(
    ovstage_data_plane_instance_t*  data_plane,
    ovstage_ordinal_query_handle_t* out_handle)
{
    return data_plane->vtable->get_oldest_preserved_ordinal(
        data_plane->context, out_handle);
}

static inline ovstage_enqueue_result_t ovstage_get_oldest_preserved_ordinal(
    ovstage_instance_t*             instance,
    ovstage_ordinal_query_handle_t* out_handle)
{
    return ovstage_get_oldest_preserved_ordinal_ex(
        &instance->data_plane, out_handle);
}

/** @brief The pair form of ovstage_get_attribute_write_floor: the same call on
 *         the data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_get_attribute_write_floor for this receiver. */
static inline ovstage_enqueue_result_t ovstage_get_attribute_write_floor_ex(
    ovstage_data_plane_instance_t*  data_plane,
    ovx_token_or_string_t           attribute,
    ovstage_ordinal_query_handle_t* out_handle)
{
    return data_plane->vtable->get_attribute_write_floor(
        data_plane->context, attribute, out_handle);
}

static inline ovstage_enqueue_result_t ovstage_get_attribute_write_floor(
    ovstage_instance_t*             instance,
    ovx_token_or_string_t           attribute,
    ovstage_ordinal_query_handle_t* out_handle)
{
    return ovstage_get_attribute_write_floor_ex(
        &instance->data_plane, attribute, out_handle);
}

/** @brief The pair form of ovstage_fetch_ordinal: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_fetch_ordinal for this receiver. */
static inline ovstage_api_status_t ovstage_fetch_ordinal_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_ordinal_query_handle_t handle,
    ovstage_timeout_ns_t           timeout,
    ovstage_ordinal_t*             out_ordinal)
{
    return data_plane->vtable->fetch_ordinal(
        data_plane->context, handle, timeout, out_ordinal);
}

static inline ovstage_api_status_t ovstage_fetch_ordinal(
    ovstage_instance_t*             instance,
    ovstage_ordinal_query_handle_t  handle,
    ovstage_timeout_ns_t            timeout,
    ovstage_ordinal_t*              out_ordinal)
{
    return ovstage_fetch_ordinal_ex(
        &instance->data_plane, handle, timeout, out_ordinal);
}

/** @brief The pair form of ovstage_release_ordinal_query: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_release_ordinal_query for this receiver. */
static inline ovstage_enqueue_result_t ovstage_release_ordinal_query_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_ordinal_query_handle_t handle)
{
    return data_plane->vtable->release_ordinal_query(data_plane->context, handle);
}

static inline ovstage_enqueue_result_t ovstage_release_ordinal_query(
    ovstage_instance_t*             instance,
    ovstage_ordinal_query_handle_t  handle)
{
    return ovstage_release_ordinal_query_ex(&instance->data_plane, handle);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Query
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_query: the same call on the data-plane
 *         instance, `&instance->data_plane`. C++ overloads ovstage_query for
 *         this receiver. */
static inline ovstage_enqueue_result_t ovstage_query_ex(
    ovstage_data_plane_instance_t* data_plane,
    const ovstage_filter_t*        filter,
    const ovx_token_t*             attrs,
    size_t                         attr_count,
    ovstage_query_handle_t*        out_query_handle)
{
    return data_plane->vtable->query(
        data_plane->context, filter, attrs, attr_count, out_query_handle);
}

static inline ovstage_enqueue_result_t ovstage_query(
    ovstage_instance_t*       instance,
    const ovstage_filter_t*   filter,
    const ovx_token_t*            attrs,
    size_t                        attr_count,
    ovstage_query_handle_t*   out_query_handle)
{
    return ovstage_query_ex(
        &instance->data_plane, filter, attrs, attr_count, out_query_handle);
}

/** @brief The pair form of ovstage_query_from_path_list: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_query_from_path_list for this receiver. */
static inline ovstage_api_status_t ovstage_query_from_path_list_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovx_primpath_list_t            path_list,
    ovstage_query_handle_t*        out_handle)
{
    return data_plane->vtable->query_from_path_list(
        data_plane->context, path_list, out_handle);
}

static inline ovstage_api_status_t ovstage_query_from_path_list(
    ovstage_instance_t*       instance,
    ovx_primpath_list_t           path_list,
    ovstage_query_handle_t*   out_handle)
{
    return ovstage_query_from_path_list_ex(
        &instance->data_plane, path_list, out_handle);
}

/** @brief The pair form of ovstage_fetch_query_result: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_fetch_query_result for this receiver. */
static inline ovstage_api_status_t ovstage_fetch_query_result_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_query_handle_t         query_handle,
    ovstage_timeout_ns_t           timeout,
    ovstage_query_result_t*        out_result)
{
    return data_plane->vtable->fetch_query_result(
        data_plane->context, query_handle, timeout, out_result);
}

static inline ovstage_api_status_t ovstage_fetch_query_result(
    ovstage_instance_t*       instance,
    ovstage_query_handle_t    query_handle,
    ovstage_timeout_ns_t      timeout,
    ovstage_query_result_t*   out_result)
{
    return ovstage_fetch_query_result_ex(
        &instance->data_plane, query_handle, timeout, out_result);
}

/** @brief The pair form of ovstage_release_query_result: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_release_query_result for this receiver. */
static inline ovstage_api_status_t ovstage_release_query_result_ex(
    ovstage_data_plane_instance_t* data_plane,
    const ovstage_query_result_t*  result)
{
    return data_plane->vtable->release_query_result(data_plane->context, result);
}

static inline ovstage_api_status_t ovstage_release_query_result(
    ovstage_instance_t*             instance,
    const ovstage_query_result_t*   result)
{
    return ovstage_release_query_result_ex(&instance->data_plane, result);
}

/** @brief The pair form of ovstage_release_query: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_release_query for this receiver. */
static inline ovstage_enqueue_result_t ovstage_release_query_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_query_handle_t         handle)
{
    return data_plane->vtable->release_query(data_plane->context, handle);
}

static inline ovstage_enqueue_result_t ovstage_release_query(
    ovstage_instance_t*       instance,
    ovstage_query_handle_t    handle)
{
    return ovstage_release_query_ex(&instance->data_plane, handle);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Read
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_read_attributes: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_read_attributes for this receiver. */
static inline ovstage_enqueue_result_t ovstage_read_attributes_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_query_handle_t         handle,
    const ovx_token_t*             attrs,
    size_t                         attr_count,
    ovstage_ordinal_range_t        range,
    ovstage_read_handle_t*         out_read_handle)
{
    return data_plane->vtable->read_attributes(
        data_plane->context, handle, attrs, attr_count, range, out_read_handle);
}

static inline ovstage_enqueue_result_t ovstage_read_attributes(
    ovstage_instance_t*       instance,
    ovstage_query_handle_t    handle,
    const ovx_token_t*            attrs,
    size_t                        attr_count,
    ovstage_ordinal_range_t   range,
    ovstage_read_handle_t*    out_read_handle)
{
    return ovstage_read_attributes_ex(
        &instance->data_plane, handle, attrs, attr_count, range, out_read_handle);
}

/** @brief The pair form of ovstage_fetch_read_next: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_fetch_read_next for this receiver. */
static inline ovstage_api_status_t ovstage_fetch_read_next_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_read_handle_t read_handle,
    ovstage_timeout_ns_t timeout,
    ovstage_read_group_t* out_groups,
    size_t capacity,
    size_t* out_count)
{
    return data_plane->vtable->fetch_read_next(
        data_plane->context, read_handle, timeout, out_groups, capacity, out_count);
}

static inline ovstage_api_status_t ovstage_fetch_read_next(
    ovstage_instance_t* instance,
    ovstage_read_handle_t read_handle,
    ovstage_timeout_ns_t timeout,
    ovstage_read_group_t* out_groups,
    size_t capacity,
    size_t* out_count)
{
    return ovstage_fetch_read_next_ex(
        &instance->data_plane, read_handle, timeout, out_groups, capacity, out_count);
}

/** @brief The pair form of ovstage_release_groups: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_release_groups for this receiver. */
static inline ovstage_api_status_t ovstage_release_groups_ex(
    ovstage_data_plane_instance_t* data_plane,
    const ovstage_read_group_t* groups,
    size_t group_count)
{
    return data_plane->vtable->release_groups(data_plane->context, groups, group_count);
}

static inline ovstage_api_status_t ovstage_release_groups(
    ovstage_instance_t* instance,
    const ovstage_read_group_t* groups,
    size_t group_count)
{
    return ovstage_release_groups_ex(&instance->data_plane, groups, group_count);
}

/** @brief The pair form of ovstage_release_read: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_release_read for this receiver. */
static inline ovstage_enqueue_result_t ovstage_release_read_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_read_handle_t          read_handle)
{
    return data_plane->vtable->release_read(data_plane->context, read_handle);
}

static inline ovstage_enqueue_result_t ovstage_release_read(
    ovstage_instance_t*       instance,
    ovstage_read_handle_t     read_handle)
{
    return ovstage_release_read_ex(&instance->data_plane, read_handle);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Write copy-in
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_write_attribute: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_write_attribute for this receiver. */
static inline ovstage_enqueue_result_t ovstage_write_attribute_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_query_handle_t         handle,
    ovx_token_or_string_t          attribute,
    ovstage_ordinal_t              ordinal,
    ovstage_write_data_t           data,
    ovstage_prim_mode_t            prim_mode)
{
    return data_plane->vtable->write_attribute(
        data_plane->context, handle, attribute, ordinal, data, prim_mode);
}

static inline ovstage_enqueue_result_t ovstage_write_attribute(
    ovstage_instance_t*           instance,
    ovstage_query_handle_t        handle,
    ovx_token_or_string_t             attribute,
    ovstage_ordinal_t             ordinal,
    ovstage_write_data_t          data,
    ovstage_prim_mode_t           prim_mode)
{
    return ovstage_write_attribute_ex(
        &instance->data_plane, handle, attribute, ordinal, data, prim_mode);
}

/** @brief The pair form of ovstage_write_attributes: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_write_attributes for this receiver. */
static inline ovstage_enqueue_result_t ovstage_write_attributes_ex(
    ovstage_data_plane_instance_t*   data_plane,
    ovstage_query_handle_t           handle,
    const ovstage_attribute_write_t* writes,
    size_t                           write_count,
    ovstage_ordinal_t                ordinal,
    ovstage_prim_mode_t              prim_mode)
{
    return data_plane->vtable->write_attributes(
        data_plane->context, handle, writes, write_count, ordinal, prim_mode);
}

static inline ovstage_enqueue_result_t ovstage_write_attributes(
    ovstage_instance_t*                 instance,
    ovstage_query_handle_t              handle,
    const ovstage_attribute_write_t*    writes,
    size_t                              write_count,
    ovstage_ordinal_t                   ordinal,
    ovstage_prim_mode_t                 prim_mode)
{
    return ovstage_write_attributes_ex(
        &instance->data_plane, handle, writes, write_count, ordinal, prim_mode);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Map / unmap writable staging buffers
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_map_attribute: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_map_attribute for this receiver. */
static inline ovstage_enqueue_result_t ovstage_map_attribute_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_query_handle_t         handle,
    const ovstage_map_desc_t*      desc,
    ovstage_ordinal_t              ordinal,
    const size_t*                  element_sizes,
    size_t                         element_count,
    ovstage_map_handle_t*          out_map_handle)
{
    return data_plane->vtable->map_attribute(
        data_plane->context, handle, desc, ordinal,
        element_sizes, element_count, out_map_handle);
}

static inline ovstage_enqueue_result_t ovstage_map_attribute(
    ovstage_instance_t*           instance,
    ovstage_query_handle_t        handle,
    const ovstage_map_desc_t*     desc,
    ovstage_ordinal_t             ordinal,
    const size_t*                     element_sizes,
    size_t                            element_count,
    ovstage_map_handle_t*         out_map_handle)
{
    return ovstage_map_attribute_ex(
        &instance->data_plane, handle, desc, ordinal,
        element_sizes, element_count, out_map_handle);
}

/** @brief The pair form of ovstage_fetch_map_next: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_fetch_map_next for this receiver. */
static inline ovstage_api_status_t ovstage_fetch_map_next_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_map_handle_t           map_handle,
    ovstage_timeout_ns_t           timeout,
    ovstage_map_group_t*           out_group)
{
    return data_plane->vtable->fetch_map_next(
        data_plane->context, map_handle, timeout, out_group);
}

static inline ovstage_api_status_t ovstage_fetch_map_next(
    ovstage_instance_t*       instance,
    ovstage_map_handle_t      map_handle,
    ovstage_timeout_ns_t      timeout,
    ovstage_map_group_t*      out_group)
{
    return ovstage_fetch_map_next_ex(
        &instance->data_plane, map_handle, timeout, out_group);
}

/** @brief The pair form of ovstage_unmap_group: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_unmap_group for this receiver. */
static inline ovstage_enqueue_result_t ovstage_unmap_group_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_map_handle_t           map_handle,
    const ovstage_map_group_t*     group,
    ovstage_cuda_sync_t            write_done_sync)
{
    return data_plane->vtable->unmap_group(
        data_plane->context, map_handle, group, write_done_sync);
}

static inline ovstage_enqueue_result_t ovstage_unmap_group(
    ovstage_instance_t*           instance,
    ovstage_map_handle_t          map_handle,
    const ovstage_map_group_t*    group,
    ovstage_cuda_sync_t           write_done_sync)
{
    return ovstage_unmap_group_ex(
        &instance->data_plane, map_handle, group, write_done_sync);
}

/** @brief The pair form of ovstage_unmap_attribute: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_unmap_attribute for this receiver. */
static inline ovstage_enqueue_result_t ovstage_unmap_attribute_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_map_handle_t           map_handle,
    ovstage_cuda_sync_t            write_done_sync)
{
    return data_plane->vtable->unmap_attribute(
        data_plane->context, map_handle, write_done_sync);
}

static inline ovstage_enqueue_result_t ovstage_unmap_attribute(
    ovstage_instance_t*       instance,
    ovstage_map_handle_t      map_handle,
    ovstage_cuda_sync_t       write_done_sync)
{
    return ovstage_unmap_attribute_ex(
        &instance->data_plane, map_handle, write_done_sync);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Delete
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_delete_attributes: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_delete_attributes for this receiver. */
static inline ovstage_enqueue_result_t ovstage_delete_attributes_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_query_handle_t         handle,
    const ovx_token_or_string_t*   attributes,
    size_t                         attribute_count,
    ovstage_ordinal_t              ordinal)
{
    return data_plane->vtable->delete_attributes(
        data_plane->context, handle, attributes, attribute_count, ordinal);
}

static inline ovstage_enqueue_result_t ovstage_delete_attributes(
    ovstage_instance_t*           instance,
    ovstage_query_handle_t        handle,
    const ovx_token_or_string_t*      attributes,
    size_t                            attribute_count,
    ovstage_ordinal_t             ordinal)
{
    return ovstage_delete_attributes_ex(
        &instance->data_plane, handle, attributes, attribute_count, ordinal);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Diagnostics
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_get_version: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_get_version for this receiver. */
static inline void ovstage_get_version_ex(
    ovstage_data_plane_instance_t* data_plane,
    uint32_t*                      out_major,
    uint32_t*                      out_minor,
    uint32_t*                      out_patch)
{
    data_plane->vtable->get_version(
        data_plane->context, out_major, out_minor, out_patch);
}

static inline void ovstage_get_version(
    ovstage_instance_t* instance,
    uint32_t*               out_major,
    uint32_t*               out_minor,
    uint32_t*               out_patch)
{
    ovstage_get_version_ex(&instance->data_plane, out_major, out_minor, out_patch);
}

/** @brief The pair form of ovstage_get_error_string: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_get_error_string for this receiver. */
static inline const char* ovstage_get_error_string_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_api_status_t           error)
{
    return data_plane->vtable->get_error_string(data_plane->context, error);
}

static inline const char* ovstage_get_error_string(
    ovstage_instance_t* instance,
    ovstage_api_status_t     error)
{
    return ovstage_get_error_string_ex(&instance->data_plane, error);
}

/** @brief The pair form of ovstage_get_last_op_error: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_get_last_op_error for this receiver. */
static inline ovx_string_t ovstage_get_last_op_error_ex(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_op_id_t                op_id)
{
    return data_plane->vtable->get_last_op_error(data_plane->context, op_id);
}

static inline ovx_string_t ovstage_get_last_op_error(
    ovstage_instance_t* instance,
    ovstage_op_id_t     op_id)
{
    return ovstage_get_last_op_error_ex(&instance->data_plane, op_id);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Resources
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_get_path_dictionary: the same call on the
 *         data-plane instance, `&instance->data_plane`. A NULL data_plane
 *         gives NULL. C++ overloads ovstage_get_path_dictionary for this
 *         receiver. */
static inline path_dictionary_instance_t* ovstage_get_path_dictionary_ex(
    ovstage_data_plane_instance_t* data_plane)
{
    if (!data_plane)
        return NULL;
    return data_plane->vtable->get_path_dictionary(data_plane->context);
}

static inline path_dictionary_instance_t* ovstage_get_path_dictionary(
    ovstage_instance_t* instance)
{
    return ovstage_get_path_dictionary_ex(
        instance != NULL ? &instance->data_plane : NULL);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Extensions
 * ─────────────────────────────────────────────────────────────────────────── */

/** @brief The pair form of ovstage_query_extension: the same call on the
 *         data-plane instance, `&instance->data_plane`. C++ overloads
 *         ovstage_query_extension for this receiver. */
static inline ovstage_api_status_t ovstage_query_extension_ex(
    ovstage_data_plane_instance_t* data_plane,
    const char*                    name,
    const void**                   out_extension)
{
    return data_plane->vtable->query_extension(
        data_plane->context, name, out_extension);
}

static inline ovstage_api_status_t ovstage_query_extension(
    ovstage_instance_t* instance,
    const char*         name,
    const void**        out_extension)
{
    return ovstage_query_extension_ex(&instance->data_plane, name, out_extension);
}

#ifdef __cplusplus
} /* extern "C" */

extern "C++"
{
/* C++ spells the pair form with the name of the instance form. A literal NULL or
 * nullptr first argument is ambiguous between the two receivers: pass a
 * typed null, or call the _ex form. */
inline ovstage_api_status_t ovstage_wait_op(
    ovstage_data_plane_instance_t* data_plane, ovstage_op_id_t op_id,
    ovstage_timeout_ns_t timeout, ovstage_op_wait_result_t* out_wait_result)
{
    return ovstage_wait_op_ex(data_plane, op_id, timeout, out_wait_result);
}

inline ovstage_api_status_t ovstage_release_op(
    ovstage_data_plane_instance_t* data_plane, ovstage_op_id_t op_id)
{
    return ovstage_release_op_ex(data_plane, op_id);
}

inline ovstage_enqueue_result_t ovstage_advance_write_floor(
    ovstage_data_plane_instance_t* data_plane,
    const ovstage_write_floor_desc_t* desc)
{
    return ovstage_advance_write_floor_ex(data_plane, desc);
}

inline ovstage_enqueue_result_t ovstage_get_oldest_preserved_ordinal(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_ordinal_query_handle_t* out_handle)
{
    return ovstage_get_oldest_preserved_ordinal_ex(data_plane, out_handle);
}

inline ovstage_enqueue_result_t ovstage_get_attribute_write_floor(
    ovstage_data_plane_instance_t* data_plane, ovx_token_or_string_t attribute,
    ovstage_ordinal_query_handle_t* out_handle)
{
    return ovstage_get_attribute_write_floor_ex(data_plane, attribute, out_handle);
}

inline ovstage_api_status_t ovstage_fetch_ordinal(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_ordinal_query_handle_t handle, ovstage_timeout_ns_t timeout,
    ovstage_ordinal_t* out_ordinal)
{
    return ovstage_fetch_ordinal_ex(data_plane, handle, timeout, out_ordinal);
}

inline ovstage_enqueue_result_t ovstage_release_ordinal_query(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_ordinal_query_handle_t handle)
{
    return ovstage_release_ordinal_query_ex(data_plane, handle);
}

inline ovstage_enqueue_result_t ovstage_query(
    ovstage_data_plane_instance_t* data_plane, const ovstage_filter_t* filter,
    const ovx_token_t* attrs, size_t attr_count,
    ovstage_query_handle_t* out_query_handle)
{
    return ovstage_query_ex(data_plane, filter, attrs, attr_count, out_query_handle);
}

inline ovstage_api_status_t ovstage_query_from_path_list(
    ovstage_data_plane_instance_t* data_plane, ovx_primpath_list_t path_list,
    ovstage_query_handle_t* out_handle)
{
    return ovstage_query_from_path_list_ex(data_plane, path_list, out_handle);
}

inline ovstage_api_status_t ovstage_fetch_query_result(
    ovstage_data_plane_instance_t* data_plane, ovstage_query_handle_t query_handle,
    ovstage_timeout_ns_t timeout, ovstage_query_result_t* out_result)
{
    return ovstage_fetch_query_result_ex(data_plane, query_handle, timeout, out_result);
}

inline ovstage_api_status_t ovstage_release_query_result(
    ovstage_data_plane_instance_t* data_plane, const ovstage_query_result_t* result)
{
    return ovstage_release_query_result_ex(data_plane, result);
}

inline ovstage_enqueue_result_t ovstage_release_query(
    ovstage_data_plane_instance_t* data_plane, ovstage_query_handle_t handle)
{
    return ovstage_release_query_ex(data_plane, handle);
}

inline ovstage_enqueue_result_t ovstage_read_attributes(
    ovstage_data_plane_instance_t* data_plane, ovstage_query_handle_t handle,
    const ovx_token_t* attrs, size_t attr_count, ovstage_ordinal_range_t range,
    ovstage_read_handle_t* out_read_handle)
{
    return ovstage_read_attributes_ex(
        data_plane, handle, attrs, attr_count, range, out_read_handle);
}

inline ovstage_api_status_t ovstage_fetch_read_next(
    ovstage_data_plane_instance_t* data_plane,
    ovstage_read_handle_t read_handle,
    ovstage_timeout_ns_t timeout,
    ovstage_read_group_t* out_groups,
    size_t capacity,
    size_t* out_count)
{
    return ovstage_fetch_read_next_ex(
        data_plane, read_handle, timeout, out_groups, capacity, out_count);
}

inline ovstage_api_status_t ovstage_release_groups(
    ovstage_data_plane_instance_t* data_plane,
    const ovstage_read_group_t* groups,
    size_t group_count)
{
    return ovstage_release_groups_ex(data_plane, groups, group_count);
}

inline ovstage_enqueue_result_t ovstage_release_read(
    ovstage_data_plane_instance_t* data_plane, ovstage_read_handle_t read_handle)
{
    return ovstage_release_read_ex(data_plane, read_handle);
}

inline ovstage_enqueue_result_t ovstage_write_attribute(
    ovstage_data_plane_instance_t* data_plane, ovstage_query_handle_t handle,
    ovx_token_or_string_t attribute, ovstage_ordinal_t ordinal,
    ovstage_write_data_t data, ovstage_prim_mode_t prim_mode)
{
    return ovstage_write_attribute_ex(
        data_plane, handle, attribute, ordinal, data, prim_mode);
}

inline ovstage_enqueue_result_t ovstage_write_attributes(
    ovstage_data_plane_instance_t* data_plane, ovstage_query_handle_t handle,
    const ovstage_attribute_write_t* writes, size_t write_count,
    ovstage_ordinal_t ordinal, ovstage_prim_mode_t prim_mode)
{
    return ovstage_write_attributes_ex(
        data_plane, handle, writes, write_count, ordinal, prim_mode);
}

inline ovstage_enqueue_result_t ovstage_map_attribute(
    ovstage_data_plane_instance_t* data_plane, ovstage_query_handle_t handle,
    const ovstage_map_desc_t* desc, ovstage_ordinal_t ordinal,
    const size_t* element_sizes, size_t element_count,
    ovstage_map_handle_t* out_map_handle)
{
    return ovstage_map_attribute_ex(
        data_plane, handle, desc, ordinal, element_sizes, element_count, out_map_handle);
}

inline ovstage_api_status_t ovstage_fetch_map_next(
    ovstage_data_plane_instance_t* data_plane, ovstage_map_handle_t map_handle,
    ovstage_timeout_ns_t timeout, ovstage_map_group_t* out_group)
{
    return ovstage_fetch_map_next_ex(data_plane, map_handle, timeout, out_group);
}

inline ovstage_enqueue_result_t ovstage_unmap_group(
    ovstage_data_plane_instance_t* data_plane, ovstage_map_handle_t map_handle,
    const ovstage_map_group_t* group, ovstage_cuda_sync_t write_done_sync)
{
    return ovstage_unmap_group_ex(data_plane, map_handle, group, write_done_sync);
}

inline ovstage_enqueue_result_t ovstage_unmap_attribute(
    ovstage_data_plane_instance_t* data_plane, ovstage_map_handle_t map_handle,
    ovstage_cuda_sync_t write_done_sync)
{
    return ovstage_unmap_attribute_ex(data_plane, map_handle, write_done_sync);
}

inline ovstage_enqueue_result_t ovstage_delete_attributes(
    ovstage_data_plane_instance_t* data_plane, ovstage_query_handle_t handle,
    const ovx_token_or_string_t* attributes, size_t attribute_count,
    ovstage_ordinal_t ordinal)
{
    return ovstage_delete_attributes_ex(
        data_plane, handle, attributes, attribute_count, ordinal);
}

inline void ovstage_get_version(
    ovstage_data_plane_instance_t* data_plane, uint32_t* out_major,
    uint32_t* out_minor, uint32_t* out_patch)
{
    ovstage_get_version_ex(data_plane, out_major, out_minor, out_patch);
}

inline const char* ovstage_get_error_string(
    ovstage_data_plane_instance_t* data_plane, ovstage_api_status_t error)
{
    return ovstage_get_error_string_ex(data_plane, error);
}

inline ovx_string_t ovstage_get_last_op_error(
    ovstage_data_plane_instance_t* data_plane, ovstage_op_id_t op_id)
{
    return ovstage_get_last_op_error_ex(data_plane, op_id);
}

inline path_dictionary_instance_t* ovstage_get_path_dictionary(
    ovstage_data_plane_instance_t* data_plane)
{
    return ovstage_get_path_dictionary_ex(data_plane);
}

inline ovstage_api_status_t ovstage_query_extension(
    ovstage_data_plane_instance_t* data_plane, const char* name,
    const void** out_extension)
{
    return ovstage_query_extension_ex(data_plane, name, out_extension);
}
}
#endif

#endif /* OVSTAGE_API_UTILS_H */
