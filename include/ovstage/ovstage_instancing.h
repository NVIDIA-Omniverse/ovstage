/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * ovstage instancing: high-level scene-graph-instancing queries.
 *
 * These functions query the latest committed data through an ovstage instance.
 * The single-prototype and prototype-root functions are `static inline`
 * wrappers with two forms. The pair form,
 * `ovstage_instancing_<name>_ex`, takes the instancing instance,
 * `&instance->ext_instances.instancing`. It rejects a NULL pointer with
 * `OVSTAGE_ERROR_INVALID_ARGUMENT` and resets the output as the documentation
 * of the function states. Then it calls its member with the context of that
 * instance, `instancing->vtable-><member>(instancing->context, ...)`. The
 * instance form, `ovstage_instancing_<name>`, takes the ovstage_instance_t*
 * and calls the pair form on its instancing instance, so a NULL instance gets
 * the same answer. C++ overloads `ovstage_instancing_<name>` for both
 * receivers, so C++ code spells both forms with the name of the instance form.
 * ovstage_ext_vtables.h defines the vtable. A member takes the context of its
 * family first, then the other parameters of its function. An implementation
 * that does not serve a function points its member at a stub that answers
 * `OVSTAGE_ERROR_NOT_SUPPORTED` and leaves the outputs unchanged. The library
 * exports no symbol for the family functions. The batch and logical-path
 * queries below are separate exported C entry points.
 *
 * Path-list results are freshly allocated through the instance path dictionary
 * with refcount = 1. Callers release them via
 * path_dictionary_release_path_list_reference(ovstage_get_path_dictionary(instance), list).
 * Individual ovx_primpath_t values are dictionary-lifetime handles and do not
 * require per-result release. An implementation of the instancing family
 * therefore creates its lists in that dictionary and accepts paths of it.
 */

#ifndef OVSTAGE_INSTANCING_H
#define OVSTAGE_INSTANCING_H

#include "ovstage_api/ovstage_api.h"
#include "ovstage_ext_vtables.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum ovstage_instancing_path_kind_t
    {
        OVSTAGE_INSTANCING_PATH_NOT_FOUND = 0,
        OVSTAGE_INSTANCING_PATH_NON_INSTANCE = 1,
        OVSTAGE_INSTANCING_PATH_INSTANCE = 2
    } ovstage_instancing_path_kind_t;

    typedef struct ovstage_instancing_path_resolution_t
    {
        ovstage_instancing_path_kind_t kind;
        ovx_primpath_t instance_root;
        ovx_primpath_t prototype_root;
        ovx_primpath_t backing_path;
    } ovstage_instancing_path_resolution_t;

    /**
     * Resolve an ordered batch of arbitrary logical prim paths synchronously.
     * Logical proxies need not have physical rows at their input paths. Nested
     * instances return the nearest logical root and its prototype, with the
     * final prototype backing path. At an exact instance root the backing path
     * is its prototype root. Ordinary prims return NON_INSTANCE, invalid root
     * handles, and the input as backing_path. Missing or unpopulated backing
     * prims return NOT_FOUND with all path handles invalid. Prototype-space
     * inputs keep that namespace for instance_root; no placement is inferred.
     *
     * @param instance Live instance, which must remain alive during the call.
     * @param logical_paths Absolute prim paths interned in its path dictionary.
     * @param path_count Number of input and output elements.
     * @param out_resolutions Caller-owned array, not overlapping the input.
     * @return OK for a completed batch, including missing paths. Invalid handles
     *         return INVALID_HANDLE; NULL arguments or non-prim paths return
     *         INVALID_ARGUMENT. An unsealed tracked write may return
     *         WRITE_FLOOR_VIOLATION; cyclic topology returns INTERNAL. Failed calls
     *         reset all accessible outputs to NOT_FOUND and invalid handles.
     *         Zero count with a live instance succeeds without stage queries;
     *         either array may then be NULL.
     * @pre Wait for relevant enqueued mutations and seal their write ordinal by
     *      advancing the write floor before querying. Exclude concurrent topology
     *      commits for a coherent latest-committed batch. No historical snapshot
     *      is implied. Prototype membership, instance edges, population, and
     *      topology changes invalidate the shared graph. Direct data-plane writes
     *      to unrelated material/value attributes leave it reusable; population
     *      source edits may conservatively rebuild it. Backing-prim existence is
     *      checked on every call.
     * @post Order and duplicates are preserved. No result releases are needed.
     *       Results are values, not live views; resolve again after changes.
     */
    ovstage_api_status_t ovstage_instancing_resolve_paths(
        ovstage_instance_t* instance,
        const ovx_primpath_t* logical_paths,
        size_t path_count,
        ovstage_instancing_path_resolution_t* out_resolutions);

    /** @brief The pair form of ovstage_instancing_get_instance_roots: the same
     *         call on the instancing instance, `&instance->ext_instances.instancing`.
     *         C++ overloads ovstage_instancing_get_instance_roots for this
     *         receiver. */
    static inline ovstage_api_status_t ovstage_instancing_get_instance_roots_ex(
        ovstage_instancing_instance_t* instancing,
        ovx_primpath_t                 prototype_root,
        ovx_primpath_list_t*           out_instance_roots)
    {
        if (instancing == NULL)
        {
            if (out_instance_roots != NULL)
                *out_instance_roots = OVX_INVALID_PRIMPATH_LIST;
            return OVSTAGE_ERROR_INVALID_ARGUMENT;
        }
        return instancing->vtable->get_instance_roots(
            instancing->context, prototype_root, out_instance_roots);
    }

    /**
     * Return all instance-root prim paths that reference @p prototype_root.
     *
     * @param instance             ovstage instance.
     * @param prototype_root       Prototype-root prim path.
     * @param out_instance_roots   Receives a path-list handle. An implementation
     *                             that does not serve this function returns
     *                             `OVSTAGE_ERROR_NOT_SUPPORTED` and leaves it
     *                             unchanged. Set to `OVX_INVALID_PRIMPATH_LIST`
     *                             on any other failure. The list may be empty
     *                             when no instance roots reference
     *                             @p prototype_root.
     * @return OVSTAGE_OK on success. OVSTAGE_ERROR_INVALID_ARGUMENT when
     *         @p instance or @p out_instance_roots is NULL, or when
     *         @p prototype_root is `OVX_INVALID_PRIMPATH`.
     */
    static inline ovstage_api_status_t ovstage_instancing_get_instance_roots(
        ovstage_instance_t*  instance,
        ovx_primpath_t       prototype_root,
        ovx_primpath_list_t* out_instance_roots)
    {
        return ovstage_instancing_get_instance_roots_ex(
            instance != NULL ? &instance->ext_instances.instancing : NULL,
            prototype_root, out_instance_roots);
    }

    /** @brief The pair form of ovstage_instancing_get_prototype_root: the same
     *         call on the instancing instance, `&instance->ext_instances.instancing`.
     *         C++ overloads ovstage_instancing_get_prototype_root for this
     *         receiver. */
    static inline ovstage_api_status_t ovstage_instancing_get_prototype_root_ex(
        ovstage_instancing_instance_t* instancing,
        ovx_primpath_t                 instance_root,
        ovx_primpath_t*                out_prototype_root)
    {
        if (instancing == NULL)
        {
            if (out_prototype_root != NULL)
                *out_prototype_root = OVX_INVALID_PRIMPATH;
            return OVSTAGE_ERROR_INVALID_ARGUMENT;
        }
        return instancing->vtable->get_prototype_root(
            instancing->context, instance_root, out_prototype_root);
    }

    /**
     * Return flattened instance roots for an ordered batch of prototype paths.
     *
     * Each output contains the flattened instance-root paths that reference
     * the corresponding input prototype path.
     * Duplicate inputs produce independently owned lists; an unrelated valid
     * path produces an owned empty list. Result lists are not shared between
     * calls.
     *
     * @param instance              Live ovstage instance.
     * @param prototype_roots       Array of dictionary-lifetime path handles.
     * @param prototype_count       Number of input and output elements.
     * @param out_instance_roots    Output array; must not overlap the input.
     * @return OVSTAGE_OK on success. Null required arguments return
     *         OVSTAGE_ERROR_INVALID_ARGUMENT; invalid or unknown path handles
     *         return OVSTAGE_ERROR_INVALID_HANDLE. An unsealed tracked write may
     *         return OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION; cyclic topology returns
     *         OVSTAGE_ERROR_INTERNAL; allocation failure returns
     *         OVSTAGE_ERROR_OUT_OF_MEMORY. Other query/read failures propagate
     *         their status. On failure all output elements are invalid and the
     *         function releases any lists it allocated. A zero count
     *         succeeds without querying stage data; either array may then be
     *         NULL.
     * @pre For nonzero count both arrays contain at least prototype_count
     *      elements. Wait for relevant enqueued mutations and seal their write
     *      ordinal by advancing the write floor. Exclude concurrent topology mutations for a coherent
     *      latest-committed enumeration; no historical snapshot is implied.
     * @post On success the caller owns one reference per output element and
     *       releases each through the instance path dictionary before destroying
     *       the instance. The instance must remain alive during the call.
     */
    ovstage_api_status_t ovstage_instancing_get_instance_roots_batch(
        ovstage_instance_t* instance,
        const ovx_primpath_t* prototype_roots,
        size_t prototype_count,
        ovx_primpath_list_t* out_instance_roots);

    /**
     * Return the prototype root referenced by @p instance_root.
     *
     * @param instance             ovstage instance.
     * @param instance_root        Instance-root prim path.
     * @param out_prototype_root   Receives the prototype-root prim path. An
     *                             implementation that does not serve this
     *                             function returns `OVSTAGE_ERROR_NOT_SUPPORTED`
     *                             and leaves it unchanged. Set to
     *                             `OVX_INVALID_PRIMPATH` on any other failure.
     * @return OVSTAGE_OK on success. OVSTAGE_ERROR_NOT_FOUND when the prim is
     *         not an instance root. OVSTAGE_ERROR_INVALID_ARGUMENT when
     *         @p instance or @p out_prototype_root is NULL, or when
     *         @p instance_root is `OVX_INVALID_PRIMPATH`.
     */
    static inline ovstage_api_status_t ovstage_instancing_get_prototype_root(
        ovstage_instance_t* instance,
        ovx_primpath_t      instance_root,
        ovx_primpath_t*     out_prototype_root)
    {
        return ovstage_instancing_get_prototype_root_ex(
            instance != NULL ? &instance->ext_instances.instancing : NULL,
            instance_root, out_prototype_root);
    }

    /** @brief The pair form of ovstage_instancing_get_prototype_roots: the same
     *         call on the instancing instance, `&instance->ext_instances.instancing`.
     *         C++ overloads ovstage_instancing_get_prototype_roots for this
     *         receiver. */
    static inline ovstage_api_status_t ovstage_instancing_get_prototype_roots_ex(
        ovstage_instancing_instance_t* instancing,
        ovx_primpath_list_t*           out_prototype_roots)
    {
        if (instancing == NULL)
        {
            if (out_prototype_roots != NULL)
                *out_prototype_roots = OVX_INVALID_PRIMPATH_LIST;
            return OVSTAGE_ERROR_INVALID_ARGUMENT;
        }
        return instancing->vtable->get_prototype_roots(instancing->context, out_prototype_roots);
    }

    /**
     * Return all prototype-root prim paths.
     *
     * @param instance              ovstage instance.
     * @param out_prototype_roots   Receives a path-list handle. An implementation
     *                              that does not serve this function returns
     *                              `OVSTAGE_ERROR_NOT_SUPPORTED` and leaves it
     *                              unchanged. Set to `OVX_INVALID_PRIMPATH_LIST`
     *                              on any other failure. The list may be empty
     *                              when the stage contains no scene-graph
     *                              instancing prototypes.
     * @return OVSTAGE_OK on success. OVSTAGE_ERROR_INVALID_ARGUMENT when
     *         @p instance or @p out_prototype_roots is NULL.
     */
    static inline ovstage_api_status_t ovstage_instancing_get_prototype_roots(
        ovstage_instance_t*  instance,
        ovx_primpath_list_t* out_prototype_roots)
    {
        return ovstage_instancing_get_prototype_roots_ex(
            instance != NULL ? &instance->ext_instances.instancing : NULL, out_prototype_roots);
    }

#ifdef __cplusplus
} /* extern "C" */

/* C++ spells the pair form with the name of the instance form. A literal NULL or
 * nullptr first argument is ambiguous between the two receivers: pass a
 * typed null, or call the _ex form. */
inline ovstage_api_status_t ovstage_instancing_get_instance_roots(
    ovstage_instancing_instance_t* instancing, ovx_primpath_t prototype_root,
    ovx_primpath_list_t* out_instance_roots)
{
    return ovstage_instancing_get_instance_roots_ex(instancing, prototype_root, out_instance_roots);
}

inline ovstage_api_status_t ovstage_instancing_get_prototype_root(
    ovstage_instancing_instance_t* instancing, ovx_primpath_t instance_root,
    ovx_primpath_t* out_prototype_root)
{
    return ovstage_instancing_get_prototype_root_ex(instancing, instance_root, out_prototype_root);
}

inline ovstage_api_status_t ovstage_instancing_get_prototype_roots(
    ovstage_instancing_instance_t* instancing, ovx_primpath_list_t* out_prototype_roots)
{
    return ovstage_instancing_get_prototype_roots_ex(instancing, out_prototype_roots);
}
#endif

#endif /* OVSTAGE_INSTANCING_H */
