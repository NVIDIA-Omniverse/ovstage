/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage_types.h
 * @brief Backend-owned ovstage data-plane types.
 *
 * @details
 * This header contains the type surface that is specific to the ovstage backend.
 * The generic vtable runtime types remain in `ovstage_api/ovstage_api_types.h`.
 *
 * @version 0.1.1
 * @date 2026-06-17
 */

#ifndef OVSTAGE_TYPES_H
#define OVSTAGE_TYPES_H

#include "ovstage_api/ovstage_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for creating an ovstage instance.
 *
 * @note GPU device configuration is intentionally omitted; see "Deferred Design
 * Items" in ovstage.h.
 */
typedef struct {
    const char* name;  /**< Optional instance name for debugging. May be NULL. */
} ovstage_instance_desc_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Process configuration (ovstage_initialize)
 *
 * The typed key/value shape mirrors ovrtx_config_t so keys can be added without
 * an ABI break and so the signature does not churn as keys land. The static
 * loader (ovstage-static) consumes package-location keys, while the ovstage
 * shared library consumes runtime-behavior keys. Pass NULL (or a struct with
 * entry_count 0) to select defaults.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Key-type tag for ovstage_config_entry_t; selects the valid key/value union members. */
typedef enum {
    OVSTAGE_CONFIG_KEY_TYPE_BOOL,
    OVSTAGE_CONFIG_KEY_TYPE_INT64,
    OVSTAGE_CONFIG_KEY_TYPE_UINT64,
    OVSTAGE_CONFIG_KEY_TYPE_DOUBLE,
    OVSTAGE_CONFIG_KEY_TYPE_STRING,
    OVSTAGE_CONFIG_KEY_TYPE_BLOB,
    OVSTAGE_CONFIG_KEY_TYPE_COUNT
} ovstage_config_key_type_t;

/** @brief Boolean config keys. Value type: bool. (None defined yet.) */
typedef enum { OVSTAGE_CONFIG_BOOL_COUNT } ovstage_config_bool_t;
/** @brief Int64 config keys. Value type: int64_t. (None defined yet.) */
typedef enum { OVSTAGE_CONFIG_INT64_COUNT } ovstage_config_int64_t;
/** @brief Uint64 config keys. Value type: uint64_t. */
typedef enum {
    /**
     * ovstage_hierarchy_computation_model_id_t override used for automatic
     * hierarchy-derived transform updates and by
     * OVSTAGE_HIERARCHY_COMPUTATION_MODEL_RUNTIME_DEFAULT.
     *
     * Defaults to OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL. An entry
     * whose value is RUNTIME_DEFAULT is ignored.
     */
    OVSTAGE_CONFIG_RUNTIME_DEFAULT_HIERARCHY_COMPUTATION_MODEL,
    OVSTAGE_CONFIG_UINT64_COUNT
} ovstage_config_uint64_t;
/** @brief Double config keys. Value type: double. (None defined yet.) */
typedef enum { OVSTAGE_CONFIG_DOUBLE_COUNT } ovstage_config_double_t;
/** @brief String config keys. Value type: ovx_string_t. */
typedef enum {
    /** Directory of the ovstage binary package (the package `bin/`). The static
     *  loader (ovstage-static) loads the ovstage shared library from here and
     *  ovstage resolves its bundled plugins/USD schemas relative to it. When
     *  absent, the loader defaults to its own module directory. Ignored by the
     *  ovstage shared library's own ovstage_initialize. */
    OVSTAGE_CONFIG_BINARY_PACKAGE_ROOT_PATH,
    OVSTAGE_CONFIG_STRING_COUNT
} ovstage_config_string_t;
/** @brief Blob config keys. Value type: ptr + size. (None defined yet.) */
typedef enum { OVSTAGE_CONFIG_BLOB_COUNT } ovstage_config_blob_t;

/** @brief A single config entry. `key_type` selects the valid `key`/`value` union members. */
typedef struct {
    ovstage_config_key_type_t key_type;
    union {
        ovstage_config_bool_t   bool_key;
        ovstage_config_int64_t  int64_key;
        ovstage_config_uint64_t uint64_key;
        ovstage_config_double_t double_key;
        ovstage_config_string_t string_key;
        ovstage_config_blob_t   blob_key;
    } key;
    union {
        bool         bool_value;
        int64_t      int_value;
        uint64_t     uint_value;
        double       double_value;
        ovx_string_t string_value;
        struct { const void* data; size_t size; } blob_value;
    } value;
} ovstage_config_entry_t;

/**
 * @brief Process configuration passed to ovstage_initialize().
 *
 * NULL, or a non-NULL struct with entry_count 0, selects defaults.
 */
typedef struct {
    const ovstage_config_entry_t* entries;
    size_t                        entry_count;
} ovstage_config_t;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Logging (ovstage_set_log_callback)
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Log severity levels. Values follow the underlying log-level ordering. */
typedef enum {
    OVSTAGE_LOG_VERBOSE = -2, /**< Most verbose (debug/trace). */
    OVSTAGE_LOG_INFO    = -1, /**< Informational. */
    OVSTAGE_LOG_WARNING =  0, /**< Warning; operation continues. */
    OVSTAGE_LOG_ERROR   =  1, /**< Error; operation may have failed (fatal is reported here too). */
    OVSTAGE_LOG_NONE    =  3, /**< Threshold sentinel: as a filter level, disables all logging.
                                   Never delivered to the callback. */
} ovstage_log_severity_t;

/**
 * @brief Callback for receiving ovstage log messages.
 *
 * Process-global (see ovstage_set_log_callback) and may be invoked from any
 * thread; the implementation serializes invocations, so the callback body needs
 * no mutex of its own for its own state. @p message is valid only for the
 * duration of the call — copy it to retain.
 *
 * @param severity  Severity of the message.
 * @param timestamp Wall-clock seconds since the epoch.
 * @param message   Message text (valid only during the call).
 * @param user_data Context passed to ovstage_set_log_callback.
 */
typedef void (*ovstage_log_callback_t)(ovstage_log_severity_t severity,
                                       double                 timestamp,
                                       ovx_string_t           message,
                                       void*                  user_data);

/** @brief Hierarchy handle - identifies an enqueued hierarchy lookup batch. */
typedef uint64_t ovstage_hierarchy_handle_t;
#define OVSTAGE_INVALID_HIERARCHY_HANDLE ((ovstage_hierarchy_handle_t)0)

/** @brief Opaque identity for a fetched hierarchy result payload. */
typedef uint64_t ovstage_hierarchy_result_id_t;
#define OVSTAGE_INVALID_HIERARCHY_RESULT_ID ((ovstage_hierarchy_result_id_t)0)

/**
 * @brief Public hierarchy computation model identifiers.
 *
 * Hierarchy computation models update hierarchy-derived stage data, such as
 * world transforms, visibility, and derived bounds/state owned by the backing
 * runtime. The enum values are stable API inputs for ovstage_compute_hierarchy.
 *
 * Not every backend, platform, or build necessarily supports every concrete
 * model below. Call ovstage_get_hierarchy_computation_models to discover the
 * concrete models and selectors advertised by the current ovstage instance,
 * along with their display names and descriptions.
 *
 * The DEFAULT_* aliases are semantic convenience choices for callers that only
 * care about CPU vs GPU placement. They intentionally alias concrete entries
 * instead of introducing extra model IDs, so catalog discovery remains stable
 * and duplicate-free.
 */
typedef enum {
    /** Invalid/sentinel value; never accepted by ovstage_compute_hierarchy. */
    OVSTAGE_HIERARCHY_COMPUTATION_MODEL_INVALID = 0,

    /** Incrementally update hierarchy-derived stage data on CPU. */
    OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL = 1,

    /** Incrementally update hierarchy-derived stage data on GPU. */
    OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_INCREMENTAL = 2,

    /** Globally recompute hierarchy-derived stage data on GPU. */
    OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_GLOBAL = 3,

    /**
     * Use the runtime default captured when this ovstage instance was created.
     *
     * The process default is configured through
     * OVSTAGE_CONFIG_RUNTIME_DEFAULT_HIERARCHY_COMPUTATION_MODEL and defaults
     * to CPU_INCREMENTAL.
     */
    OVSTAGE_HIERARCHY_COMPUTATION_MODEL_RUNTIME_DEFAULT = 4,

    /** Default CPU hierarchy computation model for this API revision. */
    OVSTAGE_HIERARCHY_COMPUTATION_MODEL_DEFAULT_CPU =
        OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL,

    /** Default GPU hierarchy computation model for this API revision. */
    OVSTAGE_HIERARCHY_COMPUTATION_MODEL_DEFAULT_GPU =
        OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_GLOBAL,
} ovstage_hierarchy_computation_model_id_t;

#define OVSTAGE_INVALID_HIERARCHY_COMPUTATION_MODEL_ID OVSTAGE_HIERARCHY_COMPUTATION_MODEL_INVALID

/** @brief Backend hierarchy relation to inspect for each input prim. */
typedef enum {
    OVSTAGE_HIERARCHY_PARENT   = 0,  /**< Parent prim path; result count is 0 or 1. */
    OVSTAGE_HIERARCHY_CHILDREN = 1,  /**< Direct child prim paths. */
    OVSTAGE_HIERARCHY_SIBLINGS = 2,  /**< Other prims with the same direct parent. */
} ovstage_hierarchy_relation_t;

/**
 * @brief Per-input hierarchy result metadata.
 *
 * `path_offset` and `path_count` select this input's relation paths from the
 * flattened `ovstage_hierarchy_result_t::paths` array. Empty relations return
 * OVSTAGE_OK with path_count 0. Missing input prims report OVSTAGE_ERROR_NOT_FOUND
 * in the item status without failing the whole fetched batch.
 */
typedef struct {
    ovstage_api_status_t status;
    size_t          path_offset;
    size_t          path_count;
} ovstage_hierarchy_item_t;

/**
 * @brief Result of a fetched hierarchy lookup batch.
 *
 * `items` has one entry for each input path in the submitted
 * `ovx_primpath_list_t`, in the same order. `paths` is a flattened array of
 * ovstage-owned string-or-token views referenced by item offsets/counts.
 * `ordinal` is the requested stage ordinal from ovstage_get_hierarchy. Both
 * arrays remain valid until ovstage_release_hierarchy_result.
 */
typedef struct {
    ovstage_hierarchy_result_id_t hierarchy_result_id;
    ovstage_ordinal_t             ordinal;
    const ovstage_hierarchy_item_t* items;
    size_t                        input_count;
    const ovx_string_or_token_t*  paths;
    size_t                        path_count;
} ovstage_hierarchy_result_t;

/**
 * @brief Description of a runtime-supported hierarchy computation model.
 *
 * `model_id` is one of ovstage_hierarchy_computation_model_id_t. `name` is
 * stable enough for logs/config files owned by the implementation;
 * `description` is human-readable guidance.
 */
typedef struct {
    ovstage_hierarchy_computation_model_id_t model_id;
    ovx_string_t                             name;
    ovx_string_t                             description;
} ovstage_hierarchy_computation_model_desc_t;

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_TYPES_H */
