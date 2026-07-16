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
  fixed-size attribute (one stacked tensor for all prims), ``true`` for a
  ragged/array attribute (a single packed tensor or one tensor per prim). Refer to :doc:`/concepts/dlpack_tensors`.
- **tensors** — the DLPack payload; can be CPU- or CUDA-resident.

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
