/* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * ovstage population: populates an ovstage instance from USD.
 *
 * ovstage itself has no USD dependency; the population API is the bridge that
 * reads USD (files or inline USDA) and mirrors it into an ovstage instance
 * so renderers like ovrtx (via ovrtx_attach_ovstage) can consume it.
 *
 * Ordinal ownership. The application — typically a frame coordinator —
 * owns ordinal lifecycle: it advances the write floor for each tick, and
 * hands the current ordinal to the population API. The population API
 * never opens or commits an ordinal of its own; each call is invoked
 * against a caller-provided ordinal so multiple producers can fold into
 * one write floor per tick.
 *
 * Every population entry point that carries an `ordinal` mutates the stage
 * through the ovstage write API, so it is bound by the same admission rule as
 * `ovstage_write_attributes`: the ordinal must be strictly **greater than** the
 * effective write floor at execution time, or the writes are rejected with
 * `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`. A newly created instance starts with a
 * write floor of 0, so the first population must use an ordinal of at least 1;
 * 0 is never a usable write ordinal. (0 remains valid wherever an ordinal is
 * *read* — it is a real ordinal value, not a sentinel.)
 *
 * Stage metadata. Populated onto the root prim, `/`, when something asks for it: a
 * selected domain may bring stage metadata of its own, and a desc names whatever it
 * wants through `stage_metadata_paths`. Each field is populated under the
 * `usd-metadata:<path>` attribute-name prefix. With nothing asking, none is populated.
 * A populated field tracks its USD source across
 * ovstage_population_apply_usd_changes: a changed value is re-read, and a path
 * naming a dictionary follows the dictionary's keys, so a key added since the
 * populate gains an attribute and a key removed loses one.
 *
 * ovrtx pairing contract. An ovstage_population_open_usd_* call or
 * ovstage_population_apply_usd_changes call performs a wholesale or
 * structural stage mutation. A consumer attached in borrow mode
 * (for example, a renderer attached with `ovrtx_attach_ovstage`) must re-anchor
 * its derived state from the shared stage before its next render: the
 * canonical sequence is
 *     `ovstage_population_open_usd_*` /
 *     `ovstage_population_apply_usd_changes` -> wait_op ->
 *     `ovstage_advance_write_floor` -> `ovrtx_update_from_stage` ->
 *     `ovrtx_step_with_stage`.
 * `ovstage_population_apply_usd_time` is time-only and does not require
 * the consumer to rebuild; the consumer's incremental update path is
 * sufficient. No additional ovstage_population_* C entry point is needed
 * for this pairing. No additional public ovstage symbols are required for
 * this integration path.
 */

#ifndef OVSTAGE_POPULATION_H
#define OVSTAGE_POPULATION_H

#include "ovstage_api/ovstage_api.h"
#include "ovstage_population_predicate.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** Coarse population domains for the canonical population entry points.
     *  Bitmask — OR values together.
     *
     *  `OVSTAGE_POPULATION_DOMAIN_NONE` (0) selects no data domain: a `domains`
     *  argument of 0 populates nothing at all. Pass
     *  `OVSTAGE_POPULATION_DOMAIN_ALL` to populate all domains. */
    typedef enum
    {
        OVSTAGE_POPULATION_DOMAIN_NONE      = 0u,
        /** Meshes, lights, materials, and cameras. */
        OVSTAGE_POPULATION_DOMAIN_RENDERING = 1u << 0,
        /** Colliders, rigid bodies, joints, articulations, and the physics
         *  schema attributes/relationships authored on them. */
        OVSTAGE_POPULATION_DOMAIN_PHYSICS   = 1u << 1,
        OVSTAGE_POPULATION_DOMAIN_ALL       = OVSTAGE_POPULATION_DOMAIN_RENDERING |
                                              OVSTAGE_POPULATION_DOMAIN_PHYSICS,
    } ovstage_population_domain_t;

    /** One population selector: which prims are populated, and which of their
     *  properties and metadata. */
    typedef struct ovstage_population_selector_t
    {
        /** Which prims this selector populates. */
        ovstage_population_prim_predicate_t     prim_predicate;

        /** Which properties of those prims are populated. */
        ovstage_population_property_predicate_t property_predicate;

        /** Metadata to populate for each selected prim, as paths: a metadata field
         *  name, extended with a `:`-joined key path to reach inside a dictionary
         *  field. A path naming a dictionary rather than a single value populates
         *  every value beneath it, one attribute each. Populated as
         *  `usd-metadata:<path>`. */
        const ovx_string_t* prim_metadata_paths;
        size_t              prim_metadata_path_count;

        /** Metadata to populate for each selected property, as paths -- same addressing
         *  and dictionary flattening as `prim_metadata_paths`. Applies to every property
         *  `property_predicate` selects; use a further selector to populate a field for
         *  some properties only. Populated as `<property>:usd-metadata:<path>`. */
        const ovx_string_t* property_metadata_paths;
        size_t              property_metadata_path_count;
    } ovstage_population_selector_t;

    /** A complete description of what to populate.
     *
     *  Zero-initialize, then set the fields you need. A desc with no selectors and
     *  `domains` set is exactly equivalent to passing that `domains` to the entry
     *  point without a desc; a desc with neither populates nothing.
     *
     *  Descs are supplied as an array and compose: `domains` is OR-ed across every
     *  desc, and their selectors are combined in array order.
     *
     *  The array, and everything it points at, must stay valid only until the call
     *  returns: the description is copied as the operation is enqueued. */
    typedef struct ovstage_population_desc_t
    {
        /** Bitmask of `ovstage_population_domain_t` — built-in domains to populate
         *  alongside this desc's selectors. `0` for selectors only. */
        uint32_t domains;

        /** The selectors that make up this description. May be `NULL` when `domains`
         *  alone is wanted. */
        const ovstage_population_selector_t* selectors;
        size_t                               selector_count;

        /** Stage metadata to populate onto the root prim, as paths: a stage
         *  metadata field name, extended with a `:`-joined key path to reach inside a
         *  dictionary field — `customLayerData:myTool:version`. A path naming a
         *  dictionary rather than a single value populates every value beneath it, one
         *  attribute each. A path that does not resolve populates nothing. */
        const ovx_string_t* stage_metadata_paths;
        size_t              stage_metadata_path_count;
    } ovstage_population_desc_t;

    /** Opaque id of an enqueued population operation, returned by the async
     *  entry points and passed to `ovstage_population_wait_op`. Zero is never a
     *  live op id. */
    typedef uint64_t ovstage_population_op_id_t;

#define OVSTAGE_POPULATION_INVALID_OP_ID ((ovstage_population_op_id_t)0)

    /** Opaque handle to a USD reference added by
     *  `ovstage_population_add_usd_reference_from_file` / `_from_string`, passed to
     *  `ovstage_population_remove_usd_reference` to take it back out. Reserved synchronously
     *  by the add call (so it is valid immediately, before the op runs) and freed
     *  by `ovstage_population_remove_usd_reference`, `_reset_usd`, or a fresh `_open_usd_*`.
     *  Zero is never a live handle. */
    typedef uint64_t ovstage_population_usd_reference_handle_t;

#define OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE ((ovstage_population_usd_reference_handle_t)0)

    /** Result of enqueuing a population operation.
     *
     *  Population runs asynchronously: `status` reports only that the operation
     *  was accepted (`OVSTAGE_OK`) and the work runs on a background worker —
     *  call `ovstage_population_wait_op(op_id)` to await completion and obtain
     *  the outcome (with detail via `ovstage_population_get_last_error`).
     *  `op_index` identifies the operation for `ovstage_population_wait_op`, or is
     *  `OVSTAGE_POPULATION_INVALID_OP_ID` if nothing was enqueued. */
    typedef struct ovstage_population_enqueue_result_t
    {
        ovstage_api_status_t           status;
        ovstage_population_op_id_t op_index;
    } ovstage_population_enqueue_result_t;

    /** Output of `ovstage_population_wait_op`.
     *
     *  Because a single wait covers all operations up to and including the awaited
     *  op, more than one op can have failed within that range. `error_op_ids`
     *  lists those failed op ids. Each failure is reported exactly once — by the
     *  first `ovstage_population_wait_op` call (on any thread) whose range covers
     *  it — so a later wait does not re-report it. For each id, retrieve the
     *  human-readable detail with `ovstage_population_get_last_op_error`.
     *
     *  The array (and the strings from `ovstage_population_get_last_op_error`) live
     *  in the calling thread's storage and stay valid only until that same thread's
     *  next `ovstage_population_wait_op` call, which overwrites them; copy anything
     *  you need to keep before calling again. Calls on different threads use
     *  independent buffers and do not clobber each other.
     *
     *  `lowest_pending_op_id` is meaningful only when the wait returns
     *  `OVSTAGE_ERROR_TIMEOUT`: it is the lowest op id in the awaited range that
     *  had not completed when the timeout elapsed (useful for partial-progress
     *  reporting). It is `OVSTAGE_POPULATION_INVALID_OP_ID` otherwise. */
    typedef struct ovstage_population_op_wait_result_t
    {
        const ovstage_population_op_id_t* error_op_ids;
        size_t                            error_op_id_count;
        ovstage_population_op_id_t        lowest_pending_op_id;
    } ovstage_population_op_wait_result_t;

    /** The reserved `usd-prim-type` value population authors onto prims that have
     *  no USD type (typeless `def "Foo"` containers). USD allows a defined prim to
     *  carry no type name, but ovstage rows are keyed/created through their prim
     *  type; authoring this synthetic type makes a typeless prim a first-class,
     *  queryable row so prim-tree/hierarchy walks can cross it (e.g. a typeless
     *  `/World` between `/` and its populated descendants). It is namespaced to
     *  never collide with a real USD type, and consumers can filter `usd-prim-type`
     *  against this value to recognize originally-typeless prims.
     *
     *  Returned as an `ovx_string_t` view over a static, null-terminated literal:
     *  `.ptr` is a valid C string and `.length` its byte length. */
    static inline ovx_string_t ovstage_population_untyped_type_name(void)
    {
        static const char name[] = "__ovstage_population_untyped__";
        ovx_string_t result;
        result.ptr = name;
        result.length = sizeof(name) - 1;
        return result;
    }

    /** Reserved prim attributes.
     *
     *  Every populated prim carries `usd-prim-type`, whatever put it in scope and
     *  whatever its properties are. A prim USD gives no type name gets
     *  `ovstage_population_untyped_type_name()` instead, so it is never absent.
     *
     *  `usd-schemas` carries the prim's applied API schema names, and is populated only
     *  when it has any — a prim with none does not carry it at all, rather than carrying
     *  an empty one.
     *
     *  Neither depends on the selector's property predicate. A selector that publishes no
     *  properties at all still yields these. */

    /** Populated value types.
     *
     *  What a USD value becomes in ovstage is fixed by its USD value type, and a caller
     *  selecting arbitrary properties needs it up front to size and interpret a read.
     *  The rules below give the whole mapping; the per-type table is in the population
     *  documentation.
     *
     *  - **Numeric** types keep their bytes. The dtype's element type and bit width come
     *    from the USD scalar type and its lane count from the number of components, so
     *    `float` is `{float, 32, 1}`, `double3` is `{float, 64, 3}`, `int4` is
     *    `{int, 32, 4}`, and `matrix4d` is `{float, 64, 16}`.
     *  - **A USD array** populates with the same dtype as its element type and
     *    `is_array = true`. Everything else populates flat, one fixed-size value per prim.
     *  - **String-like** types do not populate as text. `token`, `string`,
     *    `pathExpression` and the halves of an `asset` are interned through the path
     *    dictionary and populate as `{uint, 64, 1}` ids, which a consumer resolves back
     *    through the dictionary. An `asset` is the exception in width: it carries the
     *    `(authored, resolved)` pair as `{uint, 64, 2}`.
     *  - **An empty** token, string, path expression, or asset half populates as id 0.
     *    It is a value USD authors and reads back, so it is populated rather than
     *    dropped: absence means the property was not authored, not that it was empty.
     *  - **The semantic** records what the bytes mean. It comes from the USD type's role
     *    where it has one (`point3f` to `OVSTAGE_SEMANTIC_POINT`, `color3f` to
     *    `..._COLOR`, `frame4d` to `..._FRAME`), and otherwise from the type itself
     *    (`matrix4d` to `..._MATRIX`, `quatf` to `..._QUATERNION`, `timecode` to
     *    `..._TIME_CODE`, `token` to `..._TOKEN_ID`, `asset` to `..._ASSET_PATH_ID`,
     *    `pathExpression` to `..._PATH_EXPRESSION_STRING`). A relationship's targets
     *    populate as interned path ids with `..._RELATIONSHIP_PATH_ID`.
     *  - **`opaque`** carries no value by definition, so nothing is populated for it and
     *    nothing is reported.
     *
     *  A few properties populate in a type their USD type does not predict, because the
     *  `OVSTAGE_POPULATION_DOMAIN_RENDERING` domain publishes them in a fixed one and
     *  both domains must agree on a shared stage. These are listed in the documentation;
     *  `extent` is the one whose dtype differs rather than only its semantic. */

    /** Path predicates.
     *
     *  `OVSTAGE_POPULATION_PRIM_PREDICATE_HAS_PATH` and `..._IS_UNDER_PATH` take an
     *  absolute prim path, or `/`. A property, target, or variant-selection path is
     *  rejected at enqueue.
     *
     *  Neither matches a prim inside a native instance. */

    /** Populated ancestors.
     *
     *  A prim that `OVSTAGE_POPULATION_DOMAIN_PHYSICS` or a desc's selectors put in
     *  scope is published together with every one of its ancestor prims, so a consumer
     *  can walk the prim tree down to it however sparse the selection is.
     *
     *  An ancestor that no selector matches on its own merit carries none of the data a
     *  selected prim does — no attributes, relationships, or metadata — only the reserved
     *  ones above. It does carry the derived local-transform attributes below when it is
     *  xformable, which is what lets world-transform composition run the whole chain. */

    /** @file ovstage_population.h
     *  Populated USD metadata attributes.
     *
     *  Besides attribute and relationship values, population can surface USD
     *  *metadata* as attributes, under a reserved `usd-metadata:` prefix:
     *
     *    - Prim metadata                 -> `usd-metadata:<field>`
     *    - Property (attribute/relationship) metadata -> `<property>:usd-metadata:<field>`
     *
     *  A metadatum inside a dictionary field (e.g. `customData`) extends
     *  `<field>` with the `:`-joined key path — the same separator USD itself
     *  uses to address subdictionary elements. Example attribute names:
     *    - `usd-metadata:inactiveIds`
     *    - `usd-metadata:customData:physics:localSpaceVelocities`
     *    - `points:usd-metadata:interpolation`
     *
     *  A path with an empty component — a leading, trailing or doubled `:` — names
     *  neither a field nor a key, and is rejected wherever one is taken.
     *
     *  The value uses the same type mapping as attribute values. */

    /** Transform attributes.
     *
     *  For every prim that `OVSTAGE_POPULATION_DOMAIN_PHYSICS` or a desc's selectors
     *  put in scope, and that is USD-xformable, population also *derives* the prim's
     *  local transform. The attributes and their types are a contract consumers can rely
     *  on:
     *
     *    - `omni:xform` — `float64`, 16 lanes, `OVSTAGE_SEMANTIC_MATRIX`. The 4x4
     *      row-major local transform: the prim's USD transform ops composed into a
     *      single matrix. This is the input `ovstage_compute_hierarchy` reads to derive
     *      world transforms.
     *    - `omni:resetXformStack` — `bool`. True when the prim resets the inherited
     *      parent transform stack, which world-transform composition honors.
     *    - `omni:fabric:worldMatrix` — `float64`, 16 lanes, `OVSTAGE_SEMANTIC_MATRIX`.
     *      Created, not maintained, and only when `OVSTAGE_POPULATION_DOMAIN_RENDERING`
     *      is not among the selected domains. `ovstage_compute_hierarchy` updates this
     *      value but does not create it, so population creates it for a qualifying prim
     *      that has none and thereafter leaves it alone: the value is written once, at
     *      creation, as the local transform, and later derived values come from hierarchy
     *      computation. Treat this provisional implementation-named attribute as
     *      read-only output; author transforms through `omni:xform` and
     *      `omni:resetXformStack` instead. A prim that stops being xformable loses it.
     *      With `..._RENDERING` selected that domain owns it outright and population
     *      neither creates nor writes it.
     *
     *  A prim that is not xformable gets none of these. Being xformable does not by
     *  itself put a prim in scope: the derivation only ever applies to prims population
     *  already publishes. */


    /**
     * Register USD schema definitions with the USD runtime population reads
     * through.
     *
     * Population registers only what its USD build provides (the core `Usd*`
     * schemas). Register any additional family your stages use, so population
     * resolves each prim's full schema-declared property set rather than only its
     * authored properties.
     *
     * Registering does not load schema code, so a family that ships a C++
     * library or Python module is usable for its definitions alone.
     *
     * @param paths       Array of @p path_count paths, each a USD plugin
     *                    descriptor (`plugInfo.json`) or a directory containing
     *                    one. A descriptor's `Includes` are followed. Each
     *                    `ovx_string_t` must remain valid until this call returns.
     * @param path_count  Number of entries in @p paths. Zero is a no-op.
     *
     * @return OVSTAGE_OK on success; OVSTAGE_ERROR_INVALID_ARGUMENT for a NULL
     *  @p paths with a non-zero @p path_count or an empty entry;
     *  OVSTAGE_ERROR_NOT_FOUND if an entry names no readable descriptor;
     *  OVSTAGE_ERROR_OP_FAILED if the call came too late (see below). Detail in
     *  `ovstage_population_get_last_error`. Both argument failures are checked
     *  before anything registers, so a rejected call leaves the process
     *  untouched and can be corrected and retried.
     *
     * @remark Only the paths themselves are checked here. What a descriptor
     *  contains is USD's to validate: a malformed one, or an `Includes` pattern
     *  matching nothing, raises a USD diagnostic, contributes no plugins, and
     *  still returns OVSTAGE_OK — surfacing later as schema properties that
     *  never resolve.
     *
     * @remark **Register before the first ovstage call that reads USD schema
     *  definitions** — population and export both do. USD assembles its
     *  schema definitions once, on first read, and ignores plugins registered
     *  after that — a late family registers cleanly and contributes nothing. Such
     *  a call reports OVSTAGE_ERROR_OP_FAILED, and the plugins it registered
     *  cannot be taken back. If another USD consumer in the process reads schemas
     *  first, that cannot be detected and the call still reports success.
     * @remark Process-scoped, thread-safe and irreversible: USD offers no
     *  counterpart removal. Re-registering a family is a no-op, not an error.
     * @remark Callable on its own: it requires no prior ovstage_initialize() and
     *  no live ovstage instance.
     * @remark Registering a family makes its definitions resolvable. It does not
     *  change which prims a population domain claims.
     */
    ovstage_api_status_t ovstage_population_register_usd_schemas(
        const ovx_string_t* paths,
        size_t              path_count);

    /**
     * Enqueue an asynchronous operation to populate an ovstage instance from a
     * USD file.
     *
     * Replaces any USD content previously loaded into this ovstage with the
     * contents of @p path. This both loads the USD and populates the ovstage in
     * one operation. Initial population is one-shot; live edits
     * made afterwards are picked up by `ovstage_population_apply_usd_changes` /
     * `ovstage_population_apply_usd_time`.
     *
     * Per-stage population state is created lazily on first use and released
     * automatically when @p stage is destroyed.
     *
     * @param stage      ovstage instance to populate into.
     * @param path       Filesystem path to a `.usda` / `.usdc` / `.usd` file, or
     *                   an Omniverse Nucleus URL (`omniverse://server/path.usd`),
     *                   as an `ovx_string_t` view.
     * @param ordinal    Accumulating ordinal owned by the caller. Must be
     *                   greater than the effective write floor at execution
     *                   time; a new instance's floor is 0, so initial
     *                   population starts at 1.
     * @param time       Time in seconds at which to evaluate attributes;
     *                   converted to a USD time code via the stage's
     *                   timeCodesPerSecond.
     * @param domains    Bitmask of `ovstage_population_domain_t` selecting which
     *                   data domains to populate. `0` (DOMAIN_NONE) populates
     *                   nothing; pass `OVSTAGE_POPULATION_DOMAIN_ALL` for
     *                   everything.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the stage cannot be populated, the file cannot be opened, or population
     *  failed — are surfaced by `ovstage_population_wait_op`, with detail in
     *  `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_open_usd_from_file(
        ovstage_instance_t* stage,
        ovx_string_t        path,
        ovstage_ordinal_t   ordinal,
        double              time,
        uint32_t            domains);

    /**
     * Enqueue an asynchronous operation to populate an ovstage instance from an
     * inline USDA string.
     *
     * Same semantics as `ovstage_population_open_usd_from_file` but the USD content is
     * provided directly as text instead of loaded from disk.
     *
     * @param stage      ovstage instance to populate into.
     * @param usda       USDA content (e.g. "#usda 1.0\n...") as an `ovx_string_t` view.
     * @param ordinal    Accumulating ordinal owned by the caller. Must be
     *                   greater than the effective write floor at execution
     *                   time; a new instance's floor is 0, so initial
     *                   population starts at 1.
     * @param time       Time in seconds; converted to a USD time code via the
     *                   stage's timeCodesPerSecond.
     * @param domains    Bitmask of `ovstage_population_domain_t` selecting which
     *                   data domains to populate. `0` (DOMAIN_NONE) populates
     *                   nothing; pass `OVSTAGE_POPULATION_DOMAIN_ALL` for
     *                   everything.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the stage cannot be populated, the USDA content fails to parse, or
     *  population failed — are surfaced by `ovstage_population_wait_op`, with
     *  detail in `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_open_usd_from_string(
        ovstage_instance_t* stage,
        ovx_string_t        usda,
        ovstage_ordinal_t   ordinal,
        double              time,
        uint32_t            domains);

    /**
     * Enqueue an asynchronous operation to populate an ovstage instance from a
     * USD file, with an explicit population description.
     *
     * Identical to `ovstage_population_open_usd_from_file` except that what to populate
     * is given as an array of `ovstage_population_desc_t` rather than a bare `domains`
     * bitmask. A single desc that carries only `domains` is exactly the shorter entry
     * point.
     *
     * @param stage      ovstage instance to populate into.
     * @param path       Filesystem path to a `.usda` / `.usdc` / `.usd` file, or
     *                   an Omniverse Nucleus URL (`omniverse://server/path.usd`),
     *                   as an `ovx_string_t` view.
     * @param ordinal    Accumulating ordinal owned by the caller. Must be
     *                   greater than the effective write floor at execution
     *                   time; a new instance's floor is 0, so initial
     *                   population starts at 1.
     * @param time       Time in seconds at which to evaluate attributes;
     *                   converted to a USD time code via the stage's
     *                   timeCodesPerSecond.
     * @param descs      Array of descriptions selecting what to populate; their
     *                   `domains` are OR-ed together and their selectors combined.
     *                   Must stay valid only until this call returns. May be `NULL`
     *                   when @p desc_count is 0.
     * @param desc_count Number of entries in @p descs. `0` (or a `NULL` @p descs)
     *                   selects nothing, as `OVSTAGE_POPULATION_DOMAIN_NONE` does.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the stage cannot be populated, the file cannot be opened, or population
     *  failed — are surfaced by `ovstage_population_wait_op`, with detail in
     *  `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_open_usd_from_file_with_desc(
        ovstage_instance_t*                    stage,
        ovx_string_t                           path,
        ovstage_ordinal_t                      ordinal,
        double                                 time,
        const ovstage_population_desc_t*       descs,
        size_t                                 desc_count);

    /**
     * Enqueue an asynchronous operation to populate an ovstage instance from an
     * inline USDA string, with an explicit population description.
     *
     * Same semantics as `ovstage_population_open_usd_from_file_with_desc` but the USD
     * content is provided directly as text instead of loaded from disk.
     *
     * @param stage      ovstage instance to populate into.
     * @param usda       USDA content (e.g. "#usda 1.0\n...") as an `ovx_string_t` view.
     * @param ordinal    Accumulating ordinal owned by the caller. Must be
     *                   greater than the effective write floor at execution
     *                   time; a new instance's floor is 0, so initial
     *                   population starts at 1.
     * @param time       Time in seconds; converted to a USD time code via the
     *                   stage's timeCodesPerSecond.
     * @param descs      Array of descriptions selecting what to populate; their
     *                   `domains` are OR-ed together and their selectors combined.
     *                   Must stay valid only until this call returns. May be `NULL`
     *                   when @p desc_count is 0.
     * @param desc_count Number of entries in @p descs. `0` (or a `NULL` @p descs)
     *                   selects nothing, as `OVSTAGE_POPULATION_DOMAIN_NONE` does.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the stage cannot be populated, the USDA content fails to parse, or
     *  population failed — are surfaced by `ovstage_population_wait_op`, with
     *  detail in `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_open_usd_from_string_with_desc(
        ovstage_instance_t*                    stage,
        ovx_string_t                           usda,
        ovstage_ordinal_t                      ordinal,
        double                                 time,
        const ovstage_population_desc_t*       descs,
        size_t                                 desc_count);

    /**
     * Enqueue an asynchronous operation to add a USD file as a reference at
     * @p target_path.
     *
     * This edits only the USD source, additively — existing USD content is left
     * untouched. The shape of the merge depends on @p target_path (see below): a new
     * prim is defined and @p ref_file_path referenced on it, the reference is added
     * onto an existing prim, or — for the root "/" — the content is merged in at the
     * top level. Like `ovstage_population_reset_usd`, it does not itself touch the
     * ovstage; call `ovstage_population_apply_usd_changes` afterwards to propagate the
     * added subtree into the ovstage (at the time and domains already in effect from
     * the initial `_open_usd_*`).
     *
     * Requires existing population state: call one of the `_open_usd_*` entry
     * points first. The enqueue is rejected (`status`) if this stage has never been
     * populated.
     *
     * @param stage         ovstage instance.
     * @param ref_file_path Filesystem path to a `.usda` / `.usdc` / `.usd` file, or an
     *                    Omniverse Nucleus URL (`omniverse://server/path.usd`), as an
     *                    `ovx_string_t` view — the layer to reference.
     * @param target_path Absolute path (starting with '/') selecting how the content
     *                    is merged, additively in every case:
     *                    - the root "/" — the content is merged in at the top level
     *                      (its top-level prims are added to the stage without
     *                      clobbering the existing stage's units/upAxis/defaultPrim);
     *                    - an existing prim path — the reference is added onto that
     *                      prim and the referenced subtree composes in beneath it; the
     *                      prim and its prior content are left in place;
     *                    - a not-yet-existing prim path — a new prim is defined there
     *                      (any missing ancestors are defined) and the layer referenced on it.
     * @param out_handle  [out, optional] Receives a handle for the added reference,
     *                    reserved synchronously (valid before the op runs) for a
     *                    later `ovstage_population_remove_usd_reference`. Set to
     *                    `OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE` if the enqueue is
     *                    rejected. A non-invalid handle does not by itself mean the
     *                    reference was added — await the op to confirm.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the file cannot be opened, or the target path is not a valid absolute path —
     *  are surfaced by `ovstage_population_wait_op`, with detail in
     *  `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_add_usd_reference_from_file(
        ovstage_instance_t*              stage,
        ovx_string_t                     ref_file_path,
        ovx_string_t                     target_path,
        ovstage_population_usd_reference_handle_t* out_handle);

    /**
     * Enqueue an asynchronous operation to add inline USDA content as a reference
     * at @p target_path.
     *
     * Same semantics as `ovstage_population_add_usd_reference_from_file` but the
     * USD content is provided directly as text instead of loaded from disk. The
     * inline layer is held alive for as long as the reference exists (until
     * `ovstage_population_remove_usd_reference` / `_reset_usd` / a fresh `_open_usd_*`).
     *
     * @param stage       ovstage instance.
     * @param ref_str     USDA content (e.g. "#usda 1.0\n...") as an `ovx_string_t`
     *                    view — the layer to reference.
     * @param target_path Absolute path (starting with '/') selecting how the content
     *                    is merged, additively in every case — the root "/" merges the
     *                    content at the top level, an existing prim path adds the
     *                    reference onto that prim, a not-yet-existing prim path defines
     *                    a new prim there. See
     *                    `ovstage_population_add_usd_reference_from_file`.
     * @param out_handle  [out, optional] Receives a handle for the added reference,
     *                    reserved synchronously, for a later
     *                    `ovstage_population_remove_usd_reference`. See
     *                    `ovstage_population_add_usd_reference_from_file`.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue is reported in `status`; failures while the op runs —
     *  the USDA content fails to parse, or the target path is not a valid absolute
     *  path — are surfaced by `ovstage_population_wait_op`, with detail in
     *  `ovstage_population_get_last_error`.
     */
    ovstage_population_enqueue_result_t ovstage_population_add_usd_reference_from_string(
        ovstage_instance_t*              stage,
        ovx_string_t                     ref_str,
        ovx_string_t                     target_path,
        ovstage_population_usd_reference_handle_t* out_handle);

    /**
     * Enqueue an asynchronous operation to remove a USD reference previously added
     * by `ovstage_population_add_usd_reference_from_file` / `_from_string`.
     *
     * This edits only the USD source, undoing exactly what the matching add
     * introduced: a prim the add defined is removed whole (with its referenced
     * subtree); a reference the add placed onto a pre-existing prim is removed on its
     * own, leaving that prim and its prior content in place; a root "/" merge removes
     * just the sublayer the add inserted. Like the add, it does not itself touch the
     * ovstage; call `ovstage_population_apply_usd_changes` afterwards to propagate the
     * removal (the affected prims are tombstoned out of the ovstage). For an inline
     * (`_from_string`) reference this also releases the held layer. Carries no ordinal.
     *
     * @param stage   ovstage instance.
     * @param handle  Handle returned by an add-reference call. Removing the same
     *                handle twice, or a handle whose add never completed, fails
     *                when the op runs.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue (invalid handle, or no population state) is reported in
     *  `status`; a run-time failure (unknown/already-removed handle, or the prim
     *  could not be removed) is surfaced by `ovstage_population_wait_op`.
     */
    ovstage_population_enqueue_result_t ovstage_population_remove_usd_reference(
        ovstage_instance_t*             stage,
        ovstage_population_usd_reference_handle_t handle);

    /**
     * Enqueue an asynchronous operation to reset (clear) all USD source content
     * from this stage.
     *
     * Unlike the open entry points, which load and populate in one step, this
     * edits only the USD source: call `ovstage_population_apply_usd_changes`
     * afterwards to propagate the cleared state into the ovstage, or repopulate
     * with `ovstage_population_open_usd_from_file` / `_open_usd_from_string`.
     * Carries no ordinal.
     *
     * @param stage  ovstage instance.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue (no population state) is reported in `status`; a run-time
     *  failure is surfaced by `ovstage_population_wait_op`.
     */
    ovstage_population_enqueue_result_t ovstage_population_reset_usd(
        ovstage_instance_t* stage);

    /**
     * Enqueue an asynchronous operation to advance the time and propagate
     * time-sampled attribute changes into the ovstage. Call once per simulation
     * tick when playing back time-sampled USD content.
     *
     * Samples time-sampled attributes from the latest USD state at @p time: their
     * values are re-sampled from the current USD, so edits to an attribute's time
     * samples are reflected at the new time even if
     * `ovstage_population_apply_usd_changes` has not been called since they were made.
     *
     * Re-samples whatever the initial `_open_usd_*` put in scope, not only the
     * domains it selected: a `_with_desc` open puts prims in scope through its
     * selectors as well, so a description carrying selectors and `domains == 0` is
     * advanced like any other. Nothing outside that scope is written.
     *
     * RENDERING — additional behavior: for the RENDERING domain this call also
     * reflects pending *structural* USD edits (prim add/remove, and attribute or
     * relationship changes that are not time samples) at the new time, even if
     * `ovstage_population_apply_usd_changes` has not been called. Other domains advance
     * time for time-sampled values only and leave such structural edits to be applied
     * by `ovstage_population_apply_usd_changes`. This RENDERING-specific behavior is
     * expected to converge with the other domains over time.
     *
     * @param stage      ovstage instance.
     * @param ordinal    Accumulating ordinal owned by the caller. Must be
     *                   greater than the effective write floor at execution
     *                   time.
     * @param time       New time in seconds; converted to a USD time code via
     *                   the stage's timeCodesPerSecond.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue (no population state) is reported in `status`; a run-time
     *  failure is surfaced by `ovstage_population_wait_op`.
     */
    ovstage_population_enqueue_result_t ovstage_population_apply_usd_time(
        ovstage_instance_t* stage,
        ovstage_ordinal_t   ordinal,
        double              time);

    /**
     * Enqueue an asynchronous operation to propagate any USD edits accumulated
     * since the last call (including content emptied by
     * `ovstage_population_reset_usd`) into the ovstage.
     *
     * Intended for frame-coordinator pre-commit hooks: call once per tick
     * before consumers read from the ovstage so any pending USD-side edits
     * are visible. A no-op (returns success) when nothing has changed.
     *
     * Honors the data domains selected by the initial `_open_usd_*`: only those
     * domains' edits are propagated.
     *
     * @param stage    ovstage instance.
     * @param ordinal  Accumulating ordinal owned by the caller. Must be greater
     *                 than the effective write floor at execution time.
     * @return Enqueue result (see `ovstage_population_enqueue_result_t`). A
     *  rejected enqueue (no population state) is reported in `status`; a run-time
     *  failure is surfaced by `ovstage_population_wait_op`. A no-op (nothing
     *  changed) returns `OVSTAGE_OK`.
     */
    ovstage_population_enqueue_result_t ovstage_population_apply_usd_changes(
        ovstage_instance_t* stage,
        ovstage_ordinal_t   ordinal);

    /**
     * Wait for completion of all operations up to and including @p op_id, or until
     * @p timeout_ns elapses. Because the wait covers earlier operations too, any of
     * them may be the one that failed — see @p out_wait_result.
     *
     * `timeout_ns = 0` polls; `OVSTAGE_TIMEOUT_INFINITE` blocks indefinitely.
     *
     * @param stage            ovstage instance.
     * @param op_id            Operation id from a population enqueue result.
     * @param timeout_ns       Nanoseconds to wait; `OVSTAGE_TIMEOUT_INFINITE` blocks.
     * @param out_wait_result  [out, optional] Receives the ids of every op that
     *                         failed within the awaited range and, on timeout, the
     *                         lowest still-pending op id. May be `NULL` if the
     *                         caller only needs the return code. See
     *                         `ovstage_population_op_wait_result_t`.
     * @return
     *  - `OVSTAGE_OK` if the awaited op (and all ops before it) completed
     *    successfully.
     *  - `OVSTAGE_ERROR_TIMEOUT` if the range did not complete within @p timeout_ns.
     *  - `OVSTAGE_ERROR_OP_FAILED` if any op in the range failed; the failing ids
     *    are reported in @p out_wait_result, with per-op detail available from
     *    `ovstage_population_get_last_op_error` and the primary failure mirrored
     *    into `ovstage_population_get_last_error`.
     *  - `OVSTAGE_ERROR_INVALID_ARGUMENT` for an unknown op id (zero, or never
     *    enqueued), or when @p stage has no population state.
     */
    ovstage_api_status_t ovstage_population_wait_op(
        ovstage_instance_t*                  stage,
        ovstage_population_op_id_t           op_id,
        ovstage_timeout_ns_t                 timeout_ns,
        ovstage_population_op_wait_result_t* out_wait_result);

    /** Returns the thread-local error string for the latest population call on
     *  this thread, regardless of which ovstage instance produced it. For an
     *  op that failed asynchronously, this holds the primary failure only after
     *  `ovstage_population_wait_op` has observed it on this thread; for a rejected
     *  enqueue it is set on return. Valid until the next population call on the
     *  same thread. Returns `{NULL, 0}` when no error has been recorded. */
    ovx_string_t ovstage_population_get_last_error(void);

    /** Returns the human-readable error string for @p op_id, as reported by the
     *  last `ovstage_population_wait_op` call on this thread (i.e. an op id listed
     *  in that call's `ovstage_population_op_wait_result_t::error_op_ids`). Valid
     *  until the next `ovstage_population_wait_op` call on the same thread. Returns
     *  `{NULL, 0}` if @p op_id is not known to have failed on this thread. */
    ovx_string_t ovstage_population_get_last_op_error(ovstage_population_op_id_t op_id);

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_POPULATION_H */
