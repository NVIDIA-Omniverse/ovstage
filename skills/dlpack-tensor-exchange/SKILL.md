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
- The shipped headers (`ovstage_api/ovstage_api_types.h` for the data structs, the bundled
  `<dlpack/...>` for `DLTensor`/`DLDataType`/`DLDevice`) are the authoritative contract.

## Prerequisites

- Use an ovstage checkout that contains the `include/` headers and the referenced example/snippets.
- Read the relevant `> **Source:**` snippet before writing or explaining API usage.
- Understand the async enqueue/observe model first (see `cpu-ahead-gpu-async`): tensor ops
  are **enqueues** — the data is not produced/consumed until you wait or fetch.
- For GPU-resident data, know your CUDA event/stream ownership: **GPU synchronization
  between producer and consumer is the caller's responsibility** (ovstage coordinates via
  `cuda_sync`, it does not manage your streams).

## Instructions

1. Pick the path: copy-in (`write_attribute`), copy-out (`read_attributes` /
   `fetch_read_next`), or mapped staging write (`map_attribute` / `fetch_map_next` / `unmap_*`).
2. Describe data with a `DLTensor`: `data` pointer, `device` (`{kDLCPU,0}` or
   `{kDLCUDA, ord}`), `ndim`, `dtype` (`{code, bits, lanes}` — `lanes` is the tuple width, e.g.
   3 for a float3), `shape`, `strides`, `byte_offset`. Fixed-size read and map tensors use
   the canonical transport layout: `ndim = 1`, `shape = [data_rows]`, and `dtype.lanes`
   holds the complete tuple width.
3. **Write (copy-in):** put the tensor(s) in `ovstage_write_data_t.tensors` (client-managed —
   must stay valid until the op completes) **or** `.managed_tensors` (storage takes ownership
   via the `DLManagedTensorVersioned` deleter); exactly one is non-NULL. Set `tensor_count`
   (1 for fixed-size).
4. **GPU-resident write:** set `write_data.cuda_sync.wait_event` to an event recorded *after*
   your producing kernel — ovstage waits on it before copying in — and `.stream` to the CUDA
   stream it was recorded on (`0` = default stream). Leave `cuda_sync` `{0, 0}` for CPU or
   already-synchronized data.
5. **Read (copy-out):** after `fetch_read_next`, if `group.data.cuda_sync.wait_event` is
   non-zero, wait on it (e.g. `cuStreamWaitEvent`) before touching `tensors[i].data`. Treat the
   data as valid only for the current snapshot — copy/retain it to use beyond the immediate
   read. Release the group with `release_group`.
6. **Mapped staging write:** `fetch_map_next` hands back a writable `ovstage_map_group_t`;
   write your values into `group.data.tensors[i].data` (CPU or via a GPU kernel), then commit
   with `unmap_group` (per group) or `unmap_attribute` (commit remaining + release the
   handle), passing a write-done `ovstage_cuda_sync_t` whose `wait_event` ovstage waits on
   before sealing.
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
  `fetch_map_next` returns a newly allocated, write-only buffer; it is not a view of the
  attribute's current value and is not initialized from existing storage. `unmap_group` or
  `unmap_attribute` copies/scatters the staged bytes into backing storage, then releases the
  staging allocation. Treat map/unmap as a write-only replacement path and account for the
  extra allocation and commit copy.
- **CPU DLTensor write/read is snippet-backed** (the minimal example), and this build adds
  snippet-backed **CPU map/unmap** (`map-unmap-cpu`, the public map test) and **Python CUDA
  ingest with warp** (`gpu-warp-ingest`, write-flavors — GPU-gated at runtime). The **C-side
  GPU-resident write (`cuda_sync` sync) remains described from the headers only** — the C
  examples stay CPU-only, so the headers are the contract for that path.
- **Latest-snapshot build:** returned tensor data is valid only for the latest committed
  snapshot. To use it later you must take explicit ownership (copy / retain / transfer) — do
  not hold a borrowed pointer across further commits.
- **GPU sync is the caller's responsibility** — ovstage coordinates via `cuda_sync` but does
  not own or synchronize your streams.
- **Python `managed_tensors` transfer is not exposed** — DLPack ingest/export (incl. CUDA) is
  supported (see the Python section), but the Python write path uses only the client-managed
  `tensors` field: the caller keeps the source object alive until the op completes, rather than
  handing ownership to storage via `managed_tensors`.
- **Fixed-size shape is normalized, not preserved.** Compact convenience write shapes are
  accepted, but trailing component dimensions are folded into `dtype.lanes`; raw C reads and
  maps return the canonical 1-D row layout. This statement does not cover array/ragged
  attributes, whose tensor shapes describe per-prim elements.
- **⚠️ Draft — API in flux.** Treat exact symbols/usage as provisional against the headers.

## Overview

ovstage exchanges attribute values as **DLPack `DLTensor`s**, with a map/unmap write path that
follows a zero-copy programming model. Zero-copy is the intended direction, but the current
build is staging-backed: map hands back a write-only buffer that is copied into backing storage
at unmap, not a direct view. One data shape is reused throughout:

- **`ovstage_data_t`** (reads and map groups) and **`ovstage_write_data_t`** (writes) both
  carry: an array of `DLTensor`s (`tensors` + `tensor_count`), optional sparsity
  (`index_map` *or* `mask`, with `count`), and a GPU-sync `cuda_sync` (`{stream, wait_event}`).
- **`tensor_count` depends on attribute kind:** fixed-size attributes use a single tensor
  with all transported data rows stacked along the leading dimension; array (ragged)
  attributes use one tensor per transported data row.
- **Fixed-size tensors are lane-canonical on read and map:** the leading dimension is the
  transported data-row count and `dtype.lanes` is the full tuple width. Logical elements
  select rows directly or through `data.index_map`. Writes may use this canonical layout or
  a compact convenience shape; convenience trailing dimensions are folded and are not
  retained as schema metadata.
- **Three paths:** *copy-in* (`write_attribute`, implementation copies/scatters your tensors
  into storage), *copy-out* (`read_attributes` → `fetch_read_next`, you read from the
  returned tensors), and *mapped staging* (`map_attribute` → `fetch_map_next`, you fill a
  write-only staging buffer that is copied/scattered into backing storage by `unmap_*`).
- **`cuda_sync` semantics** (`ovstage_cuda_sync_t { uintptr_t stream; uintptr_t wait_event; }`,
  where `wait_event` is a `CUevent` and `stream` a `CUstream` as `uintptr_t`; `stream` 0 = no
  sync / default, 1 = default stream, >1 = a specific stream): `{0, 0}` means CPU-resident or
  already-synchronized; a non-zero `wait_event` on a **read** means *wait before access*; on a
  **write/unmap** it is the event ovstage *waits on* (relative to `stream`) before sealing your data.

### Fixed-size canonical and convenience layouts

For fixed-size attributes (`is_array = false`), use the canonical layout when shape stability
matters across a raw C API round trip. Convenience layouts are accepted on copy-in, but only
their leading dimension is the source data-row count; all trailing component dimensions are
folded into the canonical lane width. In this table, `N` is the source/transported data-row
count. When `data.index_map` is present, `N` may differ from the logical prim count: it can
be smaller when rows are shared or larger when a query touches only part of a transported
bucket. Without `index_map`, `N` must equal the logical prim count. A flat `(N * L,)`,
`lanes = 1` tensor is not a convenience encoding of `N` rows of width `L`; use `(N, L)` or
canonical lanes.

| Value per data row | Canonical write / raw read / raw map | Accepted convenience write | Python DLPack export |
|---|---|---|---|
| scalar | `shape = [N]`, `lanes = 1` | same | `(N,)` |
| point/vector/color (`float3`) | `shape = [N]`, `lanes = 3` | `shape = [N, 3]`, `lanes = 1` | `(N, 3)` |
| matrix (`matrix4d`) | `shape = [N]`, `lanes = 16` | `shape = [N, 4, 4]`, `lanes = 1`, or `shape = [N, 4]`, `lanes = 4` | `(N, 16)` |

The original convenience shape is not preserved: a matrix written as `(N, 4, 4)` is read and
mapped by the raw API as `shape = [N]`, `lanes = 16`. Python's DLPack protocol export adds
exactly one trailing axis for the lane width, so it produces `(N, 16)`, not `(N, 4, 4)`.
Array/ragged attributes are outside this fixed-size layout rule.

## C — write and read (copy-in / copy-out)

The minimal example builds a CPU `DLTensor`, writes it through `ovstage_write_data_t`, seals
it by advancing the write floor, and reads the column back from `group.data.tensors[0].data`
— the end-to-end CPU shape for both directions:

> **Source:** `examples/c/minimal/main.cpp` snippet `minimal-write-read`

The public test asserts the three column shapes round-trip — a 1-lane scalar, a fixed
multi-lane tuple (lanes in the dtype, not the shape), and a ragged per-prim array:

> **Source:** `tests/c/test_attribute_shapes.cpp` snippets `attribute-shapes-fixed-c`, `attribute-shapes-ragged-c`

A **semantic** is the authored USD meaning of a column's bytes, orthogonal to the storage
dtype (POINT/COLOR/MATRIX on float storage; TOKEN_ID pins uint64 token-id storage). The write
stamps it and the read recovers it — the public test asserts the round-trip:

> **Source:** `tests/c/test_attributes.cpp` snippet `semantic-roles-c`

The public contract test also authors a matrix with the convenience input layout
`shape = [3, 4, 4]`, `lanes = 1`, then observes the canonical raw read layout
`shape = [3]`, `lanes = 16`:

> **Source:** `tests/c/test_minimal.cpp` snippet `canonical-fixed-shapes-c`

> **Source:** `tests/python/test_minimal.py` snippet `canonical-fixed-shapes`

For **GPU-resident** data the only additions are the device and a producing event (the rest
of the call is identical to the snippet above):

```c
DLTensor t{};
t.data   = d_ptr;                  // CUDA device pointer
t.device = { kDLCUDA, /*ordinal*/ 0 };
t.ndim   = 1;
t.dtype  = { kDLFloat, 32, 1 };    // {code, bits, lanes}; lanes = tuple width
t.shape  = shape;
t.strides = strides;

ovstage_write_data_t w{};
w.tensors      = &t;
w.tensor_count = 1;
w.cuda_sync.wait_event = my_kernel_done_event;  // ovstage waits on this before copy-in ({0,0} if CPU/synced)
w.is_array     = false;                 // explicit fixed-size attribute kind
// ovstage_write_attribute(stage, query, attrArg, /*ordinal*/ ord, w, OVSTAGE_PRIM_MODE_UPSERT)
```

> **Source:** `examples/python/write-flavors/main.py` snippet `gpu-warp-ingest`

> The runnable GPU ingest is Python/warp — the C examples stay CPU-only. On read, mirror
> this: if `group.data.cuda_sync.wait_event` is non-zero, wait on it before reading
> `tensors[i].data`.

## C — mapped staging write (map / unmap)

For GPU kernels (or DMA) that need ovstage to allocate a writable destination buffer, use the
map iterator. In the current implementation this is staging-backed rather than a direct view
of backing storage. The flow:

1. `ovstage_map_attribute(stage, query, &desc, ordinal, element_sizes, element_count, &map)`
   — `desc` is an `ovstage_map_desc_t` (`attribute`, `dtype` (required when *creating* the
   column; `dtype.lanes` = tuple width), `semantic`, `prim_mode`). For fixed-size attributes
   pass `element_sizes = NULL`, `element_count = 0`; for array attributes pass the per-prim
   element counts so storage can pre-allocate the ragged backing.
2. `ovstage_fetch_map_next(stage, map, timeout, &map_group)` — iterate writable groups (same
   fetch-with-timeout contract as `fetch_read_next`); write your values into
   `map_group.data.tensors[i].data`. A fixed-size map group uses the same canonical
   `shape = [data_rows]`, full-`dtype.lanes` layout as a raw read.
3. Commit: `ovstage_unmap_group(stage, map, &map_group, write_done_sync)` per group, then
   `ovstage_unmap_attribute(stage, map, write_done_sync)` to commit any remainder and release
   the handle. `write_done_sync` is an `ovstage_cuda_sync_t` whose `wait_event` ovstage waits
   on before sealing your writes (`{0, 0}` if the writes are already complete/CPU).

All map/unmap ops are ordinal-keyed at the session's `ordinal`, like `write_attribute`.

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
| `ovstage_cuda_sync_t { uintptr_t stream; uintptr_t wait_event; }` | GPU sync; `{0,0}` = CPU/synced; `wait_event` (a `CUevent`) = event to wait on before access; `stream` 0/1 = default, >1 = specific `CUstream` |
| `map_attribute` / `fetch_map_next` / `unmap_group` / `unmap_attribute` | staging-backed write iterator; commit occurs at unmap |

## Troubleshooting

- **Garbage / race on GPU data** — you didn't honor the `cuda_sync`. On read, wait on
  `group.data.cuda_sync.wait_event` before access; on write/unmap, pass a `cuda_sync` whose
  `wait_event` was recorded after your kernel so ovstage doesn't seal before your writes finish.
- **Use-after-free of read data** — returned tensors are valid only for the latest snapshot.
  Copy (or otherwise take ownership of) the bytes if they must outlive the read; don't hold
  the borrowed pointer across further commits.
- **`tensors` vs `managed_tensors`** — set exactly one. Use `tensors` for client-owned memory
  that stays valid until the op completes; use `managed_tensors` to hand lifetime to storage
  (deleter invoked when no longer needed).
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
- **Convenience input shape did not round-trip** — expected for fixed-size attributes. Inspect
  the raw result as `shape = [data_rows]` with the complete tuple width in `dtype.lanes`;
  use the canonical input form when the same descriptor must be reusable without normalization.
- **Creating a mapped attribute fails** — a new column needs `desc.dtype`; a zero-initialized
  dtype only works when the existing schema is unambiguous. Changing an existing prim/name's
  dtype or semantic fails — delete the attribute first, then map/write the new schema.
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
  raw result is normalized to `(N,)` with 3 or 16 lanes.
- **Read / map:** `ReadGroup.array(i)` / `MapGroup.array(i)` return a zero-copy NumPy **view**
  (CPU), valid only until the group/result is released.

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
  `wp.from_dlpack(...)`, `torch.from_dlpack(...)` consume it zero-copy (CPU **or** CUDA). A read
  group exports read-only; a writable map group exports writable (NumPy ≥ 2.1 honors the flag).
  Each vector dtype is exported by adding exactly one trailing lane axis. Thus a canonical
  fixed-size matrix tensor `shape = [N]`, `lanes = 16` exports as `(N, 16)`, not as the
  convenience input shape `(N, 4, 4)`. Borrowed lifetime: valid only until the group/result is
  released — copy it to outlive the read.

GPU synchronization remains the caller's responsibility (record/await via `cuda_sync` per the
residency model above; DLPack ingest does not synchronize your streams for you). See
`project-setup-python` for the package surface and `error-handling` (Python) for the exception
types.

> **Source:** `examples/python/minimal/main.py` snippets `minimal-write-read`, `dlpack-interchange`

The public test asserts the three column shapes round-trip — scalar, fixed multi-lane
(lanes in the dtype), and ragged per-prim array:

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
