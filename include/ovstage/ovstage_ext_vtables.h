/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage_ext_vtables.h
 * @brief The extension vtables of an ovstage instance: the family vtables.
 *
 * @details
 * An ovstage_instance_t holds the data-plane instance, that is the public
 * vtable with its context, and the extension instances, one per family
 * (ovstage_api/ovstage_api_types.h). An extension instance pairs a family
 * vtable of this header with the context its bodies read. The families are
 * lifecycle, hierarchy, population, usd and instancing. The population family
 * holds the population functions of ovstage_population.h and the export
 * functions of ovstage_population_export.h. A population operation and an
 * export operation share one operation stream. Either wait fences the
 * operations of the other. The subtree clone is no family: ovstage_clone and
 * ovstage_fetch_clone_result are slots of the public vtable, because a clone is
 * an ordinal-keyed write of the data plane, awaited through wait_op like any
 * other operation. The usd family holds ovstage_get_usd_stage_id. This
 * TEMPORARY accessor returns the USD stage id of the population source. The
 * instancing family holds the instancing queries of ovstage_instancing.h.
 *
 * This header defines the family vtables. A family vtable holds one
 * member per function. A member takes the context of its family first,
 * `ovstage_<family>_context_t* context`, then the other parameters of its
 * function, as a slot of the public vtable takes the data-plane context
 * first. An implementation of one family therefore compiles against this
 * header and its own context type alone. The lifecycle vtable is the
 * exception: its members take the instance, because destroy_instance frees
 * the instance struct. The member prototypes name types of ovstage_types.h and
 * ovstage_population_types.h, which this header includes.
 *
 * The functions of the hierarchy, population, usd and instancing families
 * follow the data-plane pattern. Each one is a `static inline` wrapper in a
 * family header: ovstage.h, ovstage_population.h, ovstage_population_export.h
 * and ovstage_instancing.h.
 * A wrapper has two forms. The pair form, `ovstage_<name>_ex`, takes the
 * extension instance of its family. It rejects a NULL pointer with the
 * documented answer of the function. Then it calls its member with the
 * context of that instance, `<pair>->vtable-><member>(<pair>->context, ...)`.
 * The instance form, `ovstage_<name>`, takes the ovstage_instance_t* and
 * calls the pair form on `&instance->ext_instances.<family>`, so a NULL
 * instance gets the same answer. C++ overloads `ovstage_<name>` for both
 * receivers, so C++ code spells both forms with the name of the instance
 * form. The data-plane wrappers of ovstage_api/ovstage_api_utils.h have a
 * pair form and an instance form over the data-plane instance. They do not
 * check the pointer, with the exception of ovstage_get_path_dictionary. The
 * library exports no symbol for these functions. The lifecycle functions ovstage_get_config and
 * ovstage_destroy_instance stay exported C functions, like
 * ovstage_create_instance. Their bodies call the lifecycle instance with the
 * instance itself.
 *
 * ## Contract
 *
 * - The library fills a new instance with the stub pairs and its lifecycle
 *   pair. Then the factory of the implementation fills the pairs it serves,
 *   before the instance is public. A family the implementation does
 *   not serve keeps the vtable of stubs and the NULL context.
 * - Every vtable pointer and every member of every family vtable is set. A
 *   member the implementation does not serve answers
 *   OVSTAGE_ERROR_NOT_SUPPORTED and leaves the outputs unchanged.
 * - A body receives the context of its own extension instance,
 *   `instance->ext_instances.<family>.context`, and nothing else. The context
 *   has the type of the family, so a body cannot take the context of another
 *   family without a cast. A lifecycle body receives the instance and reads
 *   the lifecycle extension instance of it.
 * - Whoever writes an extension instance is responsible for its context: the
 *   context exists before the write and lives until the final destroy of the
 *   instance, whether the writer created it or reuses the context of another
 *   instance. Write an extension instance before the instance is shared,
 *   retained or used from another thread.
 * - A retained copy of an instance carries the extension instances of its
 *   source at the time of the retain.
 * - A plain copy of the instance struct or of an extension instance acquires
 *   no ownership and extends no lifetime. It is a view, not an instance for
 *   ovstage_destroy_instance. Only the instance that ovstage_create_instance
 *   returns goes to the destroy, and that destroy ends every view of it.
 * - A write of another extension instance into a pair changes which bodies
 *   the wrappers reach. It neither releases the old context nor takes
 *   ownership of the new one. Several families may share one context, so the
 *   owner releases a context once, not once per family.
 * - Whoever installs a borrowed extension instance keeps its context, its
 *   vtable and the code behind them alive for every use of the instance,
 *   also through a retained copy.
 * - Some results cross families, and a body creates them where the instance
 *   resolves them. The op index of a hierarchy function is awaited and
 *   released through wait_op and release_op of the data plane. A path list
 *   of an instancing function lives in the path dictionary of the data
 *   plane, ovstage_get_path_dictionary(instance), and the paths an instancing
 *   function accepts come from that dictionary. Nothing checks this when an
 *   extension instance is written.
 * - A consumer calls the wrappers, in the instance form or in the pair form.
 *   The instance form reads the extension instances.
 *
 * The family headers document each function.
 *
 * @version 0.4.0
 * @date 2026-10-29
 */

#ifndef OVSTAGE_EXT_VTABLES_H
#define OVSTAGE_EXT_VTABLES_H

#include "ovstage_api/ovstage_api.h"
#include "ovstage_population_types.h"
#include "ovstage_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════════
 * Family vtables
 *
 * A member takes the context of its family first, then the other parameters
 * of its function. The lifecycle members take the instance. An implementation
 * that does not serve a function points its member at a stub that answers
 * OVSTAGE_ERROR_NOT_SUPPORTED and leaves the outputs unchanged.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief The lifecycle family vtable: ovstage_get_config and
 *        ovstage_destroy_instance. The library exports these two functions,
 *        declared in ovstage.h, and their bodies call the members.
 */
typedef struct ovstage_lifecycle_vtable_t
{
    ovstage_api_status_t (*get_config)(
        const ovstage_instance_t* instance,
        ovstage_config_t*         out_config);

    /** Destroys the instance, with the contract of ovstage_destroy_instance:
     *  a status other than OVSTAGE_OK refuses the destroy, nothing is freed,
     *  and the instance stays valid until a later call returns OVSTAGE_OK. */
    ovstage_api_status_t (*destroy_instance)(
        ovstage_instance_t* instance);
} ovstage_lifecycle_vtable_t;

/**
 * @brief The hierarchy family vtable: the functions from ovstage_get_hierarchy
 *        to ovstage_compute_hierarchy. ovstage.h holds the wrappers.
 */
typedef struct ovstage_hierarchy_vtable_t
{
    ovstage_enqueue_result_t (*get_hierarchy)(
        ovstage_hierarchy_context_t*    context,
        ovx_primpath_list_t             prim_paths,
        ovstage_ordinal_t               ordinal,
        ovstage_hierarchy_relation_t    relation,
        ovstage_hierarchy_handle_t*     out_hierarchy_handle);

    ovstage_api_status_t (*fetch_hierarchy_result)(
        ovstage_hierarchy_context_t* context,
        ovstage_hierarchy_handle_t   hierarchy_handle,
        ovstage_hierarchy_result_t*  out_result);

    ovstage_api_status_t (*release_hierarchy_result)(
        ovstage_hierarchy_context_t*        context,
        const ovstage_hierarchy_result_t*   result);

    ovstage_enqueue_result_t (*release_hierarchy)(
        ovstage_hierarchy_context_t* context,
        ovstage_hierarchy_handle_t   handle);

    ovstage_api_status_t (*get_hierarchy_computation_models)(
        ovstage_hierarchy_context_t*                         context,
        const ovstage_hierarchy_computation_model_desc_t**   out_models,
        size_t*                                              out_model_count);

    ovstage_enqueue_result_t (*compute_hierarchy)(
        ovstage_hierarchy_context_t*               context,
        ovstage_hierarchy_computation_model_id_t   computation_model_id,
        ovstage_ordinal_t                          input_ordinal,
        ovstage_ordinal_t                          output_ordinal);
} ovstage_hierarchy_vtable_t;

/**
 * @brief The population family vtable: the functions from
 *        ovstage_population_open_usd_from_file to ovstage_population_wait_op,
 *        whose wrappers ovstage_population.h holds. Then the export functions
 *        from ovstage_population_export_destination_create to
 *        ovstage_population_export_wait_op, whose wrappers
 *        ovstage_population_export.h holds. The member of an export function
 *        is its name without the prefix ovstage_population_export_. The
 *        export wait is the exception: its member is export_wait_op, because
 *        wait_op is the population wait.
 */
typedef struct ovstage_population_vtable_t
{
    ovstage_population_enqueue_result_t (*open_usd_from_file)(
        ovstage_population_context_t* context,
        ovx_string_t                  path,
        ovstage_ordinal_t             ordinal,
        double                        time,
        uint32_t                      domains);

    ovstage_population_enqueue_result_t (*open_usd_from_string)(
        ovstage_population_context_t* context,
        ovx_string_t                  usda,
        ovstage_ordinal_t             ordinal,
        double                        time,
        uint32_t                      domains);

    ovstage_population_enqueue_result_t (*open_usd_from_file_with_desc)(
        ovstage_population_context_t*          context,
        ovx_string_t                           path,
        ovstage_ordinal_t                      ordinal,
        double                                 time,
        const ovstage_population_desc_t*       descs,
        size_t                                 desc_count);

    ovstage_population_enqueue_result_t (*open_usd_from_string_with_desc)(
        ovstage_population_context_t*          context,
        ovx_string_t                           usda,
        ovstage_ordinal_t                      ordinal,
        double                                 time,
        const ovstage_population_desc_t*       descs,
        size_t                                 desc_count);

    ovstage_population_enqueue_result_t (*repopulate_usd_with_desc)(
        ovstage_population_context_t*    context,
        ovstage_ordinal_t                ordinal,
        double                           time,
        const ovstage_population_desc_t* descs,
        size_t                           desc_count);

    ovstage_population_enqueue_result_t (*add_usd_reference_from_file)(
        ovstage_population_context_t*              context,
        ovx_string_t                               ref_file_path,
        ovx_string_t                               target_path,
        ovstage_population_usd_reference_handle_t* out_handle);

    ovstage_population_enqueue_result_t (*add_usd_reference_from_string)(
        ovstage_population_context_t*              context,
        ovx_string_t                               ref_str,
        ovx_string_t                               target_path,
        ovstage_population_usd_reference_handle_t* out_handle);

    ovstage_population_enqueue_result_t (*remove_usd_reference)(
        ovstage_population_context_t*             context,
        ovstage_population_usd_reference_handle_t handle);

    ovstage_population_enqueue_result_t (*reset_usd)(
        ovstage_population_context_t* context);

    ovstage_population_enqueue_result_t (*apply_usd_time)(
        ovstage_population_context_t* context,
        ovstage_ordinal_t             ordinal,
        double                        time);

    ovstage_population_enqueue_result_t (*apply_usd_changes)(
        ovstage_population_context_t* context,
        ovstage_ordinal_t             ordinal);

    ovstage_api_status_t (*wait_op)(
        ovstage_population_context_t*        context,
        ovstage_population_op_id_t           op_id,
        ovstage_timeout_ns_t                 timeout_ns,
        ovstage_population_op_wait_result_t* out_wait_result);

    /* The export functions. */

    ovstage_api_status_t (*destination_create)(
        ovstage_population_context_t*                   context,
        ovx_string_t                                    usd_identifier,
        ovstage_population_export_destination_mode_t    mode,
        ovstage_population_export_destination_handle_t* out_destination);

    ovstage_population_enqueue_result_t (*destination_enqueue_create)(
        ovstage_population_context_t*                   context,
        ovx_string_t                                    usd_identifier,
        ovstage_population_export_destination_mode_t    mode,
        ovstage_population_export_destination_handle_t* out_destination);

    ovstage_api_status_t (*to_destination)(
        ovstage_population_context_t*                  context,
        ovstage_population_export_destination_handle_t destination,
        const ovstage_population_export_desc_t*        desc,
        ovstage_population_export_report_t*            report);

    ovstage_population_enqueue_result_t (*enqueue_to_destination)(
        ovstage_population_context_t*                  context,
        ovstage_population_export_destination_handle_t destination,
        const ovstage_population_export_desc_t*        desc);

    ovstage_api_status_t (*destination_save)(
        ovstage_population_context_t*                  context,
        ovstage_population_export_destination_handle_t destination);

    ovstage_population_enqueue_result_t (*destination_enqueue_save)(
        ovstage_population_context_t*                  context,
        ovstage_population_export_destination_handle_t destination);

    ovstage_api_status_t (*destination_destroy)(
        ovstage_population_context_t*                  context,
        ovstage_population_export_destination_handle_t destination);

    ovstage_population_enqueue_result_t (*destination_enqueue_destroy)(
        ovstage_population_context_t*                  context,
        ovstage_population_export_destination_handle_t destination);

    ovstage_api_status_t (*to_usd_file)(
        ovstage_population_context_t*           context,
        ovx_string_t                            usd_identifier,
        const ovstage_population_export_desc_t* desc,
        ovstage_population_export_report_t*     report);

    ovstage_population_enqueue_result_t (*enqueue_to_usd_file)(
        ovstage_population_context_t*           context,
        ovx_string_t                            usd_identifier,
        const ovstage_population_export_desc_t* desc);

    ovstage_api_status_t (*to_usd_layer_identifier)(
        ovstage_population_context_t*           context,
        ovx_string_t                            usd_layer_identifier,
        const ovstage_population_export_desc_t* desc,
        ovstage_population_export_report_t*     report);

    ovstage_api_status_t (*to_usd_stage_identifier)(
        ovstage_population_context_t*           context,
        ovx_string_t                            usd_stage_root_layer_identifier,
        const ovstage_population_export_desc_t* desc,
        ovstage_population_export_report_t*     report);

    ovstage_population_enqueue_result_t (*enqueue_to_usd_layer_identifier)(
        ovstage_population_context_t*           context,
        ovx_string_t                            usd_layer_identifier,
        const ovstage_population_export_desc_t* desc);

    ovstage_population_enqueue_result_t (*enqueue_to_usd_stage_identifier)(
        ovstage_population_context_t*           context,
        ovx_string_t                            usd_stage_root_layer_identifier,
        const ovstage_population_export_desc_t* desc);

    ovstage_api_status_t (*export_wait_op)(
        ovstage_population_context_t*       context,
        ovstage_population_op_id_t          op_id,
        ovstage_timeout_ns_t                timeout_ns,
        ovstage_population_export_report_t* report);
} ovstage_population_vtable_t;

/**
 * @brief The usd family vtable: ovstage_get_usd_stage_id. This TEMPORARY
 *        accessor returns the USD stage id of the population source.
 *        ovstage.h holds the wrapper.
 */
typedef struct ovstage_usd_vtable_t
{
    ovstage_api_status_t (*get_usd_stage_id)(
        ovstage_usd_context_t* context,
        uint64_t*              out_usd_stage_id);
} ovstage_usd_vtable_t;

/**
 * @brief The instancing family vtable: the instancing queries of
 *        ovstage_instancing.h, which holds the wrappers.
 */
typedef struct ovstage_instancing_vtable_t
{
    ovstage_api_status_t (*get_instance_roots)(
        ovstage_instancing_context_t* context,
        ovx_primpath_t                prototype_root,
        ovx_primpath_list_t*          out_instance_roots);

    ovstage_api_status_t (*get_prototype_root)(
        ovstage_instancing_context_t* context,
        ovx_primpath_t                instance_root,
        ovx_primpath_t*               out_prototype_root);

    ovstage_api_status_t (*get_prototype_roots)(
        ovstage_instancing_context_t* context,
        ovx_primpath_list_t*          out_prototype_roots);
} ovstage_instancing_vtable_t;

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_EXT_VTABLES_H */
