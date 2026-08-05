# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/) and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1] - 2026-08-04

Patch release. The C ABI is unchanged apart from one enumerator value: no
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
- Python: a `ManagedDLTensor` releases what it retains when the capsule is
  destroyed, not when the consumer releases the tensor — for a consumed capsule,
  as soon as the consumer takes ownership, which is generally earlier.
  `np.from_dlpack(group.dlpack(0))` remains valid while the owning read/map
  operation is alive; only a custom producer whose `manager_ctx` solely owns the
  backing memory must keep the `ManagedDLTensor` alive as long as the consumer's
  view is used.
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

The 0.1.0 limitations still apply, except where superseded above.

#### New in 0.1.1

- An ordinal-range read that selects a key with a retained change after the
  range's end returns `OVSTAGE_ERROR_OUT_OF_RANGE`: the payload for that
  interval cannot be materialized, so the read yields neither change membership
  nor data.

#### Not new — pre-existing in 0.1.0, documented here for the first time

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

Initial pre-release of ovstage (builds published as `0.1.0.<build>`). API,
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
- USD-populated derived transforms are not available through `read_attributes`;
  directly authored transform columns remain readable.
- Python writes do not expose `managed_tensors` ownership transfer; callers must
  keep client-managed source tensors alive until the operation completes.
