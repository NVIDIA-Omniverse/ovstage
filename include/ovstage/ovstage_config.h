/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage_config.h
 * @brief Helpers for building ovstage_config_t entries passed to
 *        ovstage_initialize().
 */

#ifndef OVSTAGE_CONFIG_H
#define OVSTAGE_CONFIG_H

#include "ovstage_types.h"
#include <ovx/config_tokens.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build a config entry for a string setting.
 * @param key Config key from ovstage_config_string_t.
 * @param value String value. value.ptr must remain valid until the API call
 *              that consumes the config returns.
 */
static inline ovstage_config_entry_t ovstage_config_entry_string(ovstage_config_string_t key,
                                                                 ovx_string_t value)
{
    ovstage_config_entry_t entry;
    entry.key_type = OVSTAGE_CONFIG_KEY_TYPE_STRING;
    entry.key.string_key = key;
    entry.value.string_value = value;
    return entry;
}

/**
 * Build a config entry for a boolean setting.
 * @param key Config key from ovstage_config_bool_t.
 * @param value Boolean value.
 */
static inline ovstage_config_entry_t ovstage_config_entry_bool(ovstage_config_bool_t key,
                                                               bool value)
{
    ovstage_config_entry_t entry;
    entry.key_type = OVSTAGE_CONFIG_KEY_TYPE_BOOL;
    entry.key.bool_key = key;
    entry.value.bool_value = value;
    return entry;
}

/**
 * Build a config entry for an unsigned 64-bit setting.
 * @param key Config key from ovstage_config_uint64_t.
 * @param value Unsigned 64-bit value.
 */
static inline ovstage_config_entry_t ovstage_config_entry_uint64(ovstage_config_uint64_t key,
                                                                 uint64_t value)
{
    ovstage_config_entry_t entry;
    entry.key_type = OVSTAGE_CONFIG_KEY_TYPE_UINT64;
    entry.key.uint64_key = key;
    entry.value.uint_value = value;
    return entry;
}

/**
 * Configure the number of retained sealed ordinals for history-enabled
 * attributes (OVSTAGE_CONFIG_RETENTION_WINDOW).
 * The process default is four. Valid capacities are [1, SIZE_MAX - 1].
 * Supply an override before creating the first instance. Later explicit
 * values must match the active setting. Existing instances never resize.
 * After the last process reference is released the default resets to four.
 * A history-capable instance reads the setting: it bounds the earlier sealed
 * values that the selected pairs retain. An instance that keeps history off
 * ignores the setting. The setting alone retains nothing. Enroll attributes
 * with ovstage_enable_history() (ovstage_history.h) on a sealed
 * history-capable instance.
 * @remark The `implementation` member of ovstage_instance_desc_t selects the
 *         implementation: "history_selective" is history-capable, and the
 *         default "history_latest" keeps history off.
 * @note Experimental. This setting, the implementation "history_selective"
 *       and the history selection API of ovstage_history.h are experimental.
 *       Their API, ABI and semantics can change between releases without a
 *       deprecation period.
 */
static inline ovstage_config_entry_t ovstage_config_entry_retention_window(uint64_t capacity)
{
    return ovstage_config_entry_uint64(OVSTAGE_CONFIG_RETENTION_WINDOW, capacity);
}

/**
 * Configure the hierarchy computation model used by automatic transform
 * updates and by OVSTAGE_HIERARCHY_COMPUTATION_MODEL_RUNTIME_DEFAULT.
 *
 * To override the default, use CPU_INCREMENTAL, GPU_INCREMENTAL, or GPU_GLOBAL
 * (the DEFAULT_CPU and DEFAULT_GPU aliases are also accepted). RUNTIME_DEFAULT
 * means "no override"; an entry with that value is ignored.
 *
 * The setting is process-scoped and must be supplied when acquiring the first
 * process reference. Existing instances retain the default captured when they
 * were created.
 */
static inline ovstage_config_entry_t ovstage_config_entry_runtime_default_hierarchy_computation_model(
    ovstage_hierarchy_computation_model_id_t model)
{
    return ovstage_config_entry_uint64(
        OVSTAGE_CONFIG_RUNTIME_DEFAULT_HIERARCHY_COMPUTATION_MODEL,
        (uint64_t)model);
}

/**
 * Configure ordinal admission for data-plane writes, maps, clones and deletes.
 *
 * DEFAULT means no override. A fresh process-state lifetime uses
 * NONDECREASING; later DEFAULT entries inherit the active policy.
 * Supply a concrete policy on the first process reference. Later concrete
 * values must match until all initialization and instance references are gone.
 *
 * @param policy The ordinal admission policy.
 */
static inline ovstage_config_entry_t ovstage_config_entry_runtime_default_ordinal_admission_policy(
    ovstage_ordinal_admission_policy_t policy)
{
    return ovstage_config_entry_uint64(
        OVSTAGE_CONFIG_RUNTIME_DEFAULT_ORDINAL_ADMISSION_POLICY,
        (uint64_t)policy);
}

/**
 * Configure the maximum number of threads one population operation may use to
 * read what its selectors select, including the thread that runs it.
 *
 * 1 reads serially, which is the default. N > 1 allows up to N threads, and
 * OVSTAGE_POPULATION_THREAD_LIMIT_AUTO lets the runtime choose, up to the
 * concurrency the process allows. The runtime may use fewer threads than the
 * limit. Populated data does not depend on this setting.
 *
 * The limit does not cover the rest of a population operation. Opening and
 * composing the USD stage and populating OVSTAGE_POPULATION_DOMAIN_RENDERING
 * keep their own concurrency, so a limit of 1 does not make a whole
 * population operation single-threaded. Which population work the limit
 * covers may grow between releases.
 *
 * OVSTAGE_POPULATION_THREAD_LIMIT_DEFAULT (0) means no override; an
 * entry with that value is ignored. Supply a limit on the first process
 * reference. Later limits must match until all initialization and instance
 * references are gone. Existing instances retain the limit captured when they
 * were created.
 *
 * @param limit The thread limit.
 */
static inline ovstage_config_entry_t ovstage_config_entry_population_thread_limit(uint64_t limit)
{
    return ovstage_config_entry_uint64(OVSTAGE_CONFIG_POPULATION_THREAD_LIMIT, limit);
}

/**
 * Configure strict USD resolution for wholesale populations (opens and
 * repopulates).
 *
 * When enabled, ovstage_population_open_usd_* and
 * ovstage_population_repopulate_usd_with_desc fail with
 * OVSTAGE_ERROR_OP_FAILED when composition reports unresolved
 * sublayer/reference/payload asset paths (up to three of the unresolved
 * identifiers, plus a count of the rest, are listed by
 * ovstage_population_get_last_op_error). Disabled by default:
 * unresolved dependencies are logged as warnings and the op succeeds.
 *
 * Process-scoped: supply it when acquiring the first process reference (see
 * OVSTAGE_CONFIG_POPULATION_STRICT_RESOLUTION for the conflict rules).
 */
static inline ovstage_config_entry_t ovstage_config_entry_population_strict_resolution(bool enabled)
{
    return ovstage_config_entry_bool(OVSTAGE_CONFIG_POPULATION_STRICT_RESOLUTION, enabled);
}

/**
 * Configure the ovstage "binary package root" directory.
 *
 * This is used by the static loader (ovstage-static) to locate the ovstage
 * shared library and, transitively, its bundled runtime closure (plugins/),
 * which ovstage resolves relative to the directory the shared library is loaded
 * from. If not provided, the loader defaults to its own module directory.
 *
 * Pass this entry to ovstage_initialize(). The loader loads the shared library
 * once, from the root supplied on that first load — so static-loader consumers
 * that need a non-default root must call ovstage_initialize() with this entry
 * BEFORE any other ovstage_* call (ovstage_create_instance would otherwise
 * trigger the load from the default module directory). Ignored by the dynamic
 * ovstage shared library, so it is harmless when passed in non-static builds.
 *
 * @param path The package `bin/` directory. path.ptr must remain valid until
 *             the ovstage_initialize() call that consumes the config returns.
 *             The path may contain the @ref OVX_CONFIG_EXECUTABLE_DIR_TOKEN token,
 *             which the static loader substitutes with the absolute directory of
 *             the running executable (e.g. OVX_CONFIG_EXECUTABLE_DIR_TOKEN "/ovstage/bin").
 */
static inline ovstage_config_entry_t ovstage_config_entry_binary_package_root_path(ovx_string_t path)
{
    return ovstage_config_entry_string(OVSTAGE_CONFIG_BINARY_PACKAGE_ROOT_PATH, path);
}

/**
 * Absolute directory of the running executable, with no trailing separator.
 *
 * Convenience for static-loader clients building the binary package root, e.g.
 * OVX_CONFIG_EXECUTABLE_DIR_TOKEN "/ovstage", to pass via
 * ovstage_config_entry_binary_package_root_path() — so callers need not
 * reimplement the platform-specific GetModuleFileName/readlink lookup.
 *
 * Provided by the ovstage-static loader (link ovstage::ovstage_static); it is
 * not exported by the dynamic ovstage shared library. The returned pointer is
 * backed by a thread-local buffer valid until the next ovstage_executable_dir()
 * call on the same thread. Returns {NULL, 0} if the path cannot be determined.
 * Callable at any time, including before ovstage_initialize().
 */
ovx_string_t ovstage_executable_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_CONFIG_H */
