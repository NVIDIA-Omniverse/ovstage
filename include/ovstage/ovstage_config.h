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

/** @defgroup ovstage_config_helpers Configuration helper functions
 *  @{
 */

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
 * Configure the ovstage "binary package root" directory.
 *
 * This is used by the static loader (ovstage-static) to locate the ovstage
 * shared library and, transitively, its bundled runtime closure (plugins/,
 * ovstage_usd_schemas/), which ovstage resolves relative to the directory the
 * shared library is loaded from. If not provided, the loader defaults to its
 * own module directory.
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

/** @} */ // end of ovstage_config_helpers

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_CONFIG_H */
