---
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.
name: dlpack-tensor-exchange
description: >
  How ovstage attribute data crosses the boundary as DLPack DLTensors — copy-in writes,
  copy-out reads, staging-backed map/unmap writes, CPU vs CUDA residency, cuda_sync GPU sync, and
  sparsity (index_map/mask). Use when user asks about DLTensor, tensor data exchange, GPU
  residency, CUDA sync, zero-copy writes, or mapping attribute storage.
license: LicenseRef-NvidiaProprietary
version: "0.1.0"
author: NVIDIA ovstage
tags:
  - ovstage
  - dlpack
  - cuda
tools:
  - Read
  - Grep
---

# DLPack Tensor Exchange

## When to Use

Use this skill when the user asks how attribute data moves in or out of ovstage as tensors:
building `DLTensor`s for a write, reading tensor data back, choosing CPU vs CUDA residency,
synchronizing GPU producers/consumers with `cuda_sync`, writing through ovstage-allocated
map buffers, passing sparse/gathered data (`index_map` / `mask`), or how
long returned tensor data stays valid.

## Inputs

Resolve inputs in this order: existing repository files and referenced snippets, explicit user request, then broader agent context.

- Direction: **copy-in** write (`write_attribute` + `ovstage_write_data_t`), **copy-out**
  read (`read_attributes` → `fetch_read_next` → `ovstage_read_group_t.data`), or **mapped
  staging** write (`map_attribute` → `fetch_map_next` → fill → `unmap_*`).
- Residency: CPU (`DLDevice{ kDLCPU, 0 }`) or CUDA (`DLDevice{ kDLCUDA, device_ordinal }`),
  and whether a producing/consuming GPU kernel needs a `cuda_sync`.
- Attribute kind: fixed-size (one tensor, all transported data rows stacked along the leading
  dimension) vs. array/ragged (one tensor per transported data row). Logical prims select
  those rows directly or through `index_map`.
- Sparsity: dense, or `index_map` (gather/reorder/dedup) or `mask` (per-element validity) —
  the two are mutually exclusive.
- The shipped headers (`ovstage_api/ovstage_api_types.h` for the data structs,
  `ovstage_api/experimental/ovstage_placement_extension.h` for the experimental
  `ovstage_placement_t`, the bundled `<dlpack/...>` for `DLTensor`/`DLDataType`/`DLDevice`)
  are the authoritative contract.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced example/snippets.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the async enqueue/observe model first (see `cpu-ahead-gpu-async`): tensor ops
  are **enqueues** — wait or fetch to confirm production/consumption and check errors.
- For GPU-resident data, know your CUDA event/stream ownership: **GPU synchronization
  between producer and consumer is the caller's responsibility** (ovstage coordinates via
  `cuda_sync`, it does not manage your streams).

## Instructions

1. Pick the path: copy-in (`write_attribute`), copy-out (`read_attributes` /
   `fetch_read_next`), or mapped staging write (`map_attribute` / `fetch_map_next` / `unmap_*`).
2. Describe data with a `DLTensor`: `data` pointer, `device` (`{kDLCPU,0}` or
   `{kDLCUDA, ord}`), `ndim`, `dtype` (`{code, bits, lanes}` — `lanes` is the tuple width, e.g.
   3 for a float3 canonical write), `shape`, `strides`, `byte_offset`. Read and
   map tensors use scalar lanes and type-aware trailing component dimensions:
   scalars `[N]`, tuples `[N, C]`, and semantic square matrices `[N, S, S]`.
   Their strides count scalar elements; per-prim arrays use the same trailing axes.
3. **Write (copy-in):** put the tensor(s) in `ovstage_write_data_t.tensors` (client-managed —
   must stay valid until the op completes, or until the placement's `availability_event` has
   completed when one is supplied, or, if that event could not be recorded, until the
   synchronization `ovstage_placement_t` describes for that case) **or** `.managed_tensors`
   (storage takes ownership via the `DLManagedTensorVersioned` deleter, which may run after
   the op completes when an availability event is supplied); exactly one is non-NULL. Set
   `tensor_count` (1 for fixed-size).
4. **GPU-resident write:** set `write_data.cuda_sync.wait_event` to an event recorded *after*
   your producing kernel — ovstage waits on it before copying in — and `.stream` to the CUDA
   stream it was recorded on (`0` = none, `1` = the default stream, `>1` = a specific
   `cudaStream_t`). Leave `cuda_sync` `{0, 0}` for CPU or already-synchronized data.
5. **Read (copy-out):** read groups report `group.data.cuda_sync = {0, 0}`.
   Unplaced reads and placed reads without an availability event are ready on a
   successful fetch. With `placement.availability_event`, successfully fetch a
   group first, then wait on that event or order its consumer on the placement
   stream before accessing the payload. Stream `2` is the worker's per-thread
   default stream; use the event to order work on the caller's thread. Treat the
   data as valid only for the current snapshot — copy/retain it to use beyond the immediate
   read. Release the group with `release_group`. Releasing the *read* does not reclaim a
   group: an unreleased group stays pinned for the life of the stage and keeps failing later
   writes to the same prims with an "overlapping outstanding read" error. In Python, prefer
   the context manager so an exception in the processing body cannot strand it:

   > **Source:** `tests/python/test_read_groups.py` snippet `read-group-context-manager`
6. **Mapped staging write:** `fetch_map_next` hands back a writable `ovstage_map_group_t`
   after setup is Ready. Setup does not commit values, new prims or attributes, schemas,
   hierarchy connections, or change records, and it records no new ordinal ownership.
   Fill the CPU tensors, then commit with `unmap_group` (per group) or `unmap_attribute`
   (commit remaining + release the handle), and wait for
   completion. Each successful commit applies the group's values and any structural changes
   and records them once; final unmap never commits earlier groups again. Pass a zero
   write-done `ovstage_cuda_sync_t` for these CPU buffers. Advance the write floor separately before
   reading committed values back.
7. **Sparsity:** when `index_map` or `mask` is set, use `count` (logical element count) with
   `index_map[i]` (gather) or `mask` (validity); they are mutually exclusive. `index_map`
   selects *source rows*, `mask` selects *target elements* — to write a subset of a query's
   prims, use `mask`. Where the payload declares a row count (`shape[0]` fixed,
   `tensor_count` per-row array) every `index_map` entry must be below it and out-of-range is
   rejected, not reinterpreted; unreferenced rows are simply unused. Packed array transport
   declares none, so there the map defines the partition as `max(index_map) + 1` uniform
   rows. `mask` does not change the row partition, so a masked payload still carries a row
   per logical element, and it must span at least `ceil(count / 64)` `uint64_t` words —
   element `i` is bit `i % 64` of word `i / 64`, and exactly that many words are read.

## Output Format

- For explanations, cite the relevant API names, source snippets, and caveats.
- For code changes, summarize the files changed, snippets affected, and validation run.

## Scripts

This skill has no scripts.

## Limitations

- The referenced snippets remain the source of truth; update or add tested snippets before documenting new API usage.
- **Map/unmap is staging-backed in the current implementation, not a true storage map.**
  `fetch_map_next` returns a session-owned, write-only buffer; it is not a view of the
  attribute's current value and is not initialized from existing storage. `unmap_group` or
  `unmap_attribute` copies/scatters the staged bytes into backing storage. Stop accessing
  the buffer after an accepted commit submission, even if the session retains its
  allocation until final unmap. Treat map/unmap as a write-only replacement path and
  account for the staging allocation and commit copy.
- **Failed map execution is terminal.** Final unmap releases the failed session and reports
  its error; repair requires a new map or write. A failed write does not commit new prims or
  attributes or record new ordinal ownership or change records. Earlier successful groups
  remain committed. The exception is a placed write or map commit whose `availability_event`
  CUDA refuses to record: it reports `OVSTAGE_ERROR_OP_FAILED` after committing, so its
  changes and ordinal ownership stay recorded, and an `unmap_group` session stays open.
  Storage changes may remain after failure: neither individual commits nor final session
  closure promise rollback or atomicity.
- **Unplaced maps and reads return CPU payloads.** The experimental `Placement`
  extension also selects CUDA buffers (fixed-size attributes only for maps).
  Inspect `DLTensor.device`, use a matching
  consumer, and obey the availability-event and lifetime rules in
  [device placement](../../docs/concepts/dlpack_tensors.rst#experimental-device-placement).
  Never launch a CUDA kernel against a CPU map buffer.
- **CPU DLTensor write/read is snippet-backed** (the minimal example), and this build adds
  snippet-backed **CPU map/unmap** (`map-unmap-cpu`, the public map test) and **Python CUDA
  ingest with warp** (`gpu-warp-ingest`, write-flavors — GPU-gated at runtime).
  The C CUDA ingest example is in `examples/c/write-flavors/cuda_ingest.cpp`;
  `tests/c/test_placement.cpp` covers the experimental placement extension.
- **Latest-snapshot build:** returned tensor data is valid only for the latest committed
  snapshot. To use it later you must take explicit ownership (copy / retain / transfer) — do
  not hold a borrowed pointer across further commits.
- **GPU sync is the caller's responsibility** — ovstage coordinates via `cuda_sync` but does
  not own or synchronize your streams.
- **Python `managed_tensors` transfer is not exposed** — DLPack ingest/export (incl. CUDA) is
  supported (see the Python section), but the Python write path uses only the client-managed
  `tensors` field: the caller keeps the source object alive until the op completes (until the
  placement's availability event completes, when one is supplied, or, if that event could not
  be recorded, until the synchronization `Placement` describes), rather than handing ownership
  to storage via `managed_tensors`.
- **Fixed-size shape is reconstructed, not retained.** Compact convenience write shapes are
  folded into storage lanes. Reads and maps reconstruct component axes from width and semantic.
  Array/ragged values apply those axes per tensor.
- **⚠️ Draft — API in flux.** Treat exact symbols/usage as provisional against the headers.

## Overview

ovstage exchanges attribute values as **DLPack `DLTensor`s**. The map/unmap write path
provides write-only staging that is applied to attribute storage at successful unmap.
One data shape is reused throughout:

- **`ovstage_data_t`** (reads and map groups) and **`ovstage_write_data_t`** (writes) both
  carry: an array of `DLTensor`s (`tensors` + `tensor_count`), optional sparsity
  (`index_map` *or* `mask`, with `count`), and a GPU-sync `cuda_sync` (`{stream, wait_event}`).
- **`tensor_count` depends on attribute kind:** fixed-size attributes use a single tensor
  with all transported data rows stacked along the leading dimension; array (ragged)
  attributes use one tensor per transported data row.
- **Reads and maps share typed shapes:** the leading dimension is the
  transported data-row count. Both expose scalar lanes, with a component axis for tuples
  or two square axes for known MATRIX values. Logical elements select rows directly or
  through `data.index_map`. Input trailing dimensions are not retained as schema metadata.
- **Three paths:** *copy-in* (`write_attribute`, implementation copies/scatters your tensors
  into storage), *copy-out* (`read_attributes` → `fetch_read_next`, you read from the
  returned tensors), and *mapped staging* (`map_attribute` → `fetch_map_next`, you fill a
  write-only staging buffer that is copied/scattered into backing storage by `unmap_*`).
- **`cuda_sync` semantics** (`ovstage_cuda_sync_t { uintptr_t stream; uintptr_t wait_event; }`,
  where `wait_event` is a `cudaEvent_t` and `stream` a `cudaStream_t`, both as `uintptr_t`;
  `stream` 0 = none, 1 = default stream, >1 = a specific stream): `{0, 0}` means CPU-resident
  or already-synchronized; a non-zero `wait_event` on a **read** means *wait before access*; on
  a **write/unmap** it is the event ovstage *waits on* (relative to `stream`) before applying
  your data. Advancing the write floor seals committed values separately.

### Fixed-size canonical and convenience layouts

For fixed-size attributes (`is_array = false`), both canonical lane-based and convenience
layouts are accepted on copy-in. Trailing component dimensions are folded into storage lanes;
the read shape is reconstructed from component count and semantic. In this table, `N` is the source/transported data-row
count. When `data.index_map` is present, `N` may differ from the logical prim count: it can
be smaller when rows are shared or larger when a query touches only part of a transported
bucket. Without `index_map`, `N` must equal the logical prim count. A flat `(N * L,)`,
`lanes = 1` tensor is not a convenience encoding of `N` rows of width `L`; use `(N, L)` or
canonical lanes.

| Value per data row | Canonical write | Accepted convenience write | Raw read/map / Python DLPack export |
|---|---|---|---|
| scalar | `shape = [N]`, `lanes = 1` | same | `[N]`, lanes 1 / `(N,)` |
| point/vector/color (`float3`) | `shape = [N]`, `lanes = 3` | `shape = [N, 3]`, `lanes = 1` | `[N,3]`, lanes 1 / `(N, 3)` |
| matrix (`matrix4d`) | `shape = [N]`, `lanes = 16` | `shape = [N, 4, 4]`, `lanes = 1`, or `shape = [N, 4]`, `lanes = 4` | `[N,4,4]`, lanes 1 / `(N, 4, 4)` |

The original convenience shape is not stored. A known matrix reads and maps as
`(N, S, S)` for S=2,3,4; a generic tuple retains one component axis, including
12-component tuples. Array/ragged reads and maps apply the same rule per tensor.

To forward a known read/map descriptor as canonical write input, use C
`ovstage_tensor_to_write()` from `ovstage_tensor_utils.h` or Python
`typed_tensor_to_write()`. These copy metadata only and preserve compact component
axes and outer row strides. Preserve semantic, array kind, and prim/data mappings.
Copy a pinned read payload before releasing it when rewriting the same storage;
keep all write inputs alive until completion. Do not infer vector-array types
from arbitrary multidimensional scalar arrays.

## C — write and read (copy-in / copy-out)

The minimal example builds a CPU `DLTensor`, writes it through `ovstage_write_data_t`, seals
it by advancing the write floor, and reads the column back from `group.data.tensors[0].data`
— the end-to-end CPU shape for both directions:

> **Source:** `examples/c/minimal/main.cpp` snippet `minimal-write-read`

> **Source:** `tests/c/test_minimal.cpp` snippet `write-read-roundtrip-c`

The public test asserts the three column shapes round-trip — a 1-lane scalar, a fixed
multi-lane tuple (lanes in the dtype, not the shape), and a ragged per-prim array:

> **Source:** `tests/c/test_attribute_shapes.cpp` snippets `attribute-shapes-fixed-c`, `attribute-shapes-ragged-c`

A **semantic** is the authored USD meaning of a column's bytes, orthogonal to the storage
dtype (POINT/COLOR/MATRIX on float storage; TOKEN_ID pins uint64 token-id storage). The write
stamps it and the read recovers it — the public test asserts the round-trip:

> **Source:** `tests/c/test_attributes.cpp` snippet `semantic-roles-c`

The public contract test also authors a matrix with the convenience input layout
`shape = [3, 4, 4]`, `lanes = 1`, then observes the type-aware raw read layout
`shape = [3, 4, 4]`, `lanes = 1`:

> **Source:** `tests/c/test_minimal.cpp` snippet `canonical-fixed-shapes-c`

> **Source:** `tests/python/test_minimal.py` snippet `canonical-fixed-shapes`

For **GPU-resident** data the call is the `minimal-write-read` / `write-read-roundtrip-c`
setup above with three field changes and nothing else: `tensor.data` is the CUDA device
pointer instead of a host pointer, `tensor.device = { kDLCUDA, /*device ordinal*/ 0 };`
says so, and the write carries the producer's completion event,
`write.cuda_sync.wait_event = (uintptr_t)producer_event;` — a `cudaEvent_t` recorded after
the producing kernel, which ovstage waits on before copy-in (leave `cuda_sync = {0, 0}` for
CPU-resident or already-synchronized data). See
`examples/c/write-flavors/cuda_ingest.cpp` for the C form. The Python/warp form is:

> **Source:** `examples/python/write-flavors/main.py` snippet `gpu-warp-ingest`

> Read groups return zero `cuda_sync`. For a placed read with an availability event,
> fetch successfully before waiting on that event or ordering the consumer on the
> placement stream; see step 5 above. Keep the group alive until the consumer finishes.

## C — mapped staging write (map / unmap)

Use the map iterator when ovstage should allocate the writable destination buffer. The
unplaced calls return CPU staging rather than a view of attribute storage. The flow:

1. `ovstage_map_attribute(stage, query, &desc, ordinal, element_sizes, element_count, &map)`
   — `desc` is an `ovstage_map_desc_t` (`attribute`, `dtype` (required when *creating* the
   column; `dtype.lanes` = tuple width), `semantic`, `prim_mode`). For fixed-size attributes
   pass `element_sizes = NULL`, `element_count = 0`; for array attributes pass the per-prim
   element counts so ovstage can allocate the ragged staging.
2. `ovstage_fetch_map_next(stage, map, timeout, &map_group)` — iterate writable groups (same
   fetch-with-timeout contract as `fetch_read_next`); write your values into
   `map_group.data.tensors[i].data` plus `byte_offset`. A fixed-size map group uses
   the same scalar-lane typed dimensions as a raw read; strides count scalars.
3. Commit: `ovstage_unmap_group(stage, map, &map_group, write_done_sync)` per group, then
   `ovstage_unmap_attribute(stage, map, write_done_sync)` to commit any remainder and release
   the handle. Wait for each operation to observe its result. `write_done_sync` is an
   `ovstage_cuda_sync_t` that coordinates producer completion; pass `{0, 0}` for these CPU
   buffers. If all groups are committed, final unmap only closes the session.

All map/unmap ops are ordinal-keyed at the session's `ordinal`, like `write_attribute`.
Ready exposes staging only; it does not commit values, new prims or attributes, schemas,
hierarchy connections, or change records, and it records no new ordinal ownership.
Successful group or final commits apply each group's values and any structural changes and
record them once. A later failure does not undo groups committed earlier, and final unmap
does not retry a failed session. See `docs/concepts/error_handling.rst` for the
failure and repair contract.

Readback still requires advancing the relevant write floor after commit. Successful unmap
invalidates affected derived transforms without promising synchronous recomputation; use
the supported flow in `docs/scene/transforms.rst`. A pending or failed parent map adds no
committed hierarchy parent. A committed child may compute as a current root while its parent
is absent and reconnect after that parent's successful commit.

> **Source:** `tests/c/test_map_attribute.cpp` snippet `map-unmap-cpu-c`
>
> Today the mapped buffer is a newly allocated, write-only staging buffer, not a view of
> current values. The commit copy/scatter happens at unmap; true zero-copy storage views are
> not implemented. The Python equivalent maps an existing and a fresh column:
>
> **Source:** `tests/python/test_map_attribute.py` snippet `map-unmap-cpu`

### Read-only vs writable exports

`dlpack(i)` takes a `readonly` keyword that sets the DLPack read-only flag on the exported
capsule. The defaults follow the group kind: a `ReadGroup` exports `readonly=True`, a
(writable) `MapGroup` exports `readonly=False`. Pass `readonly=True` on a map group to hand a
view to code that must not write through it — the consumer then refuses the write instead of
silently corrupting the mapped buffer:

> **Source:** `tests/python/test_map_attribute.py` snippet `map-dlpack-readonly`

Two caveats:

- **The flag needs a versioned capsule.** Only a consumer that requests DLPack `max_version >=
  (1, 0)` receives the versioned layout that carries `flags`; numpy >= 2.1 does. An older
  consumer takes the legacy `DLManagedTensor`, which has no flags field, so `readonly=True` is
  silently dropped and the view is writable. Do not rely on the flag as an access-control
  boundary — it is a correctness hint to a cooperating consumer.
- **Rejection is the consumer's, not ovstage's.** numpy raises `ValueError: assignment
  destination is read-only`; another framework may word it differently or refuse at a
  different point.

## Key Types / Functions

| Symbol | Role |
|--------|------|
| `DLTensor` | the interchange unit: `data`, `device` (`kDLCPU` / `kDLCUDA`), `ndim`, `dtype` (`{code,bits,lanes}`), `shape`, `strides`, `byte_offset` |
| `ovstage_write_data_t { tensors \| managed_tensors; tensor_count; is_array; count; index_map; mask; cuda_sync; semantic }` | copy-in payload (exactly one of `tensors`/`managed_tensors`; `is_array` declares fixed vs array kind, never inferred) |
| `ovstage_data_t { tensors; tensor_count; count; index_map; mask; cuda_sync }` | read / map-group payload (same shape, read or writable) |
| `ovstage_read_group_t { …; data; meta; is_delete; is_array; semantic }` | one read result group; data in `.data` |
| `ovstage_map_group_t { prims; data; meta }` | one writable map group; fill `.data.tensors[i].data` |
| `ovstage_map_desc_t { attribute; dtype; semantic; prim_mode }` | descriptor for a map session |
| `ovstage_cuda_sync_t { uintptr_t stream; uintptr_t wait_event; }` | GPU sync; `{0,0}` = CPU/synced; `wait_event` (a `cudaEvent_t`) = event to wait on before access; `stream` 0 = none, 1 = default stream, >1 = a specific `cudaStream_t` |
| `map_attribute` / `fetch_map_next` / `unmap_group` / `unmap_attribute` | staging write iterator; each successful unmap commits only groups not already committed |

## Troubleshooting

- **Garbage / race on GPU data** — check both handoffs. On read, successfully fetch
  before waiting on the placement availability event; the returned `cuda_sync` is zero.
  On write/unmap, provide the producer's `cuda_sync.wait_event` recorded after its
  kernel. Observe operation errors separately from event completion.
- **Use-after-free of read data** — returned tensors are valid only for the latest snapshot.
  Copy (or otherwise take ownership of) the bytes if they must outlive the read; don't hold
  the borrowed pointer across further commits.
- **`tensors` vs `managed_tensors`** — set exactly one. Use `tensors` for client-owned memory
  that stays valid until the op completes (with a placement `availability_event`, until that
  event completes, or, if the event could not be recorded, until the synchronization
  `ovstage_placement_t` describes); use `managed_tensors` to hand lifetime to storage (deleter
  invoked once no work of the op reads the tensor, which with an availability event can be
  after the op completes).
- **`index_map` and `mask` both set** — they're mutually exclusive; pick gather (`index_map`)
  or validity (`mask`), and set `count` when either is present.
- **`count = 0` used to mean "no elements"** — it means *every prim the query covers*, so a
  `count` computed as the length of an empty selection widens the write to the whole query
  instead of skipping it. With `index_map`/`mask` set it is rejected
  (`INVALID_ARGUMENT: count must be non-zero when index_map or mask is supplied`); the
  Python binding rejects it in all three forms with a `ValueError`.
- **`index_map` used to pick target prims** — it picks *source rows*. `index_map=[1]` on a
  two-prim query does not write the second prim; it tells the first logical element to read
  payload row 1. Use `mask` to select which prims are written.
- **`INVALID_ARGUMENT: index_map references a source row outside the transported row count`**
  — an entry is at or past the payload's row count. Check `shape[0]` (fixed) or
  `tensor_count` (per-row array), and remember that omitting `count` alongside `index_map`
  defaults it to the map's length in the Python binding, which narrows the write to the
  query's leading prims.
- **`INVALID_ARGUMENT: ... does not divide evenly across N source row(s) (from the highest
  index_map entry)`** or **`... is not a whole number of ... elements`** — the packed-array
  counterpart. There the map declares the partition, so a stray entry asks for a row count
  the payload cannot be cut into. Both messages report the row count and where it came from;
  compare it against the rows you meant to transport.
- **`ValueError: index_map must hold one entry per logical element`** — `count` was raised
  past `len(index_map)`. The runtime reads exactly `count` entries, so widening a write means
  lengthening the map (or switching to `mask` to select target prims), not raising `count`.
- **Wrong `tensor_count`** — fixed-size attributes require `tensor_count == 1` (transported
  data rows are stacked along the leading dim); per-row tensors are for array/ragged attributes.
- **Convenience input shape did not round-trip exactly** — the input axes are not retained.
  Reads and maps reconstruct type-aware axes from component width and semantic.
  Use `.reshape(-1)` explicitly for flat NumPy buffer indexing.
- **Creating a mapped attribute fails** — a new column needs `desc.dtype`; a zero-initialized
  dtype only works when the existing schema is unambiguous. An omitted semantic adopts an
  unambiguous compatible identity. Specify both dtype and semantic when target prims have
  multiple same-name identities. Conflicting with existing storage or an authored role on a
  target prim/name fails — delete the attribute first and map/write the new schema at a later
  ordinal, or set `attribute_mode` to `OVSTAGE_ATTRIBUTE_MODE_FORCE` (Python `AttributeMode.FORCE`;
  a map also needs `dtype`) to replace that schema as one change at the same ordinal.
- **Mapped values start empty or unspecified instead of matching the attribute** — expected
  in the current implementation: mapping allocates write-only staging and does not load the
  existing payload. Populate every value you intend to commit before unmapping.

## Python

The Python bindings speak both NumPy and the standard DLPack protocol, with the same
residency and `cuda_sync`/stream sync model. See `project-setup-python` for the package
surface and `error-handling` (Python) for the exception types.

**NumPy (CPU convenience):**

- **Write:** pass a NumPy array (or a list of them for array/ragged attributes) to
  `Stage.write_attribute` / `write_attributes`; `make_dltensor` wraps it in a CPU `DLTensor`
  (`DLDevice{kDLCPU, 0}`) aliasing the array's buffer. Keep the array alive until the op completes
  — the returned `Operation` holds it for you until `.wait()`. For fixed-size attributes, a
  NumPy `(N, 3)` point array or `(N, 4, 4)` matrix array is a convenience write layout; the
  raw read and map results share typed shapes with scalar lanes.
- **Read:** `ReadGroup.array(i)` returns a zero-copy **read-only** NumPy view (CPU). Its
  `WRITEABLE` flag is `False` and cannot be re-enabled, so direct writes through the returned
  array are rejected. Copy it if you need a mutable array.
- **Map:** `MapGroup.array(i)` returns a zero-copy **writable** NumPy view (CPU) — fill it in
  place, then commit via `unmap_group` / `unmap_attribute`. Stop using the map view when
  submitting its group commit or final unmap. Read views expire when their group is released.

**DLPack protocol (numpy / warp / torch / cupy / jax; CPU or CUDA):**

- **Write (ingest):** pass any object exposing `__dlpack__` straight to `write_attribute` /
  `write_attributes` (or build one explicitly with `DLTensor.from_dlpack(obj, stream=...)`). The
  producer's buffer is aliased zero-copy, so a **CUDA device buffer is written without a host
  round-trip**; the source object is retained on the `Operation` until `.wait()`. If a producer
  exposes vector elements as a compact trailing component axis (for example Warp `vec3f` as
  `(N, 3)`, `lanes=1`), `make_dltensor(obj, dtype=float3)` can explicitly fold complete trailing
  axes into lanes without copying, where `float3` is a caller-created
  `DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)`. The fold requires byte-aligned
  source elements, requires a positive bit width, preserves the base type and byte extent, and is
  never inferred automatically, so array-valued `point3f[]` does not become ambiguous with scalar
  arrays. If the fold consumes every source axis (for example `(3,)`, `lanes=1` to `float3`), the
  adapter returns `shape=(1,)`, `ndim=1`. Explicit overrides for that view must match `shape=[1]`,
  `ndim=1`, and compact `strides=[1]`.
- **Read / map (export):** `ReadGroup.dlpack(i)` / `MapGroup.dlpack(i)` return a `ManagedDLTensor`
  implementing `__dlpack__` / `__dlpack_device__`, so `np.from_dlpack(group.dlpack(i))`,
  `wp.from_dlpack(...)`, `torch.from_dlpack(...)` consume it zero-copy on a compatible
  device. Unplaced reads/maps return CPU buffers; placement can select CUDA. Honor
  availability events before consuming payloads and keep groups alive through device work. A read
  group exports read-only; a writable map group exports writable (NumPy ≥ 2.1 honors the flag).
  Native read and map tensors already have scalar lanes and typed dimensions,
  so a matrix4d exports its existing `(N,4,4)` shape. The views borrow storage: release ends a read view's
  lifetime; stop using a map view when submitting its group commit or final unmap. Copy
  values needed beyond the owning group's or session's lifetime.

GPU synchronization remains the caller's responsibility (record/await via `cuda_sync` per the
residency model above; DLPack ingest does not synchronize your streams for you). See
`project-setup-python` for the package surface and `error-handling` (Python) for the exception
types.

> **Source:** `examples/python/minimal/main.py` snippets `minimal-write-read`, `dlpack-interchange`

The public test asserts the three column kinds round-trip — scalar, fixed tuple, and ragged
per-prim array:

> **Source:** `tests/python/test_attribute_shapes.py` snippets `attribute-shapes-fixed`, `attribute-shapes-ragged`

Semantic round-trip (POINT/COLOR/MATRIX/TOKEN_ID), Python:

> **Source:** `tests/python/test_attributes.py` snippet `semantic-roles`

The `make_dltensor` / `dltensor_to_numpy` helpers themselves round-trip a numpy array
through a `DLTensor` (lanes fold into the flat element count on read):

> **Source:** `tests/python/test_support_api.py` snippet `dlpack-round-trip`

## References

- Use the `> **Source:**` directives in this skill to locate tested snippets before reusing API patterns.
- `cpu-ahead-gpu-async` skill — the async enqueue/wait model these tensor ops ride on.
- `error-handling` skill — enqueue/wait/per-op error reporting.
- `application-flow` skill — where tensor exchange sits in the end-to-end lifecycle.
- `path-dictionary` / `string-handling` skills — identifying the prims and attribute a tensor write/read targets.
- Keep related skills, docs, and snippets synchronized when changing the workflow.

> **Source:** `tests/python/test_map_attribute.py` snippet `typed-write-adapter-outer-stride` verifies zero-copy folding, outer stride preservation, and rejection of noncompact component axes.
