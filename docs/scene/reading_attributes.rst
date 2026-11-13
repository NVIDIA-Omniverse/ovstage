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

Reading values
--------------

By default, reads return CPU buffers. The experimental ``placement=`` argument
can select a CUDA copy instead. A placed read with an availability event requires
device synchronization after fetching a group and before using its payload;
see :ref:`ovstage-device-placement`. NumPy ``column.array()`` is CPU-only.

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

.. _ovstage-read-groups-columns:

Read Groups and Columns
-----------------------

A read group contains a shared set of logical prim rows and one or more
attribute columns. In C, these are ``ovstage_read_group_t`` and
``ovstage_read_column_t``; in Python, iterate ``ReadGroup.columns`` to get
``ReadColumn`` objects. Each column carries its attribute, ordinal, deletion
state, array kind, semantic, payload, and metadata. Prim accessors and the
release identity belong to the group.

If you are updating a 0.3 consumer, see :ref:`ovstage-migrating-read-consumers`
for the renamed members and changed C fetch/release calls. For every read loop:

* Iterate every fetched group and every column it contains, even when
  requesting only one attribute. Columns follow the
  requested attribute order after exact-token deduplication; omitted attributes
  mean a column's position need not equal its position in the request. Use
  ``column.attribute`` to identify it. Groups have no ordering, disjointness,
  or stable-partition guarantee.
* Resolve logical row ``i`` to a prim with the group's mapping: in C,
  ``group.prims.index_map[i]`` when present, otherwise
  ``group.prims.offset + i``; in Python, ``group.prim_index(i)``. These indices
  address ``group.prims.list`` / ``group.prim_list``, not the entire query by
  assumption. Process only ``group.prims.count`` / ``group.prim_count`` rows.
* Resolve the value for that same logical row independently for each column:
  in C, ``column.data.index_map[i]`` when present, otherwise ``i``; in Python,
  ``column.data_row_index(i)`` when ``has_data_index_map`` is true, otherwise
  ``i``. The value index selects a stacked fixed-size tensor row or a per-row
  array tensor. Never use a prim-list index as a value index or reuse another
  column's mapping.
* Check each column's deletion state and presence before accessing payloads.
  An ``is_delete`` column is a tombstone with no tensors. An absent attribute
  is omitted; partial presence uses a column data mask or a separate group.
  If ``column.data.mask`` is present, only set bits identify present logical
  rows (Python exposes this mask through ``column.raw.data.mask``). A present
  empty array, empty string, or zero is still a value. Do not manufacture
  defaults for missing columns or masked-out rows.
* Keep the containing group alive while using columns, tensors, mappings,
  NumPy arrays, or DLPack views. Release every group after processing it, and
  copy anything that must outlive the release. Releasing the read handle does
  not release fetched groups. Python's group context manager performs
  this release; it does not extend borrowed views beyond block exit.

Groups and Lifetime
-------------------

1. Keep the read handle in a ``with`` block and call :meth:`read.wait() <ovstage.Read.wait>`.
2. Fetch all results with :meth:`read.groups() <ovstage.Read.groups>`, or call :meth:`read.fetch_next() <ovstage.Read.fetch_next>` until
   it returns ``None``. Do not assume one group contains the whole result.
3. Release each group with its own ``with`` block or
   :meth:`ovstage.Stage.release_group()`.
4. Release the read handle after fetching its results. Context-manager exit
   does this for you.

- ``ovstage_read_attributes`` enqueues the read and returns a read handle.
- ``ovstage_fetch_read_next`` fills an output array of ``ovstage_read_group_t`` — each
  returned group carries one shared prim group and one or more attribute columns — until
  ``OVSTAGE_ERROR_END_OF_ITERATION``.
- Columns may have different masks, index maps, tensor buffers, devices,
  synchronization, metadata, ordinals, or deletion state. OVStage combines
  columns only when backend fragments already have exactly the same prim
  layout; it does not intersect, reorder, repartition, or copy data to align them.
  Single-column groups remain valid. See :ref:`ovstage-read-groups-columns`
  for interpreting each column and its row mapping.
- Release groups with ``ovstage_release_groups``, then the read with
  ``ovstage_release_read``. All columns and their borrowed views have the same
  lifetime as their containing group. A group's pinned storage is an independent
  resource:
  releasing the read does **not** reclaim it, and an unreleased group keeps its
  outstanding-read coverage, so later writes to the same canonical attribute and
  prims fail with an "overlapping outstanding read" error reported at the write
  site. A public alias of that attribute shares the same coverage.
- In Python, ``ReadGroup`` is a context manager that releases on block exit,
  including when the body raises. ``fetch_next()`` returns ``None`` at end of
  iteration, so bind it before entering the block (``group = read.fetch_next()``
  → ``if group is not None:`` → ``with group:``), or iterate ``read.groups()``,
  which yields only real groups. Dropping a group without releasing it triggers a
  finalizer that releases it and emits a ``ResourceWarning``; treat that as a bug
  report rather than a strategy.
- Python exposes columns through ``ReadGroup.columns``. Each ``ReadColumn``
  exposes payloads through ``array()``, ``tensor()``, and ``dlpack()``. The semantic is
  an :class:`~ovstage.AttributeSemantic` for values known to the installed
  bindings, or the raw integer for a newer native value so it can be preserved
  without reaching into the private ctypes structure.
- ``wait()`` returns the read handle, so ``group = read.wait().fetch_next()``
  chains inside the read's ``with`` block. Do not chain on the temporary
  (``stage.read_attributes(...).wait().fetch_next()``): the ``Read`` is dropped
  unreleased and falls to the finalizer, and its fetched group is stranded with
  it. ``read.wait()`` is not redundant with ``fetch_next()``'s own wait — it is
  what releases the read's reserving op and surfaces a producer failure with its
  typed error.
- A ``ReadColumn`` borrows its group through a weak reference. Keep the group
  alive while accessing the column: ``read.fetch_next().columns[0].array(0)``
  can raise ``OvstageError`` because the temporary group has already been
  finalized. The same applies to a chained ``.dlpack(0)`` call.
- ``column.array(i)`` **borrows** the group's storage. A NumPy view already
  created from a live group does not keep that group alive and becomes invalid
  when it is released. For example, after ``group = read.fetch_next()`` and
  ``view = group.columns[0].array(0)``, calling ``stage.release_group(group)``
  invalidates ``view``. Copy out (``np.array(view)``) before release if the data
  must outlive the group. Later column access raises, but an existing view
  cannot be guarded.
- A **DLPack** producer returned by ``column.dlpack(i)`` keeps the group alive
  while the producer exists. A consumer view does not: consuming the capsule
  destroys it, and the native deleter cannot retain the Python owner for the
  consumer's lifetime. Keep the group or producer alive until the consumer is
  done, and do not explicitly release the group while its views are in use.

Batched Fetch and Release in C
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:c:func:`ovstage_fetch_read_next()` accepts a caller-owned output array, its
positive capacity, and a required output count. ``OVSTAGE_OK`` returns between
one and capacity complete groups. Only the returned slots are valid. Timeout
applies to the first group; fetch does not wait to fill the batch. A short batch
is not completion: keep fetching until ``OVSTAGE_ERROR_END_OF_ITERATION``.
Timeout, completion, and other errors return a zero count. If an error occurs
after some groups have been delivered, that call returns them with
``OVSTAGE_OK`` and the next fetch reports the error. A failure while delivering
groups is terminal for the read, including ``OVSTAGE_ERROR_OUT_OF_MEMORY``
before any group is delivered: every later fetch returns the same error,
never ``OVSTAGE_ERROR_END_OF_ITERATION``. Release the read handle to reclaim
undelivered groups. Timeouts and argument validation errors do not latch a
group-delivery failure.

Each acquisition remains independently valid after another fetch or release of
the read handle. Copying a descriptor does not acquire another reference.
:c:func:`ovstage_release_groups()` accepts groups from different reads or fetches
on the same instance, in any order. Count zero succeeds even with a null array.
Invalid, duplicate, or previously released IDs reject the entire batch without
releasing anything. Finish all uses of borrowed views, including asynchronous
GPU work, before releasing a group. The instance must outlive its groups.
Concurrent fetches on one read handle are safe and return disjoint groups.
A short batch or deferred error may be observed on another thread; serialize
fetches when one consumer must observe every group and the error. Independent
groups may be released from worker threads, without concurrently releasing the
same acquisition.

This test reads four scalar attributes on two prims. It sums the values before
releasing each batch and checks the completed result:

.. filtered-literalinclude:: ../../tests/c/test_attributes.cpp
   :language: cpp
   :snippets: batched-read-groups-c

Python's :meth:`read.fetch_next() <ovstage.Read.fetch_next>` and
:meth:`ovstage.Stage.release_group()` remain single-group convenience methods.

Copying Results Before Release
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

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

A raw fixed-size read column has one scalar-lane tensor with a leading
transported data-row axis. A point3f reads as ``(N, 3)`` and a matrix4d
with MATRIX semantic as ``(N, 4, 4)``; both have ``dtype.lanes == 1``.
``N`` is the transported data-row count and may differ from ``data.count``
(the logical prim count): it can be smaller when ``data.index_map`` shares
rows, or larger when a query touches only part of a transported bucket. A
convenience write shape is not stored; the trailing read dimensions derive
from the stored component count and semantic. Python DLPack preserves them.

When ``column.has_data_index_map`` is true, use
:meth:`column.data_row_index(i) <ovstage.ReadColumn.data_row_index>` to find the
row for each selected entry. See :doc:`/concepts/dlpack_tensors`.


These snippets read the three-component ``velocity`` values written by the
same example. They select the correct tensor row for each prim before accessing
its components. Matrix views already preserve the trailing ``(4, 4)`` dimensions; C consumers
can access the 16 components of each row:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: read-fixed-vectors

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: read-fixed-vectors-c

.. _ovstage-read-derived-metadata:

Reading Built-in Metadata
-------------------------

The reserved attributes ``usd-path``, ``usd-schemas``, ``usd-prim-type``,
``usd-ancestor-types``, ``usd-parent``, and ``usd-children`` use 64-bit identifiers. Resolve names and
paths through the :doc:`path_dictionary`. The :doc:`/guides/runtime_loop` reads
``usd-prim-type`` after population.

``usd-path``, ``usd-parent``, and ``usd-children`` are computed from current scene
structure. Latest reads return them with the requested end ordinal on the result,
but that ordinal does not select historical structure. ``usd-parent``
and ``usd-children`` have no per-write change records, so change-range reads
return no groups for them. Latest reads of the three names do not require a
watermark advance.

A change-range read of ``usd-path`` returns one group with a tombstone column (``is_delete``)
for the query prims that were deleted whole in the range. Only the latest
creation or delete of each prim in the range counts, so a prim that is deleted
and then created again gives no tombstone. A prim created in the range gives no
``usd-path`` group; its other attributes show the change. Exception: this read
needs a watermark. If the watermark of ``usd-path`` is less than the range end,
the read fails with ``ErrorCode.WRITE_FLOOR_VIOLATION``, also when no prim
changed above it. The read cannot see a prim creation or delete that has not
run yet, so only the watermark makes its result final. ``Scope.ALL``, or
``Scope.INCLUDE`` with ``usd-path`` in its list, advances this watermark.

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

.. _ovstage-why-range-reads:

When to Use a Change-Range Read
-------------------------------

A latest read answers "what are the values now?". A change-range read answers "what changed since I last looked?". Use a range read when a consumer keeps its own copy of the scene and only needs to update what moved:

* **Renderers and sensors.** Each frame, update only the prims whose transforms, materials, or other attributes changed since the previous frame, instead of re-reading the whole scene.
* **Consumers at a different rate.** If physics advances the watermark four times per rendered frame, a consumer that runs once per frame reads the whole interval and gets every prim that changed in any of those steps, with its latest value.
* **Replication and export.** Send only changed prims, and deletions, to another process, a network client, or a USD layer.

ovstage keeps no per-consumer state, so each consumer keeps a **cursor**: the last ordinal it has consumed.

1. Start with a latest read of everything the consumer needs. Set the cursor to the watermark that read covered.
2. On each update, read the range from ``cursor + 1`` to the current watermark. Apply the returned values, and treat groups marked ``is_delete`` as deletions.
3. Move the cursor to that watermark.
4. If ``cursor + 1`` is older than :meth:`ovstage.Stage.get_oldest_preserved_ordinal()`, the consumer has fallen behind the retained change records. Go back to step 1. See `Retained Change Records`_.

Coordinate with producers so the bounded read completes before later writes
replace its selected values. Otherwise, handle ``OUT_OF_RANGE`` by requesting
a newer end or current state; see `Read Ranges and Errors`_.

This catch-up, from the producer-consumer example, reads the range ``[last_seen + 1, floor]`` and returns the floor as the consumer's new cursor:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/producer-consumer/main.py
         :language: python
         :start-after: # [snippet:consume-delta]
         :end-before: # [/snippet:consume-delta]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/producer-consumer/main.cpp
         :language: cpp
         :start-after: // [snippet:consume-delta]
         :end-before: // [/snippet:consume-delta]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Choose a latest read instead when the consumer has no copy to update, or when almost everything changes every step. A range read then returns the same data with extra bookkeeping. See also :doc:`/concepts/choosing_api`.

Read Ranges and Errors
----------------------

The checks below describe the underlying read failure; asynchronous wait/fetch
can instead expose ``OP_FAILED`` with a diagnostic. See
:doc:`/concepts/error_handling` before branching on an error code.

The default ``history_latest`` implementation stores only current attribute
values. A snapshot
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
or ``OVSTAGE_ORDINAL_MAX`` (``ovstage.ORDINAL_MAX`` in Python) for current state. An end of
``OVSTAGE_ORDINAL_MAX`` skips the second check, but a selected unsealed change still
returns ``WRITE_FLOOR_VIOLATION``. Because a later change after the end makes
a bounded read return ``OUT_OF_RANGE``, a bounded read is not a replay point. A zero-initialized C range is a snapshot at
ordinal 0, not a request for current values.

These rules describe an instance of ``history_latest``, the default, and every
unselected pair of an instance of ``history_selective``. A selected pair
retains its earlier sealed values; see `Earlier Values of Selected Pairs`_.

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

.. _ovstage-selective-history:

Earlier Values of Selected Pairs
--------------------------------

An instance of ``history_latest``, the default, keeps history off. It stores
the latest committed value of every cell, and the checks above apply to every
read. An instance of ``history_selective`` turns history on for the
``(prim, attribute)`` pairs that the application selects with
:c:func:`ovstage_enable_history` (Python :meth:`ovstage.Stage.enable_history`).
A selected pair retains its earlier sealed values, up to the process setting
``OVSTAGE_CONFIG_RETENTION_WINDOW`` (default 4). It answers a snapshot at a
finite end with the value sealed at or before that end, and it answers a
change range from the retained history. Every other pair keeps the rules
above. The instance does not serve the experimental device placement
extension.

.. warning::

   History support is **experimental**. The ``history_selective`` implementation,
   the ``ovstage.history.v1`` selection extension of ``ovstage_history.h`` and
   the ``OVSTAGE_CONFIG_RETENTION_WINDOW`` setting can change between releases
   without a deprecation period. The library does not guarantee source
   compatibility, ABI or semantics. Isolate their use behind an application
   adapter. Check availability at run time.

   The ``history_selective`` implementation is available as a preview of
   the planned direction for history support, intended for early evaluation
   and feedback. It may be less stable at runtime and offer lower performance
   than the default ``history_latest`` implementation. Validate stability
   and performance with your application's workloads before adopting it.

The sequence of a selective history is:

1. Create the instance with the implementation name ``history_selective``
   (``ovstage_instance_desc_t.implementation``, Python
   ``StageConfig(implementation="history_selective")``).
2. Write and seal the first values. A selection copies the sealed values of
   its pairs as the retained baseline, so seal before you select.
3. Select the pairs with ``ovstage_enable_history()`` on a quiescent instance:
   no pending stage or renderer work, and no live reads, groups or maps. A
   completed query gives the prims, and the attribute list gives the
   attributes. Later matches of the same filter are not selected.
4. Write and seal later ordinals. Every write of a selected pair is recorded
   at its own ordinal, and a floor advance waits for those records.
5. Read a selected pair at an earlier ordinal with a snapshot at that end or
   with a change range. A latest read is unchanged.
6. Stop the retention with ``ovstage_disable_history()``. The call discards
   the retained history of the pairs and keeps their live values.

Two failures need a retry. If the baseline copy of a selection fails, every
mutation and publication of the instance is refused until the identical
selection is retried with success, or until the instance is destroyed. A
failed copy activates no partial selection. If a floor advance reports that a
record did not land, the history floor stays behind. A historical read of a
selected pair is then refused with ``WRITE_FLOOR_VIOLATION`` until a floor
advance succeeds. Retry the floor advance with the whole-stage scope.
``ovstage_history.h`` states both contracts in full.

These snippets run the sequence against the produced package and wheel:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_history.py
         :language: python
         :snippets: history-enable-read

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_history.cpp
         :language: cpp
         :snippets: history-enable-read-c

.. _ovstage-read-patterns:

Selecting Attributes with Patterns
----------------------------------

An entry of the attribute list can also be a selector pattern:

- ``*`` selects every attribute that the read can return.
- A prefix followed by ``*``, such as ``primvars:*``, selects every attribute
  whose name starts with the prefix.

A pattern has exactly one ``*``, as its last character. The match is case
sensitive and uses the full name. Other forms that contain ``*``, such as
``*mass`` or ``a*b``, fail with ``ErrorCode.INVALID_ARGUMENT``. Each read finds
the matching names again, so a read includes attributes that were written after
the query was made.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_read_patterns.py
         :language: python
         :start-after: # [snippet:read-attribute-patterns]
         :end-before: # [/snippet:read-attribute-patterns]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_read_patterns.cpp
         :language: cpp
         :start-after: // [snippet:read-attribute-patterns-c]
         :end-before: // [/snippet:read-attribute-patterns-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Rules for the result:

- Explicit attribute names enter the selection first, in caller order after
  exact-token deduplication. Repeated explicit names do not add columns again.
- Then come the names that the patterns add, sorted by name. A pattern does not
  add a name that the selection already has, so overlapping patterns give each
  name one time.
- Names that start with ``_`` or ``usd-`` are hidden, except ``usd-path``. A
  pattern selects a hidden name only when the prefix of the pattern starts with
  the same namespace. Thus ``*``, ``u*``, and ``usd*`` do not select
  ``usd-schemas``, but ``usd-*`` and ``usd-s*`` do. ``*`` does not select
  ``_name``, but ``_*`` does. The rule applies to the start of the name only.
  A hidden name in the list of attribute names is read as usual.
- A pattern that matches ``usd-path`` selects it. In a latest read it gives
  the prims. In a change-range read it gives the tombstones of prims deleted
  whole in the range.
- A latest read with ``usd-*`` also selects ``usd-parent``, ``usd-children``,
  ``usd-prim-type``, ``usd-schemas``, and ``usd-ancestor-types``. Each gives
  groups only for the prims that have a value, and ``usd-children`` groups are
  ragged arrays.
- A change-range read with ``usd-*`` selects ``usd-prim-type``,
  ``usd-schemas``, and ``usd-ancestor-types`` when they changed in the range,
  as any recorded attribute. It never selects ``usd-parent`` or
  ``usd-children``. Patterns never match ``usd-active``.
- A pattern that matches no name gives zero groups. An empty attribute list, or
  a query with no prims, also gives zero groups.

Which names a pattern finds:

- A latest read finds the names that the query prims have now. It also finds a
  name that was deleted above its watermark. That delete is not sealed, so the
  read fails with ``WRITE_FLOOR_VIOLATION`` until the watermark of the name
  reaches the delete.
- A latest read with an end below ``OVSTAGE_ORDINAL_MAX`` also finds a name that changed,
  or was deleted, after the end. The read then fails with ``OUT_OF_RANGE``, as
  for a name in the list. Thus the read does not leave out a name that the
  prims had at the end.
- A change-range read finds the names that changed, or were deleted, on the
  query prims in the range.

Each name that a pattern finds gets the same checks as a name in the list. A
change-range read with a pattern also has two more checks:

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Check
     - Error
     - Why, and what to do
   * - The stage-wide watermark is at or above the range end.
     - ``WRITE_FLOOR_VIOLATION``
     - A pattern can match a name that no prim has yet. That name has no
       watermark of its own, so only the stage-wide watermark seals it. Thus
       the read fails while the stage-wide watermark is below the range end,
       also when every known name has a higher watermark. Before the read,
       advance the watermark to the range end with ``Scope.ALL``.
   * - The range start is at or above
       :meth:`ovstage.Stage.get_oldest_preserved_ordinal()`.
     - ``OUT_OF_RANGE``
     - Records of deleted attributes below that value can be gone, so the
       pattern could miss a deleted name. Read from a later start.

.. warning::

   Do not use :meth:`ovstage.Stage.get_attribute_write_floor()` with no
   attribute to check the stage-wide watermark. After a ``Scope.INCLUDE`` or
   ``Scope.EXCLUDE`` advance, it can report a higher value than the stage-wide
   watermark.

A latest read with a pattern does not check the stage-wide watermark. The two
checks apply also when the query has no prims. When the list has names and
patterns, the checks apply to the whole read.

An outstanding write or delete on the query prims to a matching name fails the
read with ``ErrorCode.OP_FAILED``. This is the same as for an attribute name.
The rule includes a write that adds a new name.

.. note::

   Because ``*`` is a selector, an attribute name that contains ``*`` is
   reserved for read selection. Writes, maps, deletes, query filters and
   attribute lists, :meth:`ovstage.Stage.get_attribute_write_floor()`, and the
   attribute lists of ``Scope.INCLUDE`` and ``Scope.EXCLUDE`` watermark advances
   reject such a name with ``ErrorCode.INVALID_ARGUMENT``. ``Scope.ALL`` does not
   use its attribute list, so it does not check it.

For the current limits of patterns, see :doc:`/concepts/limitations`.

Further Reading
---------------

- :doc:`queries` — build the query a read runs over.
- :doc:`writing_attributes` — produce the data you read back.
- :doc:`/concepts/dlpack_tensors` — interpreting the returned tensors and their residency.

See :doc:`/scene/device_placement` for the experimental GPU-to-GPU workflow,
extension access, event ordering, and buffer lifetimes.
