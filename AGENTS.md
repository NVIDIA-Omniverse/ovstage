# AGENTS.md - AI Agent Guide for ovstage

This file gives AI coding agents the context needed to work effectively on
ovstage. It is the single entry point for agents — read it before changing code,
docs, or tests.

## What ovstage Is

ovstage is a standalone C and Python library that gives simulation and visualization applications one shared, in-process store for scene and simulation data. Applications can author data directly or populate it from OpenUSD, then let physics, rendering, sensors, and other consumers query, read, and update that data through a common API. The application controls execution and when updates become visible, while ovstage provides bulk data access, shared scene identities, and change tracking so each consumer can read the state it needs.

Reads and maps use CPU buffers by default. The experimental
`ovstage.placement` extension also supports CUDA buffers; a read of a copy
already current on the requested device avoids migration. See
[device placement](docs/concepts/dlpack_tensors.rst#experimental-device-placement).

The public surface is a pure **C API** (`include/ovstage/`) with a Python
bindings package (`python/ovstage/`) layered on top.

## Execution Model (read this first)

ovstage uses an asynchronous, **ordinal-keyed** submit/observe model:

- **Enqueue (synchronous).** Most stage data-plane mutations and data-producing operations return an `ovstage_enqueue_result_t` (status + `op_index`) immediately; the work is queued and runs later. Data-producing enqueues also reserve a typed handle that can be fed straight into the next enqueue. Lifecycle/configuration calls, diagnostics and accessors, instancing lookups, payload releases, and wait/fetch helpers are synchronous exceptions. Some of those synchronous calls can still block, and write, delete, and map submissions wait until their query is ready before returning. See `docs/concepts/threading_lifetime.rst`.
- **Ordinal-keyed write ordering.** Writes, deletes, and map commits carry an explicit `ordinal`. Same-ordinal ops run in submission order; that is the only ordering guarantee. Do not depend on cross-ordinal ordering.
- **Canonical attribute identity.** Aliases of one stored column share ordinal
  admission/history and overlap checks on intersecting prim paths.
- **Ordinal admission is nondecreasing by default.** Equal unsealed ordinals
  are admitted. Under the `STRICTLY_INCREASING` policy, write, map publication, clone, and delete must
  clear applicable floors and retained committed changes to the affected cells
  and prim lifecycle. Disjoint cells may share an ordinal; a second modification
  of the same cell needs a newer ordinal, even before sealing. Process configuration
  can select `NONDECREASING` or `FLOOR_ONLY`; see
  [Ordinal Admission Policies](docs/scene/writing_attributes.rst#ordinal-admission-policies)
  for their history and lifecycle checks. Enqueue success does not guarantee
  admission at execution.
- **Reads and queries are independent.** Reads target sealed data at or below the
  immutable write floor. Queries resolve against the latest committed state.
- **Fetch and wait.** Query/read/map fetches accept a timeout;
  `timeout == 0` polls and `OVSTAGE_TIMEOUT_INFINITE` blocks. Not every
  `ovstage_fetch_*` call has this shape: `ovstage_fetch_hierarchy_result()` is a
  nonblocking fetch without a timeout, and instancing lookups return their
  results synchronously. A clone's completion report is fetched by op id
  (`ovstage_fetch_clone_result(op_id, timeout, ...)`); it is per-op state, not a
  handle, and lives until `release_op`.
- **DLPack `DLTensor`** for tensor interchange (CUDA write sources accepted; reads and maps use CPU buffers by default. The experimental placement extension also supports CUDA read payloads and fixed-size CUDA map buffers).

**An instance of `history_latest`, the default, stores only current payload values.** An
instance of `history_selective` retains the earlier sealed values of the pairs selected with
`ovstage_enable_history()` (`ovstage_history.h`), up to the retention window. History
support (the `history_selective` implementation and `ovstage_history.h`) is experimental
and can change between releases without a deprecation period. Every other pair
keeps the rules of this paragraph. The C API surface includes ordinal and
retention concepts (e.g. `ovstage_get_oldest_preserved_ordinal`). Payload reads
return only the current stored values, but dirty metadata retains exact
change membership within a bounded window. Older records may be coalesced to one
latest marker per exact attribute and prim. The reported retention frontier is
inclusive; callers must query it rather than assume a fixed retention depth.
Range membership at or above it is exact; a selected `(attribute, path)` with a
retained successor after a requested end below `OVSTAGE_ORDINAL_MAX` returns
`OVSTAGE_ERROR_OUT_OF_RANGE` rather than a later payload, and the range is not a
per-write payload event log. A snapshot selects every queried path; use
`OVSTAGE_ORDINAL_MAX` as the end to read current values.

Built-in metadata attributes are filterable. `usd-path`, `usd-parent`, and
`usd-children` derive from stage structure. `usd-schemas`, `usd-prim-type`, and
`usd-ancestor-types` are recorded columns authored directly or by USD population;
changing the prim type does not automatically update its ancestor types.
(`usd-active` appears in the header contract but is not supported — reads/filters on it
return `NOT_SUPPORTED` — and is subject to removal in a future release.)

## Task to skill routing

Read the matching `skills/<name>/SKILL.md` before starting the task.

| Task | Skill |
|------|-------|
| Understand the end-to-end lifecycle (create, write, seal, read, release) | `skills/application-flow` |
| Set up a C/C++ consumer (headers, linking, CMake) | `skills/project-setup-c` |
| Set up a Python project (install, import, scaffold) | `skills/project-setup-python` |
| Load USD into ovstage, add references, propagate live USD edits | `skills/loading-usd` |
| Export runtime state back to USD (typed hierarchy, overlays, destinations) | `skills/exporting-to-usd` |
| Drive a headless load, read, update, read loop | `skills/runtime-loop` |
| Find prims with filter queries and inspect query results | `skills/stage-queries` |
| Clone a subtree or stamp out per-environment copies | `skills/clone-subtree-multienv` |
| Understand async submit/observe, op ids, waits, timeouts, overlap | `skills/cpu-ahead-gpu-async` |
| Exchange tensor data with DLPack (residency, CUDA sync, sparsity, map/unmap) | `skills/dlpack-tensor-exchange` |
| Check return codes and diagnose failed enqueue, wait, or fetch | `skills/error-handling` |
| Pass strings and tokens (`ovx_string_t`, `ovx_token_or_string_t`) | `skills/string-handling` |
| Intern tokens and prim paths with the path dictionary | `skills/path-dictionary` |
| Route ovstage log output, set severity and channel filters | `skills/logging` |
| Port USD API (`pxr`) authoring code to the ovstage data plane | `skills/usd-to-ovstage-migration` |
| Migrate C or Python apps from ovstage 0.2 to 0.3 | `skills/update-0-2-to-0-3` |
| Migrate C or Python apps from ovstage 0.3 to 0.4 | `skills/update-0-3-to-0-4` |

## Where to find answers

Doc pages are under `docs/`; the same paths are listed in [`llms.txt`](llms.txt).

- What ovstage is and how the data model fits together: `docs/concepts/overview.rst`
- C or Python first steps: `docs/c_api/getting_started.rst`, `docs/python_api/getting_started.rst`
- C or Python, which to use: `docs/concepts/choosing_api.rst`
- Ordinals, watermarks (write floor in the API), async ops, waits: `docs/concepts/async_model.rst`
- Threading, blocking calls, and object lifetime: `docs/concepts/threading_lifetime.rst`
- Status codes, diagnostics, error recovery: `docs/concepts/error_handling.rst`
- Tensor residency, CUDA writes, sparse writes, map/unmap: `docs/concepts/dlpack_tensors.rst`
- Strings and tokens: `docs/concepts/string_handling.rst`
- Current limitations and behavior that may change: `docs/concepts/limitations.rst`
- Moving from ovstage 0.2 to 0.3: `docs/guides/upgrading_from_0_2.rst`
- Moving from ovstage 0.3 to 0.4: `docs/guides/upgrading_from_0_3.rst`
- Reading, writing, queries, transforms, cloning, instancing, path dictionary, USD population and export: `docs/scene/`
- Runnable examples: `examples/README.md`

## Repo Layout

This repository is the canonical public-source surface for ovstage. The released
C SDK and Python wheel contain selected subsets of it: docs, examples, tests, and
skills are part of the public source but are not all copied into the binary SDK
archive.

- `include/ovstage/` - public C API headers: `ovstage.h` (data plane), `ovstage_types.h`,
  `ovstage_api/*` (experimental extensions under `ovstage_api/experimental/`,
  included explicitly),
  `ovstage_instancing.h` (high-level instancing queries, the wrappers of the instancing family; included from `ovstage.h`),
  `ovstage_population.h` (the population C API, USD -> ovstage; included from `ovstage.h`),
  `ovstage_population_export.h` (selected ovstage -> USD authoring; included explicitly),
  `ovstage_population_types.h` (the population and export types; included from `ovstage_ext_vtables.h`),
  and `ovstage_ext_vtables.h` (every family vtable and the rules of the extension instances; included from `ovstage.h`)
- `include/ovx/` - generated shared OVX headers shipped with the public mirror:
  `types.h`, `string_types.h`, `config_tokens.h`, `dlpack/dlpack.h`, and
  `path_dictionary/*`
- `include/dlpack/` - bundled DLPack header for tensor interchange
- `python/ovstage/` - Python bindings package (ctypes over the C data plane)
- `VERSION.md` - canonical OVStage semantic version. Source/editable Python
  imports read it relative to `python/ovstage`; wheel builds generate
  `ovstage/_version.py`. Keep those paths, the native build-info generator, and
  package manifests synchronized.
- `examples/` - runnable example projects: C (standalone CMake) and Python (uv);
  start at `examples/README.md`
- `tests/` - public-contract test suites (C + Python) run against the produced
  package/wheel; also the source of truth for doc/skill snippets
- `docs/` - Sphinx documentation sources for the public API site
- `README.md`, `AGENTS.md`, `CLAUDE.md`, `CHANGELOG.md`, `LICENSE`, `SECURITY.md`,
  `THIRD-PARTY-NOTICES.txt` (generated third-party notice — do not edit)
- `skills/` - task-oriented agent skills (`*/SKILL.md`)

### USD workflow routing

- Use `skills/loading-usd` for USD -> ovstage ingestion and live source edits.
- Use `skills/exporting-to-usd` for ovstage -> USD selection, typed hierarchy
  export, destination ownership, persistence, identifier routes, and export waits.
- Use `skills/application-flow` for the general create/write/seal/read lifecycle,
  `skills/error-handling` for status/diagnostics, and
  `skills/usd-to-ovstage-migration` for direct-USD to data-plane migration.

The internal implementation, unit tests, and benchmark sources are
maintained separately and are not part of this distribution; the `tests/` here
are the public-contract suites that run against the produced package. The
x86_64 packages ship the prebuilt `benchmark.ovstage` executable
(`docs/guides/benchmarking.rst`).

## Conventions

- Public headers are pure C: `extern "C"`, opaque handles, and no C++/STL/USD or
  host-framework types.
- Renaming a public symbol is a breaking change.
- Experimental public APIs live under `include/ovstage/ovstage_api/experimental/`,
  are reached through `query_extension`, state their compatibility policy in the
  header, and accept only their exact extension version.
- `ovx_string_t` is an explicit pointer+length string view and is not required
  to be null-terminated. For string literals, prefer `literal_to_ovx_string(...)`
  or another explicit-length construction. Avoid helpers that accept arbitrary
  `const char*` and call `strlen`; pass `ovx_string_t` through helper layers, or
  use existing bounded storage such as `std::string::size()` in implementation
  code.
- The shipped headers are the source of truth. If a doc and a header disagree,
  the header wins and the doc must be corrected.

## Status

Pre-release — API, behavior, and packaging may change.

