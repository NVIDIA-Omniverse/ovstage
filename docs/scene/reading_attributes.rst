.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Reading Attributes
==================

``read_attributes`` requests one or more attribute columns over a
:doc:`query </scene/queries>` and returns the results as **groups** you fetch and
then release. Reads target sealed data at or below the write floor, so advance
the floor after writing (refer to :doc:`writing_attributes`) before reading the data
back.

Read → Fetch → Release
----------------------

The minimal example writes a column, seals it, then reads it back — mapping the
returned tensor as a zero-copy view before releasing the group and the read:

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

Groups and Lifetime
-------------------

- ``ovstage_read_attributes`` enqueues the read and returns a read handle.
- ``ovstage_fetch_read_next`` yields the next ``ovstage_read_group_t`` —
  each group carries a prim group and its ``ovstage_data_t`` tensors — until
  ``OVSTAGE_ERROR_END_OF_ITERATION``.
- Release each group with ``ovstage_release_group``, then the read with
  ``ovstage_release_read``. In Python the ``Read`` handle and its groups clean up
  through their normal object lifetime.

A group's tensor data is a borrowed view into the latest committed snapshot.
Copy it if you need it after the next commit (refer to :doc:`/concepts/dlpack_tensors`).

Reading Built-in Metadata
-------------------------

ovstage auto-maintains reserved metadata attributes — ``usd-path``,
``usd-schemas``, ``usd-prim-type``, ``usd-parent``, ``usd-children`` — which
you read like any other column; they cross as ``uint64`` token ids you resolve
through the :doc:`path dictionary </scene/path_dictionary>`. The runtime-loop
example reads ``usd-prim-type`` to confirm a populate landed; refer to
:doc:`/guides/runtime_loop`.

``usd-path``, ``usd-parent``, and ``usd-children`` are derived on demand
rather than stored: a latest read synthesizes point-in-time group(s) at
``range.end_ordinal`` (``usd-children`` batches ragged rows like other array
reads, so iterate groups to the end as usual), while a range
("since"/"between") read reports them as never changed (zero groups) — they
keep no per-write change stream, so poll current values with latest reads.
The synthesized values reflect the latest committed structural state, like
query membership itself. Prims without a value (a root prim's ``usd-parent``,
a leaf prim's ``usd-children``) contribute no row, and writing, deleting, or
mapping any of the four derived names (including ``usd-active``) is rejected
with ``NOT_SUPPORTED``.

.. note::

   ``usd-active`` is currently **not supported**: a live prim is always
   active, so the attribute carries no information, and reads or filter
   predicates naming it return ``NOT_SUPPORTED``. The name remains in the
   header contract for stability only and is subject to removal in a future
   release.

.. note::

   This build retains only the **latest committed** state, so reads return the
   latest sealed value. Do not design flows that read back older ordinals.

Where to Go Next
----------------

- :doc:`queries` — build the query a read runs over.
- :doc:`writing_attributes` — produce the data you read back.
- :doc:`/concepts/dlpack_tensors` — interpreting the returned tensors and their residency.
