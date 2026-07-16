# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/) and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0]

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
