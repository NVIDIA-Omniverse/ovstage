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

:meth:`ovstage.Stage.read_attributes()` reads :term:`attribute <Attribute>`
values for a :doc:`query <queries>`. It returns a read handle; fetch its groups
of results and release each group after use.

By default, reads return CPU buffers. The experimental ``placement=`` argument
can select a CUDA copy instead. A placed read with an availability event requires
device synchronization after fetching a group and before using its payload;
see :ref:`ovstage-device-placement`. NumPy ``group.array()`` is CPU-only.

First advance the :term:`watermark <Watermark>` to cover the changes you need,
and wait for that advance. See :doc:`/concepts/overview` for the read model.

Read → Fetch → Release
----------------------

This example writes values, advances the watermark, and reads them through a
NumPy view in Python or a tensor pointer in C. It then releases the result group.
Python releases the read handle on context-manager exit; the C example also
releases it during cleanup.

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

1. Keep the read handle in a ``with`` block and call :meth:`read.wait() <ovstage.Read.wait>`.
2. Fetch all results with :meth:`read.groups() <ovstage.Read.groups>`, or call :meth:`read.fetch_next() <ovstage.Read.fetch_next>` until
   it returns ``None``. Do not assume one group contains the whole result.
3. Release each group with its own ``with`` block or
   :meth:`ovstage.Stage.release_group()`.
4. Release the read handle after fetching its results. Context-manager exit
   does this for you.

:meth:`read.wait() <ovstage.Read.wait>` observes execution errors and releases operation tracking. It
returns the handle, so :meth:`read.wait().fetch_next() <ovstage.Read.fetch_next>` is valid inside the read's
``with`` block. Keep the handle in a variable; chaining directly from
:meth:`stage.read_attributes(...) <ovstage.Stage.read_attributes>` leaves it to the finalizer.

``group.semantic`` gives the attribute's :class:`ovstage.AttributeSemantic`.
If the native library supplies a value unknown to the Python bindings, it returns
that value as an integer.

Groups own their storage separately from the read handle. Releasing the handle
does **not** release its groups. An unreleased group causes overlapping writes
to the same attributes and prims to fail. Dropping a group without releasing it triggers
a finalizer and ``ResourceWarning``; release it explicitly instead.

Both :meth:`group.array(i) <ovstage.ReadGroup.array>` and DLPack consumer arrays borrow the group's memory.
They do not keep the group alive. Never take a view from a temporary such as
:meth:`read.fetch_next().array(0) <ovstage.ReadGroup.array>` or
``np.from_dlpack(read.fetch_next().dlpack(0))``. Keep the group alive while using
its views, and copy any values that must outlive it. The group rejects access
after release, but previously returned views cannot detect that release.
See :doc:`/concepts/dlpack_tensors`.

In C, :c:func:`ovstage_fetch_read_next()` returns groups until
``OVSTAGE_ERROR_END_OF_ITERATION``. Release each with
:c:func:`ovstage_release_group()`, then release the handle with
:c:func:`ovstage_release_read()`.


These snippets copy each selected ``heat`` value before releasing its group.
The query selects ``/World/Writes/A``, ``/World/Writes/B``, and ``/World/Writes/C``;
the earlier write examples set their values to 300, 400, and 200, and advance
the watermark to 15. ``paths`` is the stage's :class:`PathDictionary <ovstage.PathDictionary>` (``dict`` in C).
The C helpers ``waitOp()``, ``check()``, and ``checkOvx()`` wait for operations
and check stage or dictionary errors. The copies remain usable after cleanup:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: setup, read-copy-groups

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: read-copy-groups-c

Fixed-Size Result Shape
-----------------------

A raw fixed-size read group has one scalar-lane tensor with a leading
transported data-row axis. A point3f reads as ``(N, 3)`` and a matrix4d
with MATRIX semantic as ``(N, 4, 4)``; both have ``dtype.lanes == 1``.
``N`` is the transported data-row count and may differ from ``data.count``
(the logical prim count): it can be smaller when ``data.index_map`` shares
rows, or larger when a query touches only part of a transported bucket. A
convenience write shape is not stored; the trailing read dimensions derive
from the stored component count and semantic. Python DLPack preserves them.



These snippets read the three-component ``velocity`` values written by the
same example. They select the correct tensor row for each prim before accessing
its components. For matrices, reshape the Python view to ``(-1, 4, 4)`` or use
16 components per row in C:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: read-fixed-vectors

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: read-fixed-vectors-c

Reading Built-in Metadata
-------------------------

The reserved attributes ``usd-path``, ``usd-schemas``, ``usd-prim-type``,
``usd-ancestor-types``, ``usd-parent``, and ``usd-children`` use 64-bit identifiers. Resolve names and
paths through the :doc:`path_dictionary`. The :doc:`/guides/runtime_loop` reads
``usd-prim-type`` after population.

``usd-path``, ``usd-parent``, and ``usd-children`` are computed from current scene
structure. Latest reads return them with the requested end ordinal on the result,
but that ordinal does not select historical structure. These attributes have
no per-write change records, so change-range reads return no groups for them.
They do not require a watermark advance.

``usd-children`` can span multiple groups; fetch them all. A prim without a
value contributes no row: for example, the root has no parent and a leaf has
no children. Writes, deletes, and maps of these three names or ``usd-active``
fail with ``ErrorCode.NOT_SUPPORTED``. Reads and filters of ``usd-active`` also
fail: live prims are always active. The name is retained in the API for now
and may be removed.


These snippets query the direct children of ``/World/Group``, read ``usd-path``,
and resolve the identifiers to strings. This is the separate Queries example,
whose scene contains ``/World/Group/Left`` and ``/World/Group/Right``. The Python tab includes its imports. The C helper ``resolvePath()`` resolves a path identifier through
the dictionary:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/queries/main.py
         :language: python
         :snippets: setup, query-matched-paths

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/queries/main.cpp
         :language: cpp
         :snippets: query-matched-paths-c

Read Ranges and Errors
----------------------

A snapshot selects every queried path for each requested attribute. An inclusive
change range ``[start, end]`` selects only pairs changed in that interval.
Neither form restores earlier values. The checks below describe the underlying
read failure; asynchronous wait/fetch can instead expose ``OP_FAILED`` with a
diagnostic. See :doc:`/concepts/error_handling` before branching on an error code.

.. note::

   ``usd-active`` is currently **not supported**: a live prim is always
   active, so the attribute carries no information, and reads or filter
   predicates naming it return ``NOT_SUPPORTED``. The name remains in the
   header contract for stability only and is subject to removal in a future
   release.

.. note::

   This build stores only current attribute values. A snapshot
   (``OrdinalRange.latest(end)`` in Python) checks every queried path for each
   requested attribute. A change range ``[start, end]`` checks only queried
   ``(attribute, path)`` pairs changed within that range.

   For each requested attribute, both forms apply these checks in order:

   1. A selected, unsealed change at or before ``end`` returns
      ``OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION``.
   2. A recorded change after ``end`` on a selected path returns
      ``OVSTAGE_ERROR_OUT_OF_RANGE``, whether that later change is sealed or not.

   Changes on unrelated paths do not affect these checks. For example, seal a
   write at 3, then overwrite it at 5. Reading that attribute with end 4 returns
   ``OUT_OF_RANGE``. Sealing 5 does not restore the old value. Use a newer end
   or ``UINT64_MAX`` (``(1 << 64) - 1`` in Python) for current state. An end of
   ``UINT64_MAX`` skips the second check, but a selected unsealed change still
   returns ``WRITE_FLOOR_VIOLATION``. Because a later change after the end makes
   a bounded read return ``OUT_OF_RANGE``, a bounded read is not a replay point. A zero-initialized C range is a snapshot at
   ordinal 0, not a request for current values.

   While the end is above the attribute's write floor, changes between the floor
   and the end can still be admitted and, once sealed, change what the same
   request returns. Once the end is at or below the floor, later writes, deletes
   and maps cannot change the recorded values that a successful read of the same
   request returns for paths the query still selects. This does not cover
   derived built-ins or query membership, which reflect current state, changes
   that ``ovstage_compute_hierarchy`` records at or below the floor, or history
   below the retention frontier.

   A returned group's ordinal is informational. It may be above or below the
   requested end and is not, by itself, proof of which state the payload
   represents.

   These checks use recorded changes. Snapshot reads of values without change
   records, including derived built-ins, return current state. Queries select
   current paths, not the paths present at an earlier ordinal. Under
   ``FLOOR_ONLY`` admission, a write or delete above the applicable floors can
   be admitted at an older ordinal than an existing change to the same attribute
   and path; reads then return the most recently applied change.
   ``STRICTLY_INCREASING`` and the default ``NONDECREASING`` policy reject such older changes
   against retained history.

   Before relying on an old snapshot or interval, query the retention frontier
   with ``ovstage_get_oldest_preserved_ordinal`` (``Stage.get_oldest_preserved_ordinal()``
   in Python). History below that frontier may have been discarded, so a
   successful read does not guarantee the requested earlier state. For example,
   after an attribute's deletion history is discarded, a snapshot from before
   that deletion may return no row for the attribute instead of ``OUT_OF_RANGE``.

In the write-options example, A and B last changed ``heat`` at ordinal 14,
while C last changed at 12. These snippets select the inclusive interval
``[14, 14]``, so only A and B contribute values. The watermark is already 15,
and no later writes have replaced those values:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: read-change-range

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: read-change-range-c

.. _ovstage-read-retention:

Retained Change Records
-----------------------

Before relying on an old interval, call
:meth:`ovstage.Stage.get_oldest_preserved_ordinal()`, wait for the returned
ordinal query, fetch its value, and release the query. Records of which
attributes changed on which prims are exact at or above that value. Older records
may have been combined or discarded; do not assume a fixed retention depth.

This boundary describes :term:`change <Change>` records, not stored versions of
values. A value can be unavailable even when its change record is retained.


These snippets check whether an interval starting at ordinal 14 is within the
retained change records. They release the ordinal query after fetching its
result; the check does not guarantee that the interval's values still exist:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: read-retained-records

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: read-retained-records-c

Further Reading
---------------

- :doc:`queries` — build the query a read runs over.
- :doc:`writing_attributes` — produce the data you read back.
- :doc:`/concepts/dlpack_tensors` — interpreting the returned tensors and their residency.

See :doc:`/scene/device_placement` for the experimental GPU-to-GPU workflow,
extension access, event ordering, and buffer lifetimes.
