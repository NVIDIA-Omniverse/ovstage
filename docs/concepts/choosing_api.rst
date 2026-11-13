.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

.. _ovstage-choosing-api:

Choosing an API
===============

Several ovstage calls reach the same data by different routes. This page compares them by the inputs you have and the result you need. The semantics of each call live in the :doc:`/scene/queries`, :doc:`/scene/writing_attributes`, and :doc:`/scene/reading_attributes` pages; :doc:`/concepts/limitations` lists the behaviors that can change between releases.

Writing Values
--------------

.. list-table::
   :header-rows: 1
   :widths: 18 27 27 28

   * - Call
     - Use when
     - Avoid when
     - Usage notes
   * - ``write_attribute``
     - You already hold the values in an array. You need ``mask``, ``index_map``, or ``count``. The source is a CUDA tensor.
     - You generate every value in place for the whole query and want to skip your own buffer (see ``map_attribute``).
     - Keep input data valid and unchanged until the operation completes.
   * - ``write_attributes``
     - Several attributes change in one step, or you create prims with several attributes at once.
     - You need all-or-nothing results. Validation is all-or-nothing, but execution is not.
     - One operation writes multiple entries. If a later entry fails during execution, earlier entries stay written at the submitted ordinal, and the failed entry may leave data visible to a ``HAS`` query. Repair a partly completed batch at a newer ordinal. Two entries may not write the same prim and attribute in one call.
   * - ``map_attribute`` and unmap
     - You compute values on the CPU for **every** prim in the query and can write them straight into an ovstage-owned CPU buffer.
     - Only some prims change; the values already live in a NumPy, Warp, or Torch array; the source is on the GPU.
     - There is no ``mask``, ``index_map``, or ``count``: unmap commits every row. The buffer is not pre-loaded with existing values, so fill every row or read first and write back. CPU by default; experimental :ref:`ovstage-device-placement` supports CUDA maps for fixed-size attributes. Nothing changes until unmap succeeds, and a failed commit is not rolled back. To map a subset, build the query with ``query_from_path_list``.

In Python, a ``with`` block around a :class:`ovstage.Map` calls unmap on exit, including when an exception is propagating, so a half-filled buffer can be committed. Fill and verify before leaving the block.

Both write forms need an ordinal above the attribute's watermark (write floor in the API) and an :meth:`ovstage.Stage.advance_write_floor()` before readers can see the data. See :doc:`/concepts/overview`.

Addressing Rows and Prims
-------------------------

These options apply to ``write_attribute`` (and each ``write_attributes`` entry). ``index_map`` selects **source rows**; ``mask`` and ``count`` select **target prims**.

.. list-table::
   :header-rows: 1
   :widths: 18 27 27 28

   * - Option
     - Use when
     - Avoid when
     - Usage notes
   * - Dense (no option; one source row per query prim)
     - Every prim in the query gets its own value.
     - Only a few prims change.
     - Supply one source row per target prim. CPU and CUDA source tensors are supported.
   * - ``index_map``
     - Several prims share a row, or rows must be reordered. The payload may hold fewer rows than prims.
     - You want to choose which prims are written; ``index_map`` never selects targets.
     - Python defaults ``count`` to ``len(index_map)``; C needs an explicit ``count``.
   * - ``mask`` (with ``count``)
     - A subset of a query changes and the values are already positioned per prim.
     - You want to save bandwidth: the payload still carries a row for every prim up to ``count``.
     - Reduces the write set, not the payload. In C, supply at least ``(count + 63) / 64`` 64-bit words, using integer division.
   * - ``count``
     - Only the leading ``count`` prims of the query change.
     - The query comes from a filter. Filter-query order is unspecified, so "leading" has no useful meaning there.
     - Meaningful for ``query_from_path_list``, where you control the order.
   * - Smaller query
     - The same subset changes repeatedly.
     - The subset changes shape every frame.
     - Build a ``query_from_path_list`` once over the subset, reuse it, and write densely. This makes the selection explicit.

Reading Values
--------------

The table describes the default ``history_latest`` implementation. Retaining
earlier values requires experimental :ref:`ovstage-selective-history`.

.. list-table::
   :header-rows: 1
   :widths: 18 27 27 28

   * - Read form
     - Use when
     - Avoid when
     - Usage notes
   * - Latest read
     - You need current values for the selected prims.
     - You need only what changed since your last read.
     - Returns the latest committed payload; the end ordinal does not select an older value. Fails with ``ErrorCode.WRITE_FLOOR_VIOLATION`` if a selected change is above the watermark. Results use CPU memory by default; see :ref:`ovstage-device-placement` for CUDA buffers. Do not modify read results, and release the group before overlapping writes.
   * - Ordinal-range read ``[start, end]``
     - A consumer wants to know which prims and attributes changed in an interval, and then wants their current values. Changes are sparse.
     - You need the value **as of** an old ordinal. Reads do not provide arbitrary historical values.
     - Selects changed (prim, attribute) pairs in the inclusive interval, then returns their **latest** payload or a deletion marker (``is_delete``). Fails with ``ErrorCode.OUT_OF_RANGE`` if a selected pair changed again after ``end``. Empty intervals return no groups. Change membership is exact only at or above :meth:`ovstage.Stage.get_oldest_preserved_ordinal()`; the retained depth is bounded and not part of the contract.

Use a latest read for current derived metadata (``usd-path``, ``usd-parent``,
and ``usd-children``). Range reads of ``usd-parent`` and ``usd-children`` return
no groups; ``usd-path`` range reads can return whole-prim deletion tombstones
and require sealing through the range end. See :ref:`ovstage-read-derived-metadata`.
A consumer that must see attribute deletions also needs a range read; a latest
read of a deleted attribute returns nothing for those prims.

Selecting Prims
---------------

.. list-table::
   :header-rows: 1
   :widths: 18 27 27 28

   * - Call
     - Use when
     - Avoid when
     - Usage notes
   * - ``query`` with a filter
     - The set is defined by stage state: prim type, applied schema, subtree, or has-attribute.
     - You already know the paths, or the prims do not exist yet. A filter cannot match absent prims. You need value comparisons (``LT``, ``GT``, and similar); see :doc:`/scene/queries` for the supported filter forms.
     - Membership is a snapshot taken when the query runs. Order is unspecified.
   * - ``query_from_path_list``
     - You hold the path list, you need a stable order, you will upsert prims that do not exist yet, or you want a subset of an earlier query.
     - You want ovstage to discover the prims for you.
     - Synchronous. Keep the path list valid while using the query. Existence is not checked. Only paths from this instance's :doc:`path dictionary </scene/path_dictionary>` are accepted.

Release queries you no longer use.

Getting Data In
---------------

.. list-table::
   :header-rows: 1
   :widths: 18 27 27 28

   * - Approach
     - Use when
     - Avoid when
     - Usage notes
   * - Author directly (``write_attribute``, ``write_attributes``, ``map_attribute``)
     - The application or simulation already owns the data, or it creates prims that have no USD source.
     - You want USD composition, references, and time samples resolved for you.
     - You control ordinals and the layout of every column.
   * - Populate from USD (``open_usd*``)
     - You start from a USD scene and a consumer needs its prims and values. Population selects composed values at one time; it does not copy layers.
     - You need history or multiple USD times in memory at once.
     - Wait for population with its population-specific wait function. Population does not advance the watermark; do that yourself after the population operation completes. Provisional API; see :doc:`/scene/population`.
   * - Add or remove a USD reference
     - You grow or shrink the USD source after the first load.
     - You have not opened a scene yet.
     - Edits the USD source only; call ``apply_usd_changes`` to push the edits into the stage.
   * - ``apply_usd_changes``
     - USD edits happened after population (authoring, references).
     - You only advanced time.
     - Do not share its ordinal with ``apply_usd_time`` unless the attributes are disjoint.
   * - ``apply_usd_time`` (Python: ``update_from_usd_time``)
     - Time-sampled USD values must be re-sampled at a new time.
     - The scene has few time-varying values and you drive them yourself.
     - Rewrites every in-scope time-varying property on each call, even when the time is unchanged.

Values written by either route are ordinary stage data. Runtime writes do not edit USD layers, and a type conflict between populated and application-written data for the same attribute fails the write. Use :doc:`/scene/exporting_to_usd` for the reverse direction.

Tracking Changes with Several Consumers
---------------------------------------

Each consumer keeps its own cursor (the last ordinal it has processed) and asks for what changed since.

1. Complete and check every producer write for the update, repairing any failure before continuing. Advance the watermark and check its completion (producers, or the application on their behalf). See :ref:`ovstage-write-recovery`.
2. Read the current watermark with :meth:`ovstage.Stage.get_attribute_write_floor()`. Passing ``None`` returns the global value, the **global watermark** (``global write floor`` in the API).
3. Read the change range ``(last, watermark]``, which is ``OrdinalRange.between(last + 1, watermark)`` in Python, and consume the groups.
4. Release the groups, then store ``watermark`` as the new cursor.

Before step 3, compare ``last + 1`` with :meth:`ovstage.Stage.get_oldest_preserved_ordinal()`. If the cursor is older, the consumer has fallen behind: change membership for that interval may have been combined or discarded. Do a full latest read to resynchronize, then resume range reads.

If a range read fails with ``ErrorCode.OUT_OF_RANGE``, a selected pair changed again after your end ordinal. Retry with a later end ordinal that the watermark has reached, or resynchronize with a latest read.

See :ref:`ovstage-why-range-reads` for the cursor pattern and a tested example in C and Python.

Two points apply to every consumer:

* While you hold a read group, writes, maps, and deletes that overlap its (attribute, prim) set fail with ``ErrorCode.OP_FAILED``, and so does any operation, including a clone, that would give ``omni:fabric:worldMatrix`` to a prim the group covers: a hierarchy parent or ancestor of the operation's prims, or a prim with a transform input between one of them and a descendant that carries it (see :doc:`/scene/transforms`). A slow consumer therefore holds up producers. Copy what you need and release promptly.
* A read that overlaps an unfinished write or open map on the same data also fails. Order the steps: write, wait, advance the watermark, wait, then read.

The watermark does not retain a step for a consumer that falls behind; see :doc:`/concepts/overview`.

C and Python
------------

Both languages expose the same operations. Python wraps the C API; it adds convenience but no separate capabilities. Differences to expect:

.. list-table::
   :header-rows: 1
   :widths: 24 38 38

   * - Topic
     - C
     - Python
   * - Operation results
     - Each enqueue returns a status and an ``op_index``; you call ``ovstage_wait_op`` and ``ovstage_release_op``.
     - :class:`ovstage.Operation` has ``op_id`` and ``wait()``. A rejected enqueue surfaces as an exception at ``wait()``. A failed operation reports ``ErrorCode.OP_FAILED``.
   * - Handles
     - Release query, read, group, map, and ordinal-query handles explicitly.
     - ``with`` blocks release handles; ``Read.groups()``, ``Map.groups()``, and ``Query.result()`` add iteration and fetch helpers.
   * - Population
     - Enqueue returns a population operation id; wait with ``ovstage_population_wait_op``.
     - :mod:`ovstage.population` has a blocking form and an ``_async`` form of each call. Some names differ from C; see :doc:`/scene/population`.
   * - Write inputs
     - Tensors or managed tensors (``managed_tensors``).
     - NumPy arrays, DLPack producers, or :class:`ovstage.DLTensor`. Managed tensors are not exposed.
   * - Ranges
     - ``ovstage_ordinal_range_t`` with ``has_start_ordinal``.
     - ``OrdinalRange.latest(n)`` and ``OrdinalRange.between(a, b)``.
   * - ``count`` defaults
     - ``0`` means the full query; ``index_map`` or ``mask`` needs an explicit count.
     - Defaults to the full query, or to ``len(index_map)``; ``mask`` needs an explicit count; ``0`` is rejected.
   * - Errors
     - Status codes and ``ovstage_get_last_error``.
     - :class:`ovstage.OvstageError` with an ``ErrorCode``.

Data layout, ordinals, watermarks, and the limits in :doc:`/concepts/limitations` are identical. For blocking rules and handle lifetime across threads, see :doc:`/concepts/threading_lifetime`.
