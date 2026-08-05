.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

DLPack Tensor Exchange
======================

ovstage exchanges attribute values as DLPack ``DLTensor`` s across three paths:
copy-in writes (``write_attribute``), copy-out reads (``read_attributes`` →
``fetch_read_next``), and a zero-copy map/unmap path that fills ovstage-owned
storage directly. Tensors can be CPU- or CUDA-resident.

The Shared Data Struct
----------------------

One data shape is reused across the API. ``ovstage_data_t`` (reads and map
groups) and ``ovstage_write_data_t`` (writes) both carry:

- ``tensors`` + ``tensor_count`` — an array of ``DLTensor`` s.
- optional sparsity — ``index_map`` *or* ``mask``, with ``count``.
- ``cuda_sync`` — GPU synchronization descriptor (``ovstage_cuda_sync_t``; described below).

``tensor_count`` depends on the attribute kind, which is declared explicitly through
``is_array`` and never inferred:

.. list-table::
   :header-rows: 1

   * - Attribute Kind
     - ``is_array``
     - Tensor layout
   * - Fixed-size (scalar / fixed vector)
     - ``false``
     - A single tensor with all transported data rows stacked along the leading dimension (``tensor_count == 1``).
   * - Array / ragged
     - ``true``
     - One tensor per row, or a single packed tensor for all rows — ``tensor_count`` selects the transport, not the attribute kind.

``DLTensor`` fields are the standard DLPack set: ``data``, ``device``
(``{kDLCPU, 0}`` or ``{kDLCUDA, ordinal}``), ``ndim``, ``dtype``
(``{code, bits, lanes}`` — ``lanes`` is the tuple width, e.g. 3 for a float3),
``shape``, ``strides``, and ``byte_offset``.

Fixed-Size Canonical Layout
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Fixed-size tensors returned by raw reads and maps use a lane-canonical layout:

- ``tensor_count == 1`` and ``ndim == 1``.
- ``shape[0] == N``, where ``N`` is the transported data-row count.
- ``dtype.lanes`` is the complete tuple width.

Logical prims select those data rows one-to-one when ``index_map`` is NULL. If
``index_map`` is present, logical prim ``i`` selects row ``index_map[i]`` and
``N`` may differ from ``count``: it can be smaller when rows are shared, or
larger when a query touches only part of a transported bucket.

Copy-in writes may use either the canonical form or compact convenience
dimensions. Convenience dimensions are folded into ``dtype.lanes`` and are not
preserved as schema.

Without ``index_map``, the leading dimension must equal the logical element
count. A flat ``shape = [N * L]``, ``lanes = 1`` tensor is not a convenience
encoding of ``N`` rows of width ``L``; use ``shape = [N, L]``, ``lanes = 1`` or
the canonical ``shape = [N]``, ``lanes = L`` form.

.. list-table::
   :header-rows: 1

   * - Value per data row
     - Canonical write / raw read / raw map
     - Accepted convenience write
     - Python DLPack export
   * - Scalar
     - ``shape = [N]``, ``lanes = 1``
     - Same
     - ``(N,)``
   * - ``point3f``
     - ``shape = [N]``, ``lanes = 3``
     - ``shape = [N, 3]``, ``lanes = 1``
     - ``(N, 3)``
   * - ``matrix4d``
     - ``shape = [N]``, ``lanes = 16``
     - ``shape = [N, 4, 4]``, ``lanes = 1`` (or ``[N, 4]`` with 4 lanes)
     - ``(N, 16)``

Python DLPack export expands a multi-lane dtype by exactly one trailing axis;
it does not reconstruct the convenience input shape. Array/ragged attributes
are outside this fixed-size layout rule.

.. important::

   In raw OVStage fixed-size transport, ``dtype.lanes`` is the canonical
   encoding of the complete component width; the attribute semantic separately
   supplies the geometric role. For example, three ``point3f`` data rows use
   ``shape = [3]``, ``lanes = 3``, and ``OVSTAGE_SEMANTIC_POINT``.

   ``shape = [3, 3]``, ``lanes = 1`` is accepted as a convenience write, but
   OVStage normalizes it. Raw reads and maps return ``shape = [3]``,
   ``lanes = 3`` — not ``shape = [9]``, ``lanes = 1``. The original trailing
   shape is not preserved. Python's ``ReadGroup.array(0)`` is a separate flat
   base-element view of length 9; use ``ReadGroup.dlpack(0)`` when a consumer
   should see the expanded ``(3, 3)`` lane axis.

Copy-In Write and Copy-Out Read
-------------------------------

The minimal example builds a ``DLTensor`` over a CPU buffer, writes it at an
ordinal, seals the ordinal, and reads the column back:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:minimal-write-read]
         :end-before: # [/snippet:minimal-write-read]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:minimal-write-read]
         :end-before: // [/snippet:minimal-write-read]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

For a write, exactly one of ``tensors`` (client-managed; must stay valid until
the op completes) or ``managed_tensors`` (storage takes ownership through the
``DLManagedTensorVersioned`` deleter) is non-NULL.

In Python, the bindings accept and return DLPack-compatible arrays, so NumPy,
PyTorch, and Warp tensors interchange through ``from_dlpack`` with the same
residency model.

Some producers expose library vector types as scalar DLPack tensors with a
trailing component axis. For example, Warp exports ``vec3f`` as ``(N, 3)`` with
``lanes = 1``. For an array-valued ``point3f[]`` row, first create
``float3 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)``, then
call ``make_dltensor(warp_points, dtype=float3)``. The adapter aliases the same
allocation while folding only complete compact trailing axes into
``dtype.lanes``. It validates the unchanged base type, a positive bit width, and
byte extent, and requires byte-aligned source elements; no automatic shape-based
inference is performed, so a scalar array with shape ``(N, 3)`` keeps its
original meaning unless the caller requests the lane fold.
When folding consumes every source axis, such as ``(3,)`` into a three-lane
dtype, the adapter normalizes the mathematically rank-zero result to
``shape=(1,)``, ``ndim=1``: a one-element tensor view compatible with ovstage's
leading-dimension transport.
Explicit overrides for this view must use ``shape=[1]``, ``ndim=1``, and compact
``strides=[1]``.

GPU Residency and Synchronization
---------------------------------

``cuda_sync`` (``ovstage_cuda_sync_t``) coordinates GPU producer/consumer
ordering, and remains the caller's responsibility:

.. code-block:: c

   typedef struct {
       uintptr_t stream;     /* 0 = none, 1 = the default stream, >1 = a specific cudaStream_t */
       uintptr_t wait_event; /* 0 = none, else a cudaEvent_t to wait on */
   } ovstage_cuda_sync_t;

The two fields are **independent knobs** — set either, both, or neither; each
non-zero field adds its own synchronization before the op:

- ``stream`` — ``0`` = none; ``1`` = the default stream; ``>1`` = a specific
  ``cudaStream_t``. When set, all work currently queued on that stream is
  drained.
- ``wait_event`` — ``0`` = none; otherwise a ``cudaEvent_t`` that is waited on.
- ``{0, 0}`` — no synchronization (CPU-resident or already-synchronized data).

On a **read**, the synchronization is applied before your code accesses the
returned data. On a **write / unmap**, it is what ovstage waits on before
sealing your data.

Sparsity
--------

``index_map`` (gather / reorder / dedup) and ``mask`` (per-element validity) are
**mutually exclusive**. Set ``count`` (the logical element count) whenever either
is present.

They address different axes. ``index_map`` picks the **source row** each logical
element reads (``index_map[i]`` is a row of the payload, not a prim), so it
gathers, reorders, or broadcasts data. ``mask`` picks the **target elements** to
write, leaving the rest untouched. To write a subset of a query's prims, use
``mask``.

Where the payload declares a transported row count — ``shape[0]`` for a
fixed-size payload, ``tensor_count`` for per-row array transport — every
``index_map`` entry must be below it, and an out-of-range entry is rejected with
``OVSTAGE_ERROR_INVALID_ARGUMENT``. Unreferenced rows are left unused, so the
payload may be wider than the query. Packed array transport carries no row count
of its own, so the map defines the partition instead: the payload is cut into
``max(index_map) + 1`` uniform rows, and a partition the payload cannot support
(one that does not divide evenly, or whose rows are not a whole number of
``dtype`` elements) is rejected with ``OVSTAGE_ERROR_INVALID_ARGUMENT``. A
``mask`` leaves the row partition alone, so a masked payload must still carry a
row for every logical element, including the unselected ones.

Zero-Copy Map/Unmap
-------------------

.. note::

   The map/unmap path below is documented from the shipped headers; the current
   examples cover only the CPU, copy-in path, so this flow is **not yet
   snippet-backed**. Treat the sequence as an API reference and refer to
   :doc:`/c_api/index` for exact signatures.

Instead of copying data in, a producer can fill ovstage-owned storage directly:

1. ``ovstage_map_attribute`` with an ``ovstage_map_desc_t``
   (``{ attribute; dtype; semantic; prim_mode }``) — creating a new column
   requires ``desc.dtype``.
2. ``ovstage_fetch_map_next`` to obtain each map group.
3. Fill ``group.data.tensors[i].data`` in place.
4. ``ovstage_unmap_group`` per group, then ``ovstage_unmap_attribute`` to commit
   the remainder and release, passing a ``write_done_sync``
   (an ``ovstage_cuda_sync_t``).

All map/unmap ops are ordinal-keyed at the session ``ordinal``. Changing an
existing prim/name's dtype or semantic fails — delete the attribute first.

.. note::

   This build retains only the **latest committed** snapshot. Returned tensor
   data is valid for that snapshot only — copy, retain, or transfer ownership to
   use it later; do not hold a borrowed pointer across further commits.

Where to Go Next
----------------

- :doc:`/scene/writing_attributes` / :doc:`/scene/reading_attributes` — the write and read paths in context.
- :doc:`async_model` — the enqueue/wait model these exchanges ride on.
