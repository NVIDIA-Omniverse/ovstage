# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/) and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.0] - 2026-10-13

### Summary

- Feature release. Ordinal admission is now nondecreasing by default; reads,
  writes and maps gain experimental CPU/CUDA placement; attribute schemas can be
  replaced explicitly; USD population exposes richer metadata; and USD data can
  be exported into existing layers and stages in ovstage's USD runtime.

- C/C++ callers must rebuild for changed descriptor layouts. Python APIs,
  tensor layouts and runtime behavior also have breaking changes; see the
  **Breaking** entries below.

### Added

- **Data plane**

  - **Experimental CPU/CUDA placement for reads, writes and maps.**
    `ovstage_placement_t` and Python `Placement` select a device copy and
    stream. Placed reads use a current device copy when available; writes
    transfer to the requested device and maps expose writable staging there.
    Caller-owned availability events support device-side synchronization; check
    operation status separately. Map events are recorded at commit. C callers
    explicitly include `ovstage_placement_extension.h` from the experimental
    headers and discover placement through
    `ovstage_query_placement_extension()`. Missing or incompatible extension
    versions report `OVSTAGE_ERROR_NOT_FOUND`. Placement preserves the core
    vtable ABI; its experimental API, ABI and semantics may change between
    releases.
  - **Inspect clone completion.** `ovstage_fetch_clone_result()` and Python
    `CloneOperation.result()` report cloned prims, target/source counts and
    ordinal.
  - **Read relationship and connection columns.** Single-path relationship
    columns support `OVSTAGE_SEMANTIC_RELATIONSHIP_PATH_ID` with
    `is_array = false` in writes, maps, reads and USD export. CPU reads and
    deletes support connection columns; reads return target-path and
    target-attribute token pairs. GPU connection reads remain unsupported.

- **USD population and export**

  - **Read authored physics data alongside rendering output.** PHYSICS
    population publishes `usd-metadata:typeName` on typed prims, including
    structural ancestors. Where PHYSICS or a selector publishes a
    PointInstancer's `prototypes`, `prototypes:usd-targets` preserves the
    authored target order indexed by `protoIndices`. Live edits update both
    authored-data columns, including removal of a type name or clearing
    prototype targets.
  - **Repopulate from the current USD source.**
    `ovstage_population_repopulate_usd_with_desc()` and Python
    `population.repopulate_usd_with_desc()` rewrite selected values from USD
    while retaining source layers and references. Direct overrides of those
    values are replaced; content outside the description is not removed. The
    descriptions replace the scope and domains of subsequent USD updates.
  - **Export into USD layers and stages already open in ovstage.**
    `ovstage_population_export_to_usd_layer_identifier()` and
    `ovstage_population_export_to_usd_stage_identifier()` have asynchronous and
    Python equivalents. The destination must be retained in ovstage's USD
    runtime; an unopened identifier reports `OVSTAGE_ERROR_INVALID_ARGUMENT`.
    Export authors in memory and never saves.
  - **Query USD type ancestry and applied schemas.** Population records known
    type ancestry in `usd-ancestor-types` and bare multiple-apply schema names
    alongside their instances in `usd-schemas`. Rendering population retains its
    documented type/tag conventions; export omits bare schema names.
  - `OVSTAGE_POPULATION_DEFAULT_TIME` / `population.DEFAULT_TIME` name USD
    default time; test it with `ovstage_population_is_default_time()` /
    `population.is_default_time()`, not equality, because its value is NaN.

- **Runtime and tools**

  - **Inspect the loaded build and configuration.** `ovstage_get_build_info()` /
    `ovstage.get_build_info()` report build identity before initialization;
    `ovstage_get_config()` / `Stage.config` expose captured explicit
    configuration.
  - Expanded documentation with a runtime overview and C/Python walkthroughs
    for population, transforms, USD export and CUDA writes.
  - A benchmarking guide covers the packaged benchmark executable, filtering
    and platform setup.
  - Python data-plane operation and handle `wait()` methods return their
    receiver for chaining. Population-operation return values are unchanged.
  - Python `ReadGroup.semantic` exposes the returned attribute semantic.

### Changed

- **Data plane**

  - **Breaking: nondecreasing ordinal admission by default.** `NONDECREASING`
    rejects mutations older than retained changes to affected attributes or prim
    creation/deletion history. Equal ordinals remain allowed above applicable
    write floors. Aliases share floors, admission history and overlap checks.
    Configure `OVSTAGE_CONFIG_RUNTIME_DEFAULT_ORDINAL_ADMISSION_POLICY` in C or
    `StageConfig.runtime_default_ordinal_admission_policy` in Python;
    alternatives are `STRICTLY_INCREASING` and `FLOOR_ONLY`. Enqueue may succeed
    before a history conflict is detected; `wait()` then reports
    `OVSTAGE_ERROR_OP_FAILED`.
  - **Breaking: snapshot reads now honor their end ordinal.** 0.2.1 returned
    current values regardless of the snapshot end. A queried attribute/path with a
    change above its write floor at or before the snapshot end reports
    `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`. Otherwise, a retained change after
    the end reports `OVSTAGE_ERROR_OUT_OF_RANGE`, even if unsealed. For current
    values use `UINT64_MAX` (Python `OrdinalRange.latest((1 << 64) - 1)`). A
    zero-initialized range requests ordinal 0. Below the
    retained-history boundary, success cannot guarantee the earlier state.
  - **Breaking (C ABI): replace schemas during writes/maps.**
    `OVSTAGE_ATTRIBUTE_MODE_FORCE` replaces a conflicting schema; `REJECT`
    remains the default. `attribute_mode` is appended to `ovstage_write_data_t`
    and `ovstage_map_desc_t`, preserving existing field offsets.
    `ovstage_write_data_t` and `ovstage_attribute_write_t` grow. Rebuild C/C++
    callers and zero-initialize descriptors to preserve REJECT. FORCE maps
    require `dtype` and replace at commit.
  - **Breaking: read/map tensor layouts.** Raw DLTensor outputs now use scalar
    lanes and explicit component axes: a `float3` changes from `[N]` with 3
    lanes to `[N,3]` with 1 lane; a float64 4x4 `MATRIX` changes from `[N]` with
    16 lanes to `[N,4,4]` with 1 lane. Outputs provide explicit compact strides.
    **Write inputs and map-creation dtypes remain lane-based.** Use
    `ovstage_tensor_to_write()` / `typed_tensor_to_write()` to forward
    descriptors; the C helper requires
    `<ovstage/ovstage_api/ovstage_tensor_utils.h>`. Python `ReadGroup.array()` and
    `MapGroup.array()` now return shaped views instead of flat arrays; use
    `.reshape(-1)` for flat indexing. Include all shape dimensions in counts.
  - **Breaking: instance-owned path dictionaries.** Path lists belong to one
    instance and cannot be shared across instances or outlive their owner.
    Python `PathDictionary` requires a `Stage`. *Migration:* create lists in the
    destination stage's dictionary and release them before destroying that
    stage. Tokens and paths are guaranteed valid only for their owning instance's
    lifetime; intern them through the instance that uses them, and do not retain
    its dictionary pointer after its final owning handle is released.
  - **Breaking: connection writes reject surplus pairs.** Supply exactly one
    pair per target prim. Surplus pairs in shared input, previously ignored, now
    fail during execution; `ovstage_wait_op()` / Python `wait()` reports
    `OVSTAGE_ERROR_OP_FAILED`.
  - **Breaking: single-path relationship read semantics.** These columns now
    report `OVSTAGE_SEMANTIC_RELATIONSHIP_PATH_ID` instead of `NONE`; their
    64-bit payload is unchanged. Update code that branches on the returned
    semantic.
  - Mapped prim/attribute creation, `HAS` membership and hierarchy changes
    become visible at successful group/final unmap rather than during setup.
    Failed commits may leave partial changes without completed change records;
    close the failed session and repair with a new map or write.
  - **Reduce query/read and write overhead.** Query creation/refresh,
    unchanged-range reads, attribute lookup/discovery and dense writes avoid
    redundant work, including writes that create prims and columns.
  - Queries and reads can progress during OVRTX rendering of an attached stage,
    subject to operation ordering and overlap checks.

- **USD population and instancing**

  - **Breaking: combined population retains rendering values.** With RENDERING
    also populated, Imageable `purpose` and a PointInstancer's `extent` and
    `prototypes` retain rendering values instead of authored USD values. Read
    authored prototype targets from `prototypes:usd-targets`.
  - **Breaking: opening USD merges into existing content.** An open overwrites
    populated values but no longer clears unrelated prims or attributes.
    *Migration:* explicitly delete existing content first when replacement is
    intended. Resetting the USD source requires `apply_usd_changes` to propagate
    the clear; use an earlier ordinal for that clear than for the new open.
  - **Breaking: prototype-root paths are opaque.** Combined rendering and
    physics/selector population shares one prototype root and one prototype link
    per instance root. *Migration:* resolve roots through instancing queries
    instead of constructing USD `/__Prototype_N` paths. The new mapping
    preserves prototype identity across deactivate/reactivate cycles and
    metadata edits.
  - **Breaking: instance-root metadata uses instance types across domains.**
    PHYSICS and selector population now report `XformInstance` or
    `ScopeInstance` in `usd-prim-type`, add instance API tags to `usd-schemas`,
    and use only `Xformable` in `usd-ancestor-types`. Instances below a
    `PointInstancer` retain their authored type and applied schemas instead;
    their prototype links remain available to instancing queries. *Migration:*
    read the authored type from `usd-metadata:typeName`; type filters see the
    instance type. Inferred typed export recovers the authored type only where
    that metadata is populated, preserves schema-declared properties, omits
    synthetic instance tags, and does not recreate instance references.
    RENDERING alone may classify a Scope and non-Scope sharing a prototype by the
    first instance encountered; PHYSICS and selectors classify each root
    separately.
  - **Breaking: selected string fields now read as UTF-8 bytes.** If your
    selectors read render settings/products/vars, material network strings,
    valued scalar string primvars or supported camera/sensor fields as `uint64`
    IDs, decode UTF-8 byte arrays with `OVSTAGE_SEMANTIC_STRING` instead. String
    arrays, legacy semantic fields and unrelated strings retain interned IDs.
  - **Breaking: PHYSICS/selector attribute semantics.** The legacy
    `SemanticsAPI` fields `semanticType` and `semanticData` now report
    `OVSTAGE_SEMANTIC_TOKEN_ID` instead of `NONE`. On supported shaped lights,
    `inputs:shaping:focusTint` reports `VECTOR` instead of `COLOR`. Payloads are
    unchanged.
  - **Breaking (Python): population ordinals and time keyword.** `open_usd`,
    `open_usd_from_string`, `apply_usd_changes` and their asynchronous forms
    require `ordinal`; none is generated. Population's `time_code` keyword is
    renamed `time`. Values remain seconds, converted through
    `timeCodesPerSecond`. *Migration:* pass caller-coordinated ordinals and
    replace `time_code=` with `time=`; positional time arguments are unchanged.
  - **Breaking: empty schema sets are now published.** Generic population
    publishes `usd-schemas` even for prims with no applied schemas, so `HAS` now
    selects those prims too. Use `CONTAINS` to require a specific schema.
    RENDERING-only population retains its previous presence behavior.
  - **Breaking: quaternion export uses `(x, y, z, w)` semantic lane order.**
    Remove workarounds for the former `(w, x, y, z)` interpretation. Explicit
    quaternion rules on untyped tuples keep their compatibility order.
  - **Breaking: PHYSICS/selector schema values use their declared type.**
    Compatible numeric shapes are converted, including float-to-integer
    truncation and rounding to narrower floating-point types. Unconvertible
    values are skipped with a warning, retaining prior values; 0.2.1 published
    them in their authored type. Check for precision loss; export uses the
    populated values. Attributes without a schema declaration retain their
    authored numeric type.
  - **Breaking: STRING export rejects trailing NUL terminators.** 0.2.1 stripped
    a trailing NUL; it now makes the row unsupported, or fails export with
    `REQUIRED`. Write length-delimited STRING rows without a C terminator.
  - **Breaking: population/export wait errors.** Foreign or stale operation IDs
    report `OVSTAGE_ERROR_INVALID_HANDLE` instead of `INVALID_ARGUMENT`. Update
    error-code checks.

- **Runtime and tools**

  - **Breaking (Python):** `Stage.clone()` returns `CloneResult` instead of
    `None`.
  - **Breaking for benchmark filters:** benchmark names are renamed to public
    terminology. *Migration:* refresh filters from the executable's benchmark
    listing; benchmark behavior is unchanged.
  - **Breaking: initialization rejects unknown config keys.**
    `ovstage_initialize()` returns `OVSTAGE_ERROR_INVALID_ARGUMENT` for
    unsupported configuration entries; remove them.
  - **Breaking (Windows loader/Python):** the ovstage loader resolves
    `ovstage.dll` imports from its directory, registered package/user DLL
    directories and System32, not `PATH`, the application or current directory.
    Register extra locations with `AddDllDirectory` / `os.add_dll_directory`.
    Applications linking `ovstage.dll` directly are unaffected by this change.

### Removed

- **Breaking:** `ovstage_population_untyped_type_name()` and the synthetic
  `__ovstage_population_untyped__` type. Typeless USD prims remain typeless.
  *Migration:* handle absent `usd-prim-type` rather than matching the
  sentinel.
- **Breaking (C/C++ source):** `ovalign.h` and its unimplemented declarations
  are removed. Remove `#include <ovstage/ovalign.h>`; no exported symbol is
  removed.

### Fixed

- **Data plane**

  - Unmapping no longer fails because unrelated prims gained the same attribute.
  - **Track cloned attributes accurately.** Clones include copied type/schema
    metadata, connection-only columns and mixed-type attributes in change
    tracking. Clones reject duplicate or invalid target paths, overlapping
    derived target subtrees and incompatible leftover descendants.
  - Named deletes remove `usd-prim-type` and `usd-schemas` and record their
    removal.
  - Repeatedly creating and releasing queries against an unchanged stage no
    longer accumulates query-cache memory.
  - Paths created from components are absolute and agree with equivalent string
    paths; an empty component sequence resolves to `/`.

- **USD population, instancing and hierarchy**

  - **Preserve combined rendering and physics/selector scenes.** Population
    retains rendering purpose, point-instancer prototype geometry and bounds,
    plus supported tags for primvars, mesh subdivision, skinned geometry,
    material bindings, prototype geometry, skeletons and lights through USD
    edits. It avoids removing rendered rows when a generic description stops
    selecting them. Derived transform columns are protected from same-name USD
    properties.
  - **USD edits no longer remove still-referenced scene content.** Resyncs
    retain relationship targets, live instance roots and prims retained by later
    edits in the same batch. Whole-stage resyncs refresh affected root prims and
    prototypes; instance ancestors are populated.
  - Unrelated ancestor resyncs preserve unchanged material subset meshes;
    deleting an untyped instance preserves shared prototype geometry still used
    by surviving instances.
  - PHYSICS/selector population follows attribute type/role changes using schema
    replacement.
  - Mapped transform/reset-stack edits refresh derived transforms on supported
    computation triggers. Compute hierarchy before advancing the write floor;
    0.2.1 documented sealing first. Hierarchy computation records rewritten
    `omni:fabric:worldMatrix` rows at its output ordinal, making them visible to
    change-range consumers.
  - Concurrent ovstage instance creation, population and destruction now
    synchronize access to the shared USD notice-handler registry.
  - RENDERING time updates resize time-varying skeleton-animation arrays to the
    sampled values, including the first update after default-time population.
  - RENDERING value updates preserve the roles of scalar `float3` attributes;
    newly introduced non-schema scalar `float3` attributes use their authored
    USD role.
  - RENDERING time updates reapply time-sampled values even at the same time
    and restore default opinions when applying `DEFAULT_TIME`.
  - RENDERING purpose inheritance honors an explicitly authored USD `default`.
    PHYSICS/selector population now honors supported authored legacy light
    parameters when their `inputs:*` counterpart is unauthored; RENDERING live
    edits follow the same precedence.
  - USD schema registration follows linked descriptor files and directories. A
    descriptor's relative `Root`, `ResourcePath` and `Includes` resolve beside
    the link, not its target; link the containing directory or include the
    referenced files alongside the descriptor. Dangling links still report
    `OVSTAGE_ERROR_NOT_FOUND`.

- **USD export**

  - **Preserve USD export values.** Export handles booleans, half quaternions,
    authored empty arrays, pseudo-root metadata and empty-token schema fallbacks
    correctly. Semantic UTF-8 strings export as strings; embedded NUL bytes are
    reported as unsupported instead of silently truncated. Typed export maps
    rendering's `geometry` purpose to USD `default`.

- **Python and runtime**

  - **Keep Python operation and map resources alive.** Python groups fetched
    through a `Map` retain it. Accepted group unmaps and finalization through
    that `Map` invalidate subsequent tensor/index access; raw-handle
    finalization and previously obtained views remain caller-managed. Python
    operation timeouts preserve pending-operation ownership and input buffers,
    allowing another wait instead of consuming the operation prematurely.
  - Standalone Windows assertion failures no longer leave headless processes
    blocked behind a modal dialog.

### Limitations

- **Data retention and device placement**

  - The default runtime retains latest payloads and bounded change metadata, not
    historical payload snapshots. Query the oldest retained ordinal before
    relying on earlier state.
  - Range-read performance depends on attribute storage: mixed-type attributes
    still scan all rows. Discovered attribute tokens live with their stage's
    dictionary; query lookup storage remains allocated while the query is
    retained.
  - CUDA reads do not support array-valued attributes (including mesh `points`),
    mixed-type columns, connections or reserved `usd-*` metadata. CUDA maps do
    not support array-valued attributes. These cases report
    `OVSTAGE_ERROR_NOT_SUPPORTED`. Keep borrowed write tensors alive until the
    availability event completes.

- **Population and hierarchy**

  - RENDERING population does not fully enforce attribute write floors or
    ordinal admission against other writers. Explicit hierarchy computation
    does not enforce write floors. Coordinate both with writers and consumers.
  - RENDERING does not perform PHYSICS/selector numeric schema conversion;
    mismatched authored types can yield schema fallbacks or empty/zero-filled
    arrays.
  - USD edits that resync an existing populated prim, including adding or
    removing a reference, can republish its selected subtree and replace direct
    writes. PHYSICS/selector population records rewritten values at the apply
    ordinal, including unchanged values; RENDERING rewrites are not reported
    by the same change tracking. Use new prim paths for new content to avoid
    resyncing existing populated subtrees.

## [0.2.1] - 2026-10-05

### Summary

- Patch release improving Windows library compatibility, cloned-scene transforms, and CPU/CUDA attribute updates. No public C API/ABI or Python API changes from 0.2.0; existing applications do not need API migration.

### Fixed

- **Load ovstage alongside other MaterialX versions on Windows.** Versioned MaterialX DLL names prevent name conflicts that could stop ovstage from loading in applications using another MaterialX version.
- **Keep cloned objects in the correct position.** Cloned prims inside USD scene-graph instances now follow the cloned hierarchy's transforms instead of retaining the source's world transforms, so moving a cloned hierarchy also moves its affected child prims. If the destination parent does not exist, the clone root attaches to the nearest existing ancestor; if none exists, it remains unparented.
- **Update selected prims without corrupting other values.** CPU and CUDA attribute writes to a subset of prims now preserve attribute values on prims outside that selection, allowing applications to update selected objects without disturbing the rest of the scene.
- **Use GPU hierarchy computation when ovstage initializes first.** Fixed an initialization-order issue that could prevent ovrtx from using GPU transform-hierarchy computation when ovstage initialized first.

### Changed

- Updated the bundled Omniverse Client Library from 2.74.0 to 2.75.1, bringing security updates to the networking dependencies used for remote asset access.

## [0.2.0] - 2026-09-08

### Summary

- Feature release. Population can now be *described* by the caller rather than
  selected from a fixed set of domains, a selected slice of ovstage state can be
  exported back to USD, and physics-domain population is substantially more
  complete. How USD `asset`, `pathExpression`, `bool`, metadata and relationship
  values are represented changes — read the **Breaking** entries before upgrading.

- The C ABI gains sixteen exported entry points and loses none.
  `OVSTAGE_SEMANTIC_ASSET_STRING` is renamed `OVSTAGE_SEMANTIC_ASSET_PATH_ID`
  (value 1 unchanged), `OVSTAGE_SEMANTIC_TIME_CODE` (14) and
  `OVSTAGE_SEMANTIC_FRAME` (15) are appended, and the `static inline`
  `ovstage_population_stage_info_path()` is removed.

### Added

- The `ovstage-dynamic` shared loader, now behind the public `ovstage::ovstage`
  CMake target. Like `ovstage::ovstage_static`, it opens the runtime on the
  first initialize/create call, so schema discovery paths can be registered
  before OpenUSD loads. ***Breaking (Windows):*** the import library in `lib/`
  is renamed `ovstage.lib` to `ovstage-dynamic.lib` — no change if you link
  through the target, but a build naming it directly must be updated.

- **ovstage-to-USD export.** `ovstage_population_export.h` (C) and
  `ovstage.population` (Python) author a selected slice of ovstage state back
  into USD: either the committed state, or only the properties dirtied since a
  given ordinal, which makes repeated incremental export practical. What is
  exported, and how it lands in USD, is configurable per prim, applied schema,
  attribute, property edge and metadata field. Export targets a one-shot file
  or a reusable destination, blocking or asynchronous. See
  `docs/scene/exporting_to_usd.rst`, the `usd-export` examples, and the
  `exporting-to-usd` agent skill.

- **Caller-described population.**
  `ovstage_population_open_usd_from_file_with_desc` and
  `ovstage_population_open_usd_from_string_with_desc` (Python
  `open_usd_with_desc` / `open_usd_from_string_with_desc`) take what to populate
  as a caller-supplied description rather than a bare `domains` bitmask. A
  description still carries a `domains` field, but adds selectors composed from
  prim and property predicates, so scope is no longer limited to the fixed set
  of domains. Describing only `domains` reproduces the shorter entry point
  exactly. See the population guide and `ovstage_population_predicate.h`.

- `ovstage_population_register_usd_schemas()` /
  `ovstage.population.register_usd_schemas()` registers your own USD schema
  families, so population and export see each prim's full schema-declared
  property set rather than only its authored properties. Must be called before
  the first ovstage call that reads USD schema definitions; a later call fails
  with `OVSTAGE_ERROR_OP_FAILED` and contributes nothing, but still registers
  its plugins irreversibly.

- `ovstage_attribute_semantic_t` gained `OVSTAGE_SEMANTIC_TIME_CODE` and
  `OVSTAGE_SEMANTIC_FRAME` (`AttributeSemantic.TIME_CODE` /
  `AttributeSemantic.FRAME` in Python), so time codes and `frame4d` are
  writable and round-trip as
  themselves rather than as a roleless `double` and a `MATRIX`
  indistinguishable from a real `matrix4d`. Sentinel time codes are not
  representable.

- `read_attributes` serves an attribute name the queried prims use for more than
  one column type, one group per prim, where the whole name previously returned
  nothing. Such reads produce host memory, so a GPU-destination read skips the
  name and serves the rest as usual.

- `find_package(ovstage)` warns at configure time when your Linux toolchain
  resolves to a Homebrew/Linuxbrew binutils, which is unsupported and otherwise
  fails your build later with cryptic unresolved X11/GL or `GLIBC_2.xx` errors;
  silence it with `-DOVSTAGE_SKIP_LINKER_CHECK=ON`. The README documents the
  Linux system requirements.

- `OVSTAGE_PROFILER_BACKEND=nvtx` routes standalone profiling to NVTX, using a
  statically linked profiler rather than a host-supplied one.

- The rendered docs show the ovstage version on every page, and the docs build
  fails if `python/pyproject.toml` or the `ovstage.__version__` fallback
  disagrees with `VERSION.md`.

### Changed

- **Breaking:** a scalar `asset` is one `{kDLUInt, 64, 2}` element per prim
  holding `(authored token, resolved token)` under the renamed
  `OVSTAGE_SEMANTIC_ASSET_PATH_ID` (`AttributeSemantic.ASSET_PATH_ID` in
  Python), and `asset[]` the same with `is_array = true` and one pair per
  element; token id 0 means no path, so an asset authored as `@@` is the zero
  pair rather than being skipped. Every domain now lands this one
  representation, where `OVSTAGE_POPULATION_DOMAIN_PHYSICS` populated a single
  authored-path token, so a prim populated under `DOMAIN_ALL` used to fail on
  the type conflict. *Migration:* decode the two ids with
  `path_dictionary_get_strings_from_tokens` instead of `authored\0resolved` byte
  rows, which are now rejected; a PHYSICS-only `asset[]` consumer gets twice the
  payload, now carrying resolved paths. ovstage does not resolve assets — intern
  both paths before writing — and the semantic selects the asset column type, so
  a token-pair write leaving it `NONE` creates an ordinary `uint64x2` column.

- **Breaking:** `OVSTAGE_SEMANTIC_PATH_EXPRESSION_STRING` carries an interned
  token id rather than text, requiring dtype `{kDLUInt, 64, 1}` with `is_array`
  picking the USD form, where population used ragged UTF-8 byte rows and
  `pathExpression[]` NUL-joined every expression into one row. Token id 0 means
  no expression, so one authored as empty text keeps its place in an array.
  *Migration:* resolve the id with `path_dictionary_get_strings_from_tokens`
  (Python `PathDictionary.token_to_string`) and take an array's length from the
  id count.

- **Breaking:** population writes USD `bool` attributes as `{kDLBool, 8, 1}`
  instead of `{kDLUInt, 8, 1}`. The payload is unchanged — one byte per prim —
  but consumers asserting the former `uint8` dtype must be updated.

- **Breaking:** an attribute authored as the empty token or the empty string
  is populated as token id 0, where it was previously skipped with a serialization
  warning — so a consumer that read absence as "authored empty" now sees the
  column present. In an array an empty element is id 0 and keeps its place;
  previously one empty element dropped the *entire* array.

- **Breaking:** relationship and connection columns are explicitly array-valued
  — a relationship's targets populate as interned path ids, `{kDLUInt, 64, 1}`
  with `OVSTAGE_SEMANTIC_RELATIONSHIP_PATH_ID` and `is_array = true` — so a
  write or map must declare `is_array` truthfully. Both were previously given a
  scalar column shape, which let a connection write skip the per-path
  buffer-count check and let a connection column be mapped; mapping an array
  column is unsupported, so that is now rejected.

- **Breaking:** populated USD metadata moved under a reserved `usd-metadata:`
  attribute-name prefix — `usd-metadata:<field>` for prim metadata,
  `<property>:usd-metadata:<field>` for property metadata, extended by the
  `:`-joined key path inside a dictionary field — so a reader keyed on the old
  bare names finds no column: `inactiveIds` is now `usd-metadata:inactiveIds`.
  Under the prefix, metadata can no longer collide with an attribute or
  relationship of the same name; a dictionary key USD cannot address by key path
  is not populated, and a path with an empty component is rejected.

  Stage metadata (`metersPerUnit`, `kilogramsPerUnit`, `upAxis`, and anything a
  desc names) moved to the root prim `/` from a reserved
  `/__ovstage_population_stage_info__` prim, and is populated only when a domain
  or a desc's `stage_metadata_paths` asks for it, where it was previously
  authored regardless — so `domains = 0` (`OVSTAGE_POPULATION_DOMAIN_NONE`) now
  populates nothing at all. A path naming a dictionary populates every value
  beneath it, one column each, where it previously produced nothing, and
  populated stage metadata now tracks its USD source across
  `ovstage_population_apply_usd_changes`.

- **Breaking (Python):** integer arguments the bindings put into a fixed-width C
  field are normalized with `operator.index` rather than `int()` and
  range-checked, so a negative or over-range value raises `ValueError` and a
  wrong type raises `TypeError` naming the argument. ctypes wrapped out of range
  rather than raising, so an unguarded argument coerced silently and the native
  side validated the *wrapped* value: `ordinal=5.25` wrote at ordinal 5,
  `count = 2**32` became `0` and widened a write to the whole query, an
  `index_map` entry of `2**32` aliased source row `0`, a fractional
  `map_attribute` `element_sizes` entry sized that prim's ragged-array storage,
  `severity=2` installed an INFO threshold, `domains=-1` selected every domain,
  and a fractional `prim_index()` / `data_row_index()` / `tensor()` broke the
  documented row-placement idiom. Four cases need action:

  - An integral float such as `ordinal=5.0` is rejected alongside `5.25`, as are
    `'7'` and `b'5'`, which `int()` had parsed: a float carries 53 bits of
    mantissa against these 64-bit fields. `bool`, `IntEnum`/`IntFlag`, numpy
    integers and 0-d integer arrays are unaffected.
  - An `attribute` that is neither `int` nor `str` is rejected instead of
    falling through to the *string* branch, where `attribute=5.75` interned a
    new column. A numpy integer is not an `int` subclass, so
    `attribute=np.int64(12345)` previously meant the attribute *named* `12345`
    and now means token `12345`.
  - `domains` is guarded as the `uint32_t` the header declares rather than as a
    signed enum, so high bits are accepted and negatives rejected. On Python
    3.10 `~PopulationDomain.PHYSICS` is `-3` and now raises; write
    `PopulationDomain.ALL & ~PopulationDomain.PHYSICS`.
  - `severity`, `prim_mode`, `scope` and a filter `op` are validated against
    their enum members. `semantic`, `model` and `relation` deliberately keep the
    weaker range guard, so a wheel older than the loaded library does not reject
    a value that library accepts.

- **Breaking (Python):** DLPack lane counts are validated in `[1, 255]` where a
  `DLDataType` enters the bindings — `numpy_to_dldatatype`, `make_dltensor`,
  `DLTensor.from_dlpack` and `dltensor_to_numpy` — and no longer require a
  reachable backing array, so a malformed `DLDataType` decoded from a read or
  map result, or ingested from an external producer, can no longer describe a
  view past its payload and crash when accessed. But `numpy_to_dldatatype`
  accepted `[1, 65535]` in 0.1.1, so a lane count above 255 now raises.

- Population reports the same value types and semantics in every domain, so an
  attribute's representation no longer depends on which domains were requested.
  Eleven properties move, each gated on the schema named:

  - `extent` on a `UsdGeomImageable` prim provides one `{kDLFloat, 64, 6}`
    value per prim packing USD's two `(min, max)` points as six doubles, rather
    than a two-element `float3[]`. A non-imageable prim's `extent` keeps its USD
    type, and a value that is not the two-point form is skipped with a warning.
  - `normals` on a `UsdGeomCurves` or `UsdGeomPoints` prim becomes
    `OVSTAGE_SEMANTIC_POINT`; a Mesh keeps `OVSTAGE_SEMANTIC_NORMAL`.
  - `inputs:color` with `UsdLuxLightAPI` and `inputs:shadow:color` with
    `UsdLuxShadowAPI` become `OVSTAGE_SEMANTIC_VECTOR`, and so do seven roleless
    `float3` attributes: `omni:rtx:domeLight:size` with
    `OmniFiniteDomeLightAPI`; `nerf:crop:minBounds`, `nerf:crop:maxBounds` and
    `nerf:offset` with `OmniNerfVolumeAPI`; and `omni:nurec:crop:minBounds`,
    `omni:nurec:crop:maxBounds` and `omni:nurec:offset` with
    `OmniNuRecVolumeAPI`.

  Except for `extent`, only the reported semantic differs.

- `ovstage_population_apply_usd_time` re-samples whatever the initial
  `_open_usd_*` put in scope, not only the domains it selected, so a description
  carrying selectors and `domains == 0` is advanced like any other.

- Documented the complete USD-value-type mapping, the reserved and derived
  columns, and what an in-scope prim's ancestors carry, in
  `ovstage_population.h` and the population guide. Every population entry point
  carrying an `ordinal` is bound by the same write-floor admission rule as
  `ovstage_write_attributes`, so a new instance's first population needs an
  ordinal of at least 1.

- Added a transform guide covering local `omni:xform` authoring,
  effective-world authoring through `omni:resetXformStack`, hierarchy
  computation, and the provisional derived world-matrix output.

- Reads decode the `timecode` and `frame4d` column roles instead of reporting
  `NONE`, including on columns ovstage did not author, so
  `ovstage_read_group_t`'s `semantic` can report 14 / 15 where it reported 0.
  Writing a conflicting role-bearing semantic onto such a column — notably
  `OVSTAGE_SEMANTIC_MATRIX` onto a `frame4d` column — now fails with
  `OVSTAGE_ERROR_INVALID_ARGUMENT`, since a column's role is immutable.

- Cloned attribute connections are retargeted when they point inside the source
  subtree, but are **not** ordinal-change-tracked; cloned attribute values,
  including relationship targets, are. 0.1.1 documented connections as tracked,
  which they were not. Scene hierarchy changes remain untracked.

- `ovstage_clone` per-clone bookkeeping scales with the number of cloned prims
  rather than attributes × prims, removing the dominant cost of large fan-out
  clones of multi-attribute subtrees. Observable clone semantics are unchanged.

- The write, read and hierarchy paths are substantially faster on
  simulation-shaped workloads, with no change to observable semantics: repeated
  full-query writes reuse a resolved column binding, enqueue caches validation
  across batched attributes and overlapping queries, `read_attributes` builds
  indices from interned path handles, and `ovstage_compute_hierarchy` reuses its
  GPU transform engine across calls.

- The standalone runtime's console output is silent by default; diagnostics come
  only through the opt-in log callback (`ovstage_set_log_callback` /
  `ovstage.set_log_callback`), where runtime components previously printed
  warning and error lines with no way to turn them off. Embedded in a host that
  configures its own logging, console behavior follows the host. Operation
  failures are unaffected.

- Starting the standalone runtime no longer inherits the host's profiler
  configuration; standalone profiling is governed only by
  `OVSTAGE_PROFILER_BACKEND` / `OVSTAGE_PROFILER_CAPTURE_MASK` and
  `ovstage_initialize`.

### Removed

- **Breaking:** the package no longer bundles or registers physics USD schema
  definitions (`bin/ovstage_usd_schemas/`: `PhysxSchema`,
  `OmniUsdPhysicsDeformableSchema`, Newton); core `UsdPhysics` is still
  registered by the USD build, so prim types and applied API schemas from it are
  unaffected. Register your own copy with
  `ovstage_population_register_usd_schemas()` before the first call that reads
  USD schema definitions, or those prims populate with their authored attributes
  only — unauthored attributes no longer resolve their schema fallbacks.
  Deployments that copied `ovstage_usd_schemas/` beside their executable can
  drop that step.

- `ovstage_population_stage_info_path()`, along with the reserved prim it named;
  the path it returned is now simply `/`.

### Fixed

- Bounded reads now keep a homogeneous semantic-bearing attribute in compatible
  span groups instead of producing one group per prim (the 4,096-prim physics
  case now returns one group instead of 4,096). Same-name representations are
  partitioned only when their retained changes intersect the queried paths and
  ordinal range. Unrelated representations elsewhere no longer cause excessive
  read-group fan-out, while selected heterogeneous values and deletion tombstones
  retain their correct metadata.

- **Breaking (Python):** `PathDictionary.path_to_string` resolves the absolute
  root prim path to `/` instead of `""`, matching USD, so every valid
  prim-path handle round-trips back through `intern_path` and an empty string is
  never a successful result.

- `OVSTAGE_POPULATION_DOMAIN_PHYSICS` delivered an incomplete scene: data a
  physics consumer needs was reachable only by also populating `RENDERING`, or
  not at all. Now populated under `PHYSICS` alone:

  - Native scene-graph instancing tags, so the `ovstage_instancing_*` queries
    resolve without `RENDERING`.
  - A world-matrix column for `ovstage_compute_hierarchy` to fill; the compute
    updates an existing column but does not create one.
  - The Newton schema surface, and prims carrying only a Newton schema.
  - Complete collider geometry, the effective physics-purpose material binding
    per collider, rigid body and deformable body, and resolved collision-group
    membership.
  - Prims whose only physics schema stands alone, dropped previously even though
    their properties already counted as physics: `PhysxForceAPI`,
    `PhysxCharacterControllerAPI`, `PhysxParticleSamplingAPI`,
    `PhysxAutoDeformableAttachmentAPI`, the typed `OmniPhysics*Attachment` /
    `OmniPhysicsElementCollisionFilter` prims, and
    `OmniPhysicsDeformableBodyAPI` with its body/sim family.
  - A capsule's `height` and `radius` under `PhysxCharacterControllerAPI`, which
    the schema itself does not describe.
  - A deformable sim mesh's live geometry, including a `UsdGeomTetMesh`'s
    `tetVertexIndices`; the deformable schemas name only rest state.
  - The physics material bound on a prim carrying no physics schema of its own,
    where a consumer walking a collider's ancestors looks for it.
  - A collider mesh's `UsdGeomSubset` children, with face indices and per-face
    material binding.
  - `primvars:omni:scenePartition`, which names the simulation environment and
    so decides what may collide with what; it resolved only under `DOMAIN_ALL`.
  - A collider's `holeIndices` and `doubleSided`.
  - A particle set's live positions — `points`, or `positions` on a
    `PointInstancer` — which set the particle count, plus `velocities` and
    `accelerations`.
  - `physxParticle:maxParticles` and
    `physxParticle:fluidBoundaryDensityScale`, which no registered schema
    declares.

- A `PointInstancer` whose prototype reaches physics only through scene-graph
  instancing is populated; the scope test did not cross instance boundaries, so
  the instancer previously got no row at all — no `positions`, no
  `protoIndices`, no `prototypes`.

- Prototype content that leaves the populated scope loses its row on drain,
  where it survived if a sibling kept the prototype relevant.

- The instancing queries no longer report interior rows of a geometry instance
  as instance roots, keeping nested copies out of
  `ovstage_instancing_get_instance_roots`.

- Every fundamental USD value type populates in both scalar and array form,
  where coverage was uneven and a gap silently dropped the column: `uchar`,
  `half`, `timecode`, `double2`, `double4`, `half2/3/4`, `int2/3/4`, `quath`,
  `matrix2d` and `matrix3d` with their array forms, plus `uint[]`, `uint64[]`,
  `quatf[]`, `quatd[]` and `matrix4d[]`. This recovers
  `omniphysics:restTetVtxIndices`, `restTriVtxIndices`, `restAdjTriPairs`,
  `crvSegIndicesSrc1`, `physxCookedData:<instance>:buffer` and a
  `PointInstancer`'s `orientationsf`.

- Populated bool attributes are visible to consumers reading them as bools.
  `omni:resetXformStack` was the load-bearing case: the flag was dropped, so a
  prim resetting its transform stack was composed with its parent anyway.
  `ovstage_compute_hierarchy` on the GPU now honors it whether the column holds
  `bool` or an 8-bit integer.

- An authored relationship with no targets is populated as an empty column rather
  than being omitted, so it is distinguishable from one never authored.

- A populated `usd-metadata:` column is deleted when its USD source is removed,
  for prim and property metadata alike.

- An unauthored per-axis `physxJointAxis:<instance>:maxJointVelocity` uses
  the runtime default `FLT_MAX` rather than `1000000`, which silently capped an
  axis intended to be unlimited.

- `RENDERING`-domain population at USD default time composes correctly, where
  opening at default time and the first `ovstage_population_apply_usd_time`
  after it left time-sampled attributes at their initial sample.

- `ovstage_population_apply_usd_time` no longer fails permanently once a
  time-varying prim is removed or redefined at the same path.

- `ovstage_population_apply_usd_changes` no longer leaves stale rows or columns
  where a fresh populate would not: a column no longer selected for the prim is
  deleted, a prim that loses its physics schemas is dropped along with
  containers left empty, instancing tags are reconciled per prototype across
  every instance edit, `UsdGeomSubset` rows track their mesh, and resolved
  collection membership is re-resolved whenever a change can affect it.

- Replacing populated stage content copies only *authored* root metadata, so a
  schema fallback such as a default `timeCodesPerSecond` is no longer reported
  as authored.

- A read of an optional scalar attribute across a type-heterogeneous query keeps
  the prims that do carry the column, instead of giving up on the whole name.

- `ovstage_get_hierarchy` with `PARENT` returns no parent for a top-level
  populated prim rather than the pseudo-root.

- Three GPU and map resource defects: an out-of-range CUDA `device_id` returns
  `OVSTAGE_ERROR_INVALID_ARGUMENT` instead of targeting device 0, a failed or
  abandoned map releases its runtime mapping slot, and a transient CUDA driver
  query failure no longer caches "no GPU" for the life of the process.

- USD-support-layer diagnostics route through the log callback instead of
  printing raw to stderr, arriving at `OVSTAGE_LOG_INFO`,
  `OVSTAGE_LOG_WARNING` or `OVSTAGE_LOG_ERROR` by kind.

- Loading ovstage into a process that already registered a profiler no longer
  attempts a duplicate registration.

- Path-dictionary decomposition returns an actionable `OVX_API_ERROR` for
  invalid, unknown or expired prim-path handles, and Python
  `PathDictionary.path_to_string` raises `OvxError` rather than returning an
  empty string.

- Python: `ReadGroup` is a context manager with a finalizer safety net, so a
  group dropped without `release_group` no longer strands its pinned storage.
  The pin also held the group's outstanding-read coverage, so one leaked group
  kept every later write to the same attribute and prims failing with
  `overlapping outstanding read` for the life of the `Stage`. `release_group` is
  guarded against double release, a dropped group releases with a
  `ResourceWarning`, and using a released group raises. `MapGroup` is
  unaffected; it commits through `unmap_group`.

  **Upgrading:** release is now implicit, which 0.1 never did, so the rule that
  a group's views must not outlive it reaches code 0.1 let slide.
  `values = read.fetch_next().array(0)` leaked the group under 0.1 and left the
  view readable; it now reads reclaimed storage. `array()` and `dlpack()` borrow
  the group's storage without keeping it alive, so bind the group while you read
  and copy out anything that must outlive it.

- Python: `ReadGroup.array()` returns a zero-copy read-only NumPy view whose
  `WRITEABLE` flag cannot be re-enabled, so it no longer aliases committed read
  storage writably; `MapGroup.array()` remains writable. Because legacy DLPack
  cannot carry read-only state, NumPy can export this view only over DLPack 1.0
  or later; use `ReadGroup.dlpack(i)` for cross-framework exchange.

- Python: `PathDictionary.create_path_list` / `create_path_list_from_strings`
  return a refcounted `ovstage.PathList` — an `int` subclass that *is* the
  handle, adding a context manager and `release()` — instead of a bare `int`
  that leaked a reference per call. Lists borrowed from read results stay plain
  `int`, and explicit `destroy_path_list` still works. **Upgrading:** a list
  passed to `query_from_path_list` stays caller-owned, so bind it to a name and
  release it yourself rather than passing a freshly created one inline.

- Python: resource finalizers release *before* emitting their `ResourceWarning`,
  since `warnings.warn` raises under `-W error::ResourceWarning` and so skipped
  the release for exactly the users being strict about resources. Covers
  `Query`, `Read`, `OrdinalQuery`, `Map`, `ReadGroup` and `PathList`.

- Python: `import ovstage.population` and `import ovstage.instancing` work; both
  were plain attributes of `ovstage`, so every dotted spelling raised
  `ModuleNotFoundError`. `gates` left `ovstage.__all__` — a test-hook extension
  only test-instrumented builds export — but is still reachable as
  `ovstage.gates`.

### Limitations

- The 0.1.x limitations still apply, except where superseded above.
- **New in 0.2.0**

  - Asynchronous export is a convenience scheduler, not a USD synchronization
    boundary: do not concurrently read or author the same destination storage, and
    fence asynchronous work before mixing it with synchronous export. Every
    accepted asynchronous export must eventually receive a terminal wait.
  - Export cannot author into a USD layer or stage the calling application already
    has open; state crosses back only through saved storage. A reusable
    destination is bound to its source ovstage instance, which must outlive it,
    and only one active destination may own a canonical identifier per process. In
    Python, dropping a live `ExportDestination` queues a best-effort no-save
    release and emits a `ResourceWarning` — use `with` or call `close()`.
  - `ovstage_population_register_usd_schemas()` validates the paths it is given,
    not their contents: a malformed descriptor, or an `Includes` pattern matching
    nothing, contributes no plugins and still returns `OVSTAGE_OK`. A family
    registered after ovstage itself has read schema definitions contributes
    nothing, reports `OVSTAGE_ERROR_OP_FAILED` and cannot be taken back; only when
    another USD consumer in the process read schemas first is the late
    registration undetectable, and reported as success.

## [0.1.1] - 2026-08-04

### Summary

- Patch release. The C ABI is unchanged apart from one enumerator value: no
  exported symbol was added, removed, or renamed, and no vtable slot or struct
  layout moved, but the `ovstage_config_uint64_t` count sentinel advances from
  `0` to `1` now that the enum has a key. The headers add
  `OVSTAGE_ERROR_OUT_OF_RANGE`, the runtime-default hierarchy computation model
  selector, its configuration key, and two `static inline` configuration-entry
  helpers; Python adds `StageConfig`, `ErrorCode.OUT_OF_RANGE`, and
  `HierarchyComputationModel.RUNTIME_DEFAULT`. Some read and write validations
  became stricter, others became less restrictive, and several now report a
  different code and message, so callers that branch on status codes should read
  the notes below.

### Added

- Process configuration can select the runtime-default hierarchy computation
  model, which automatic transform updates use and which callers can request
  explicitly with `OVSTAGE_HIERARCHY_COMPUTATION_MODEL_RUNTIME_DEFAULT`. C
  callers build the entry with either of two new helpers,
  `ovstage_config_entry_runtime_default_hierarchy_computation_model()` or the
  generic `ovstage_config_entry_uint64()`, and pass it to `ovstage_initialize`;
  Python callers set it through `StageConfig` when creating a `Stage`. The
  setting is process-scoped and defaults to
  `OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL`.
- Python: `make_dltensor` accepts a `dtype` layout override for a non-NumPy
  DLPack producer, folding complete trailing dimensions into `dtype.lanes`. A
  Warp `vec3f` buffer exported as `(N, 3)` with one lane can be re-described as
  `(N,)` with 3 lanes without copying the producer buffer. A fold that consumes
  every producer axis normalizes to a one-element shape, so a component-only
  producer shaped `(3,)` with one lane becomes `(1,)` with 3 lanes. 0.1.0
  rejected every override on this path with `ValueError`.

### Changed

- Reads now enforce the sealing rule the API has always specified: an ordinal
  must be sealed before data written at it can be read back. Ordinal-range reads
  were not gated at all in 0.1.0 and returned data from ordinals that were still
  open; they now validate the changes a range selects. Both read kinds also now
  validate while the write floor is still at its initial value of `0`, a case
  0.1.0 skipped, so a write at a positive ordinal is not readable until the
  floor advances to cover it. Either failure reports
  `OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`. Snapshot validation is at the same time
  narrowed from the whole attribute column to the paths a query selects, so an
  unsealed write to one prim no longer vetoes a read of untouched prims and
  reads that 0.1.0 rejected can now succeed. Pending overlaps continue to report
  `OVSTAGE_ERROR_OP_FAILED`, and a range that selects no change still returns
  zero groups.
- An ordinal-range read can now fail with the new `OVSTAGE_ERROR_OUT_OF_RANGE`
  status. The current implementation stores only the latest payload per key, so
  when a selected `(attribute, path)` changed again after the range's end, the
  only stored value is newer than the range and the interval cannot be
  materialized, whether or not that later change is sealed. Latest-snapshot
  reads never report this status: their `end_ordinal` is not a historical
  payload bound, so the current value is exactly what they ask for.
- `ovstage_initialize` now validates process configuration instead of ignoring
  it. A malformed, duplicate, or unknown entry, or a runtime setting that
  conflicts with one already active in the process, returns
  `OVSTAGE_ERROR_INVALID_ARGUMENT`.
- When ovstage starts its own runtime, it supplies a fixed internal argument
  list, so host process arguments are no longer parsed as runtime options.
  Configuration arrives through `OVSTAGE_*` environment variables and
  `ovstage_initialize`. A runtime the host started stays host-owned and keeps
  whatever configuration the host gave it.
- Clone retargets relationship targets, scalar and array path values, and USD
  attribute connections that point inside the source subtree; paths outside it
  are copied unchanged, so clones keep referencing shared materials and other
  shared resources. Cloned attribute values, including relationship targets, are
  ordinal-change-tracked. Scene hierarchy changes, such as parent child lists,
  still are not.
- Fixed-size reads and maps expose a canonical lane-based layout: `ndim == 1`, a
  leading dimension equal to the transported data-row count, and the complete
  per-row tuple width in `dtype.lanes`. Convenience write inputs such as
  `(N, 3)` or `(N, 4, 4)` are still accepted, but their trailing shape is folded
  into lanes and is no longer echoed back on read. A fixed-size write without
  `index_map` must now have `shape[0]` equal to the logical element count; a
  flat `(N * L,)` one-lane tensor is not inferred as `N` rows of width `L`.
- Python: DLPack consumer views do not keep their group alive: retain the group
  or its `ManagedDLTensor` producer while using the view. Do not access the
  producer or its views after releasing the read group or an accepted group or
  final unmap submission. Keeping the owning operation alive is insufficient.
  A custom producer whose `manager_ctx` solely owns the backing memory must
  likewise keep the `ManagedDLTensor` alive while the consumer's view is used.
- The headers and guides now specify the `mask` buffer contract (element `i` is
  bit `i % 64` of word `i / 64`; a non-NULL mask must address at least
  `ceil(count / 64)` `uint64_t` words, because exactly that many are read), the
  distinct roles of `count`, `index_map`, and `mask`, and that array writes do
  not fold trailing dimensions into `dtype.lanes` the way fixed-size writes do.
  See `ovstage_write_data_t` and the writing-attributes guide.
- Documented the read representation for scalar `asset` attributes populated
  through the RENDERING population domain: each prim's value is one fixed
  `{kDLUInt, 64, 2}` element carrying the
  `{authored-path token, resolved-path token}` pair (resolved token `0` when
  unresolved), with attribute semantic `NONE`. Decode tokens through the shared
  path dictionary (`path_dictionary_get_strings_from_tokens`, Python
  `PathDictionary.token_to_string`). This is transitional and is planned to
  change to `OVSTAGE_SEMANTIC_ASSET_STRING` byte rows in a future release.

### Fixed

- `write_attribute` derived a write's transported row count from `index_map`
  rather than from the payload, so a map entry past the payload's rows invented
  extra rows and the resulting row width was reported as though the caller had
  declared it: a 4-byte `float32` scalar was rejected as
  `OVSTAGE_ERROR_NOT_SUPPORTED` with "dtype code=2 bits=32 lanes=1 and 2
  byte(s)". Fixed-size writes now take their row count from the tensor's
  `shape[0]` and per-row array writes from `tensor_count`, and both range-check
  `index_map` against it. Packed array transport declares no row count, so there
  the map still defines the partition; a partition the payload cannot support is
  now `OVSTAGE_ERROR_INVALID_ARGUMENT` with a message naming the map.
- Most write payload rejections no longer return a bare error code with an empty
  message. Missing `count`, mutually exclusive `index_map`/`mask`, an oversized
  `count`, a malformed single source tensor, an out-of-range row selection, and a
  payload that does not divide evenly across its rows each now carry a
  diagnostic naming the offending input. Per-row (`tensor_count > 1`) tensor
  validation still reports a bare `OVSTAGE_ERROR_INVALID_ARGUMENT`.
- A row width that is not a whole number of `dtype` elements is now
  `OVSTAGE_ERROR_INVALID_ARGUMENT` rather than `OVSTAGE_ERROR_NOT_SUPPORTED`: it
  reflects a payload cut into the wrong number of rows, not a capability the
  build lacks. Genuinely unrepresentable dtypes and over-wide fixed rows remain
  `OVSTAGE_ERROR_NOT_SUPPORTED`.
- A write that declares `OVSTAGE_SEMANTIC_MATRIX` is no longer restricted to
  non-array values with the fixed `{kDLFloat, 64, 16}` layout. The matrix role
  is represented independently of the numeric layout, so `matrix3f`, `matrix4f`,
  and matrix-valued arrays write and read back with the layout the caller
  declared. 0.1.0 rejected them with `OVSTAGE_ERROR_OP_FAILED`.
- `read_attributes` on a USD-populated scalar `asset` attribute returned
  `OVSTAGE_ERROR_END_OF_ITERATION` with no groups when the read covered a single
  prim, even though the attribute was discoverable with an `OVSTAGE_FILTER_OP_HAS`
  query and the same read succeeded across more than one prim. Single-prim reads
  now return the representation multi-prim reads already produced.
- A write whose source tensor lives in CUDA device memory reached storage but
  did not mark the written elements as changed, so downstream consumers such as
  a renderer never observed the new values; a CUDA-sourced `omni:xform` update
  left the rendered scene unchanged. Such writes now flag the elements they
  touch, and wait for their device-side copy to complete before the operation
  reports done, where previously it could report completion with the copy still
  in flight.
- Population no longer drops a render settings `camera` relationship whose
  camera prim does not exist yet, so that relationship survives population and
  cloning. Other relationships still require their targets to exist.
  Relationship targets are now also taken as authored rather than forwarded
  through relationship chains.
- USD scene-graph instance proxies now carry the reset-transform-stack state of
  the prim they stand in for, so a proxy whose source prim resets its transform
  stack is populated with the correct transform.
- ovstage initializes reliably however the host process was launched. Some
  command lines, such as an inline multi-line `python -c` command or an argument
  containing a newline, could abort initialization.
- Python: `write_attribute`/`write_attributes` now validate `count`,
  `index_map`, and `mask` together and raise `ValueError` rather than reaching
  the runtime. A `count` larger than `len(index_map)`, or larger than the `mask`
  could index, made the runtime read past the caller's buffer; a `count` of `0`
  computed from an empty selection silently widened a write to the whole query
  instead of writing nothing; a negative `count` wrapped through the `uint32`
  field to roughly `2^32`; and a `mask` without an explicit non-zero `count`
  produced a payload the runtime rejected with an empty message. To write more
  of the query, lengthen `index_map` rather than raising `count`, or omit
  `count` to write the whole query.
- Python: `wait`, `fetch`, and `flush_log` timeouts are validated instead of
  being wrapped through the `uint64` field, where `-1` happened to become
  `TIMEOUT_INFINITE`. A non-integer now raises `TypeError`, and a negative or
  oversized value raises `ValueError`, before the pending operation and its
  input keepalives are consumed.
- Python: a rejected enqueue captures its diagnostic when the operation is
  created rather than when it is waited on, so
  `op1 = write(...); op2 = write(...); op1.wait()` no longer reports `op2`'s
  message or none at all.
- Python: `numpy_to_dldatatype` now accepts every dtype spelling numpy itself
  accepts. A numpy scalar type (`np.float32`), a character code (`"f4"`), and a
  builtin (`float`) were all rejected with `ValueError: Unsupported numpy dtype
  for DLPack`; only an exact dtype-name string or an `np.dtype` instance
  resolved. Inputs are now normalized through `np.dtype` first. Dtypes DLPack
  has no mapping for still raise `ValueError`, as does `None`.
- Python: invalid DLPack lane counts are rejected instead of producing a
  malformed layout. `numpy_to_dldatatype` raises for `lanes` outside
  `[1, 65535]` rather than wrapping negative values through the `uint16` field,
  and `dltensor_to_numpy` rejects a zero lane count and refuses to build a view
  larger than the numpy array a `make_dltensor` tensor wraps. A non-numpy
  `make_dltensor` layout override likewise requires positive source and
  requested bit widths. Tensors from other producers still get no size check;
  see the limitation below.
- Python: writing into a read-only DLPack view no longer raises
  `SystemError: error return without exception set` and no longer leaks the
  tensor's retained resources. This affected any exception propagating while an
  exported tensor was released, not only read-only writes.
- Python: `Stage.destroy()` reports a failed native destruction by raising
  `OvstageError` instead of ignoring the status. When instance destruction
  fails it keeps its native handle and process reference, so the caller can
  retry without losing ownership state.
- Python: on Windows, a bundled plugin could fail to load because its own
  runtime dependency sits in a sibling `plugins/<name>.lib` directory that was
  not on the DLL search path. Every immediate `plugins/*` subdirectory is now
  registered with the Windows loader when the library is loaded.
- The package ships its `LICENSE`, with corrected wheel metadata.

### Limitations

- The 0.1.0 limitations still apply, except where superseded above.

- **New in 0.1.1**

  - An ordinal-range read that selects a key with a retained change after the
    range's end returns `OVSTAGE_ERROR_OUT_OF_RANGE`: the payload for that
    interval cannot be materialized, so the read yields neither change membership
    nor data.

- **Not new — pre-existing in 0.1.0, documented here for the first time**

  - A latest-snapshot read returns the current committed value rather than the
    value as of the requested `end_ordinal`, so it can return a payload written at
    a higher ordinal, and nothing in the result marks that this happened.
  - Python: `dltensor_to_numpy` sizes its view from the tensor's own `shape` and
    `dtype.lanes`, and can check that against the real buffer only for a tensor
    `make_dltensor` wrapped around a numpy array. For any other producer (a
    non-numpy DLPack export, or a hand-built `DLTensor`), a descriptor that
    overstates its payload yields a view extending past the buffer, and reading
    it is out of bounds. Attribute data ovstage itself stores cannot carry an
    out-of-range lane count, since the write path caps lanes at 255.
  - Writing to a USD-populated scalar `asset` attribute is not supported:
    `OVSTAGE_SEMANTIC_ASSET_STRING` byte-row payloads are rejected with a type
    mismatch, and raw token-pair writes are not validated end-to-end.
  - `asset[]` attributes are not populated by generic authored-attribute
    population and are not readable.

## [0.1.0] - 2026-07-16

### Summary

- Initial pre-release of ovstage (builds published as `0.1.0.<build>`). API,
  behavior, and packaging may change before GA.

### Added

- Asynchronous, ordinal-keyed C data-plane API (`include/ovstage/`): writes,
  reads, queries, zero-copy map/unmap, cloning, deletion, hierarchy queries,
  and instancing queries, with DLPack `DLTensor` tensor interchange. This
  build retains the latest committed snapshot only.
- USD population C API (`ovstage_population.h`): load composed USD scenes into
  the runtime stage, add/remove references, and propagate USD edits and time
  samples at application-owned ordinals.
- Shared path dictionary (`include/ovx/path_dictionary/`) for interned prim
  paths and tokens exchanged across OV libraries.
- Python bindings package (`python/ovstage/`, ctypes over the C data plane).
- Runnable C and Python example pairs (`examples/`), task-oriented agent
  skills (`skills/`), Sphinx documentation sources (`docs/`), and the
  public-contract test suites (`tests/`).

### Limitations

- Payload reads return the latest committed payload or tombstone, not historical
  payload versions. Ordinal ranges retain bounded change membership only; query
  `ovstage_get_oldest_preserved_ordinal` before consuming older ranges.
- Map/unmap is staging-backed and write-only: mapped buffers are not initialized
  from current storage, and unmap copies or scatters staged data into storage.
- Submission may perform synchronous preparation or wait for prerequisite
  handles, and accepted work executes through one serialized lane per instance.
- Overlapping reads, writes, deletes, and maps may be rejected while conflicting
  operations or borrowed groups remain live; release or finish them and retry.
- Query predicates support only the documented operator/attribute matrix;
  other operators exposed by the headers or bindings return `NOT_SUPPORTED`.
- `write_attributes` groups completion under one operation but is not atomic;
  individual entries may apply incrementally.
- Clone copies relationships verbatim without retargeting references inside the
  cloned subtree. Only value attributes are ordinal-change-tracked for clones.
- Derived world-transform rows may be absent or stale until their owning
  workflow materializes them and runs hierarchy computation. Directly authored
  `omni:xform` local-transform columns remain readable.
- Python writes do not expose `managed_tensors` ownership transfer; callers must
  keep client-managed source tensors alive until the operation completes.

[0.3.0]: https://github.com/NVIDIA-Omniverse/ovstage/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/NVIDIA-Omniverse/ovstage/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/NVIDIA-Omniverse/ovstage/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/NVIDIA-Omniverse/ovstage/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/NVIDIA-Omniverse/ovstage/releases/tag/v0.1.0
