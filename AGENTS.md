# AGENTS.md - AI Agent Guide for ovstage

This file gives AI coding agents the context needed to work effectively on
ovstage. It is the single entry point for agents — read it before changing code,
docs, or tests.

## What ovstage Is

`ovstage` is a vectorized, GPU-native runtime stage for USD data — a shared,
high-performance data substrate for OV Libraries (physics, rendering, animation,
graph). It reads, writes, and manages simulation data (transforms, velocities,
materials, metadata) across CPU and GPU memory, with zero-copy data paths.

The public surface is a pure **C API** (`include/ovstage/`) with a Python
bindings package (`python/ovstage/`) layered on top.

## Execution Model (read this first)

ovstage uses an asynchronous, **ordinal-keyed** submit/observe model:

- **Enqueue (synchronous).** Most stage data-plane mutations and data-producing
  operations return an `ovstage_enqueue_result_t` (status + `op_index`)
  immediately; the work is queued and runs later. Data-producing enqueues also
  reserve a typed handle that can be fed straight into the next enqueue.
  Lifecycle/configuration calls, diagnostics and accessors, instancing lookups,
  payload releases, and wait/fetch helpers are synchronous exceptions. Some of
  those synchronous calls can still block.
- **Ordinal-keyed write ordering.** Writes, deletes, and map commits carry an
  explicit `ordinal`. Same-ordinal ops run in submission order; different-ordinal
  ops are independent and may run concurrently.
- **Reads and queries are independent.** Reads target sealed data at or below the
  immutable write floor. Queries resolve against the latest committed state.
- **Fetch and wait.** Query/read/map fetches accept a timeout;
  `timeout == 0` polls and `OVSTAGE_TIMEOUT_INFINITE` blocks. Not every
  `ovstage_fetch_*` call has this shape: `ovstage_fetch_hierarchy_result()` is a
  nonblocking fetch without a timeout, and instancing lookups return their
  results synchronously.
- **Zero-copy by default**, with DLPack `DLTensor` for tensor interchange.

**This build is latest-snapshot only.** The C API surface includes ordinal and
retention concepts (e.g. `ovstage_get_oldest_preserved_ordinal`). Payload reads
still return only the latest committed state, but dirty metadata retains exact
change membership within a bounded window. Older records may be coalesced to one
latest marker per exact attribute and prim. The reported retention frontier is
inclusive; callers must query it rather than assume a fixed retention depth.
Range membership at or above it is exact, but returned payloads are still latest
and the range is not a per-write payload event log.

Built-in metadata attributes are auto-maintained and filterable: `usd-path`,
`usd-schemas`, `usd-prim-type`, `usd-parent`, `usd-children`. (`usd-active`
appears in the header contract but is not supported — reads/filters on it
return `NOT_SUPPORTED` — and is subject to removal in a future release.)

## Repo Layout

This repository is the canonical public-source surface for ovstage. The released
C SDK and Python wheel contain selected subsets of it: docs, examples, tests, and
skills are part of the public source but are not all copied into the binary SDK
archive.

- `include/ovstage/` - public C API headers: `ovstage.h` (data plane), `ovstage_types.h`,
  `ovstage_api/*`,
  `ovstage_instancing.h` (high-level instancing queries; included from `ovstage.h`),
  `ovstage_population.h` (the population C API, USD -> ovstage; included from `ovstage.h`),
  and `ovalign.h`
- `include/ovx/` - generated shared OVX headers shipped with the public mirror:
  `types.h`, `string_types.h`, `config_tokens.h`, `dlpack/dlpack.h`, and
  `path_dictionary/*`
- `include/dlpack/` - bundled DLPack header for zero-copy tensor interchange
- `python/ovstage/` - Python bindings package (ctypes over the C data plane)
- `examples/` - runnable example projects: C (standalone CMake) and Python (uv);
  start at `examples/README.md`
- `tests/` - public-contract test suites (C + Python) run against the produced
  package/wheel; also the source of truth for doc/skill snippets
- `docs/` - Sphinx documentation sources for the public API site
- `README.md`, `AGENTS.md`, `CLAUDE.md`, `CHANGELOG.md`, `LICENSE`, `SECURITY.md`,
  `THIRD-PARTY-NOTICES.txt` (generated third-party notice — do not edit)
- `skills/` - task-oriented agent skills (`*/SKILL.md`)

The internal implementation, unit tests, and benchmarks are
maintained separately and are not part of this distribution; the `tests/` here
are the public-contract suites that run against the produced package.

## Conventions

- Public headers are pure C: `extern "C"`, opaque handles, and no C++/STL/USD or
  host-framework types.
- Renaming a public symbol is a breaking change.
- Keep generic C API contracts implementation-neutral. Document current
  limitations in explicit `@remark` sections, and
  direct callers to runtime-reported capabilities such as the retention
  frontier instead of promising implementation policy constants.
- The public C API is the contract; alternate implementations may exist behind
  it. Do not expose private implementation or host-framework component names in
  public headers, docs, examples, bindings, or skills. Literal externally
  defined attribute tokens are allowed where required by the data contract.
  A concrete implementation identifier namespace needed to make an API usable
  may appear only in an explicit implementation-scoped `@remark`.
- `ovx_string_t` is an explicit pointer+length string view and is not required
  to be null-terminated. For string literals, prefer `literal_to_ovx_string(...)`
  or another explicit-length construction. Avoid helpers that accept arbitrary
  `const char*` and call `strlen`; pass `ovx_string_t` through helper layers, or
  use existing bounded storage such as `std::string::size()` in implementation
  code.
- `<ovx/path_dictionary/path_dictionary.h>` and its sibling path-dictionary headers
  are high-risk: the path dictionary is shared with downstream consumers (ovrtx,
  physics, Isaac Sim). Flag any change to them explicitly. The canonical source
  is `../../ovrtx/public/include/ovx/`; `include/ovx/` in this tree is generated.
- The shipped headers are the source of truth. If a doc and a header disagree,
  the header wins and the doc must be corrected.

## Status

Pre-release — API, behavior, and packaging may change.

