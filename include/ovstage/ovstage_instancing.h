/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * ovstage instancing: high-level scene-graph-instancing queries.
 *
 * These functions query the latest committed data through an ovstage instance.
 * Path-list results are freshly allocated through the
 * instance path dictionary with refcount = 1; callers release them via
 * path_dictionary_release_path_list_reference(ovstage_get_path_dictionary(instance), list).
 * Individual ovx_primpath_t values are dictionary-lifetime handles and do not
 * require per-result release.
 */

#ifndef OVSTAGE_INSTANCING_H
#define OVSTAGE_INSTANCING_H

#include "ovstage_api/ovstage_api.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @defgroup ovstage_instancing Instancing queries
     *  @{
     */

    /**
     * Return all instance-root prim paths that reference @p prototype_root.
     *
     * @param instance             ovstage instance.
     * @param prototype_root       Prototype-root prim path.
     * @param out_instance_roots   Receives a path-list handle. The list may be
     *                             empty when no instance roots reference
     *                             @p prototype_root.
     * @return OVSTAGE_OK on success.
     */
    ovstage_api_status_t ovstage_instancing_get_instance_roots(
        ovstage_instance_t*   instance,
        ovx_primpath_t       prototype_root,
        ovx_primpath_list_t* out_instance_roots);

    /**
     * Return the prototype root referenced by @p instance_root.
     *
     * @param instance             ovstage instance.
     * @param instance_root        Instance-root prim path.
     * @param out_prototype_root   Receives the prototype-root prim path.
     * @return OVSTAGE_OK on success, OVSTAGE_ERROR_NOT_FOUND when the prim is
     *         not an instance root.
     */
    ovstage_api_status_t ovstage_instancing_get_prototype_root(
        ovstage_instance_t* instance,
        ovx_primpath_t     instance_root,
        ovx_primpath_t*    out_prototype_root);

    /**
     * Return all prototype-root prim paths.
     *
     * @param instance              ovstage instance.
     * @param out_prototype_roots   Receives a path-list handle. The list may be
     *                              empty when the stage contains no scene-graph
     *                              instancing prototypes.
     * @return OVSTAGE_OK on success.
     */
    ovstage_api_status_t ovstage_instancing_get_prototype_roots(
        ovstage_instance_t*   instance,
        ovx_primpath_list_t* out_prototype_roots);

    /** @} */ // end of ovstage_instancing

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_INSTANCING_H */
