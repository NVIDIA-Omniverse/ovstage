.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Writing Attributes
==================

``write_attribute`` copies a column of data — one value per prim in a
:doc:`query </scene/queries>` — into the stage at an explicit ``ordinal``. The
write is asynchronous: it enqueues and returns an ``op_index``, and the data
becomes readable only after you advance the write floor to seal that ordinal.

The Write → Seal Sequence
-------------------------

The minimal example builds a ``DLTensor`` over one float per prim, writes it at
ordinal 1, and advances the write floor:

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

Key Parameters
--------------

- **attribute** — the column key, given as a token or string (refer to
  :doc:`/concepts/string_handling`).
- **ordinal** — must be **above** the current write floor, or the write is
  rejected with ``OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION``.
- **is_array** — declares the attribute kind explicitly: ``false`` for a
  fixed-size attribute (one tensor with transported data rows stacked along
  its leading dimension), ``true`` for a ragged/array attribute (a single
  packed tensor or one tensor per data row). Refer to :doc:`/concepts/dlpack_tensors`.
- **tensors** — the DLPack payload; can be CPU- or CUDA-resident.

Fixed-Size Write Shapes
-----------------------

The canonical fixed-size input has ``shape = [N]`` and places the complete
per-row tuple width in ``dtype.lanes``. Convenience inputs such as ``(N, 3)``
with one lane for a point or ``(N, 4, 4)`` with one lane for a matrix are also
accepted. Their trailing dimensions are folded into lanes and are not
preserved: raw reads and maps return ``(N,)`` with 3 or 16 lanes. Here ``N`` is
the source data-row count; ``index_map`` can associate multiple logical target
prims with the same source row. Without ``index_map``, ``N`` must equal the
logical target count. A flat ``(N * L,)`` one-lane tensor is not inferred as
``N`` rows of width ``L``; use ``(N, L)`` or canonical lanes. Array/ragged
attributes do not use this rule.

Array Write Element Widths
--------------------------

Array writes do **not** fold trailing dimensions into lanes. ``dtype.lanes`` is
the element width and is taken exactly as sent, so an array payload's element
count is ``total_bytes / (bits * lanes / 8)`` regardless of its shape. A
``(P, 3)`` one-lane tensor is therefore ``3 * P`` scalar elements, not ``P``
three-component ones — the opposite of the fixed-size rule above.

This matters because ``(P, 3)`` with one lane is what NumPy and Warp emit for a
``vec3f`` array. Writing one against an **existing** ``float3[]`` column is
rejected, but on an attribute that does not exist yet nothing contradicts it:
the write succeeds and creates a ``float[]`` column of ``3 * P`` scalars. This
is the one place where a descriptor that omits the producer's intent yields a
wrong schema rather than an error.

State the element width on the descriptor rather than relying on the shape.
``make_dltensor`` re-describes the producer's buffer in place — a validated,
metadata-only change with no copy:

.. code-block:: python

   float3 = ovstage.DLDataType(code=ovstage.DLDataTypeCode.kDLFloat, bits=32, lanes=3)
   points = ovstage.make_dltensor(warp_points, dtype=float3)  # (P,3) lanes=1 -> (P,) lanes=3

Targeting a Subset of the Query
-------------------------------

``count`` is the number of logical elements a write addresses — the leading
``count`` prims of the query, in query order. It defaults to the query's full
prim count and may not exceed it. Two mutually exclusive parameters refine that
element axis, and in the C API both require an explicit non-zero ``count``:

- **index_map** selects *source data*, not targets. ``index_map[i]`` is the
  transported row that logical element ``i`` reads from, so it gathers,
  reorders, or broadcasts rows — ``index_map = [0, 0]`` writes one source row to
  two prims. The map holds one entry per logical element. Where the payload
  declares its own row count — ``shape[0]`` for a fixed-size write,
  ``tensor_count`` for per-row array transport — every entry must be below it,
  and an out-of-range entry is rejected with
  ``OVSTAGE_ERROR_INVALID_ARGUMENT``. Rows the map never references are simply
  unused, so a payload may be wider than the query it is written through.
  Packed array transport declares no row count of its own, so there the map
  *defines* one: the payload is cut into ``max(index_map) + 1`` uniform rows.
  Rows above the highest entry cannot be expressed that way — use one tensor
  per row when rows must be described individually.
- **mask** selects *targets*: a bitmask over the ``count`` logical elements
  where only set bits are written, leaving the remaining prims untouched. Use
  this — not ``index_map`` — to write some prims of a wider query. A mask does
  not change how the payload is cut into rows, so the payload must still carry
  a row for every one of the ``count`` logical elements, including the
  unselected ones. Element ``i`` is bit ``i % 64`` of word ``i / 64``, so a
  non-null mask must contain at least ``ceil(count / 64)`` ``uint64_t`` words —
  the runtime reads exactly that many, and a shorter buffer is read past its
  end.

A common mistake is reaching for ``index_map`` to pick target prims. To write
only the second prim of a two-prim query, use ``mask`` with the second bit set,
or build a query that covers just that prim.

.. note::

   In Python, ``count`` is filled in for you when ``index_map`` is given without
   it (defaulting to ``len(index_map)``, which addresses only the query's
   leading prims). ``mask`` has no default: supply ``count`` and enough 64-bit
   words to cover it, or the binding raises ``ValueError``. The query-prim-count
   default applies only when neither parameter is present. An explicit ``count``
   must be positive there — ``0`` is this contract's spelling of "the whole
   query", so the binding rejects it rather than let ``len()`` of an empty
   selection widen a write to every prim.

Sealing with the Write Floor
----------------------------

A write is not observable until ``ovstage_advance_write_floor`` seals its
ordinal. Advancing the floor is monotonic in effect: a backwards advance clamps
rather than erroring. Reads then target data at or below the floor.

Attribute Semantics
-------------------

Writes carry an ``ovstage_attribute_semantic_t`` (``AttributeSemantic`` in
Python; ``NONE`` by default). Geometric semantics stamp a role on the column —
for example a 4×4 transform is written with the ``MATRIX`` semantic. Identity
semantics (token / relationship / connection path ids) pin the column's base
type and require pre-interned id payloads. For a worked transform-write example
over successive ordinals, refer to :doc:`/guides/runtime_loop`.

Passing the Attribute as String or Token
-----------------------------------------

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:string-or-token-arg]
         :end-before: # [/snippet:string-or-token-arg]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:string-or-token-arg]
         :end-before: // [/snippet:string-or-token-arg]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Where to Go Next
----------------

- :doc:`reading_attributes` — read the sealed column back.
- :doc:`/concepts/dlpack_tensors` — tensor layout, residency, and the zero-copy map/unmap alternative.
- :doc:`/concepts/async_model` — ordinals, the write floor, and observing the write.
