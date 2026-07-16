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
     - A single tensor with all prims stacked along the leading dimension (``tensor_count == 1``).
   * - Array / ragged
     - ``true``
     - One tensor per row, or a single packed tensor for all rows — ``tensor_count`` selects the transport, not the attribute kind.

``DLTensor`` fields are the standard DLPack set: ``data``, ``device``
(``{kDLCPU, 0}`` or ``{kDLCUDA, ordinal}``), ``ndim``, ``dtype``
(``{code, bits, lanes}`` — ``lanes`` is the tuple width, e.g. 3 for a float3),
``shape``, ``strides``, and ``byte_offset``.

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
