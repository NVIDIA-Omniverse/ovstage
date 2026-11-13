.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Limitations and Current Behavior
==========================================

This page is the single place that lists what ovstage does not do and where its current behavior is narrower than the API shape suggests. Other pages link here instead of repeating these limits.

These limits apply to the release documented here. Follow the public API contract and check the release notes when upgrading.

For blocking behavior, thread safety, and object lifetime, see :doc:`/concepts/threading_lifetime`. For error codes, see :doc:`/concepts/error_handling`.

Memory and Devices
------------------------------------

ovstage represents data in arrays and uses DLPack tensors for CPU/GPU interchange. CUDA tensors are accepted as write sources. Reads and maps use CPU memory by default; the experimental :ref:`ovstage-device-placement` extension also supports CUDA buffers.

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - Reads and maps use CPU memory unless a device is selected explicitly.
     - Select :ref:`ovstage-device-placement` for CUDA consumers and inspect the returned tensor device.
   * - CUDA maps support fixed-size attributes only; array-valued CUDA maps and maps of the reserved metadata names return ``NOT_SUPPORTED``.
     - Use CPU maps for array-valued attributes and for the reserved metadata.
   * - CUDA source data must be ready before the write uses it. Synchronization can delay completion.
     - Complete the producer work or supply the supported ``cuda_stream`` or ``cuda_event`` synchronization. Keep the source buffer valid until the operation completes, or through device completion when a placement availability event is supplied.
   * - Pinned host memory (``kDLCUDAHost``) is rejected with ``NOT_SUPPORTED``. Accepted devices are CPU, CUDA, and CUDA managed.
     - Pass a CPU tensor, or a CUDA or CUDA-managed tensor.
   * - ``cuda_stream`` or ``cuda_event`` with a CPU tensor returns ``NOT_SUPPORTED``. CUDA map commits require the source handoff described by :ref:`ovstage-placement-synchronization`.
     - Pass synchronization values only for the corresponding CUDA source and observe placement completion rules.

See :doc:`/concepts/dlpack_tensors` for tensor shapes and the write-source rules.

History and Change Tracking
--------------------------------------------

An instance of ``history_latest``, the default, stores the latest committed value of each attribute. Change tracking records which prims and attributes changed and at which ordinal. It does not record earlier values. The watermark (write floor in the API) is the highest ordinal sealed for an attribute. An instance of ``history_selective`` retains the earlier sealed values of the pairs selected with :c:func:`ovstage_enable_history` (Python :meth:`ovstage.Stage.enable_history`), up to the retention window. Every other pair keeps the limitations below. See :ref:`ovstage-selective-history`.

.. warning::

   History support is **experimental**. The ``history_selective`` implementation,
   the ``ovstage.history.v1`` selection extension of ``ovstage_history.h``, the
   ``OVSTAGE_CONFIG_RETENTION_WINDOW`` setting and the renderer history
   presentation can change between releases without a deprecation period. The
   library does not guarantee source compatibility, ABI or semantics. Isolate
   their use behind an application adapter. Check availability at run time.

   The ``history_selective`` implementation is available as a preview of
   the planned direction for history support, intended for early evaluation
   and feedback. It may be less stable at runtime and offer lower performance
   than the default ``history_latest`` implementation. Validate stability
   and performance with your application's workloads before adopting it.

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - On ``history_latest`` a read always returns the latest value. A range read tells you that a prim changed in the range, not what the value was then.
     - Keep your own copy if you need earlier values, or select the pairs on an instance of ``history_selective`` (experimental).
   * - ``history_selective`` does not support the experimental :ref:`ovstage-device-placement` extension, including explicit CPU placement as well as CUDA placement.
     - Use the regular read, write, and map APIs on ``history_selective``, rather than the device-placement APIs. Use ``history_latest`` when explicit device placement is needed.
   * - Exact change history is available only within a bounded window. Query :meth:`ovstage.Stage.get_oldest_preserved_ordinal()` for the current boundary.
     - Read each range soon after it is sealed, and check the reported boundary before reading.
   * - A consumer that falls behind the reported history boundary can miss deletions and exact change membership.
     - Resynchronize with a latest read after falling behind.
   * - A read behind the history boundary may succeed without complete change information.
     - Call :c:func:`ovstage_get_oldest_preserved_ordinal()` (Python :meth:`~ovstage.Stage.get_oldest_preserved_ordinal`) and compare it with your range start. The value is a conservative stage-wide number.
   * - A range read returns the latest payload for each selected prim. If a selected prim and attribute changed after the range end, the read fails with ``OUT_OF_RANGE``.
     - Read ranges in order and stop at the watermark.
   * - On ``history_latest`` a snapshot (``OrdinalRange.latest(end)``) checks every selected path against its end; it does not restore older values. A selected pair of ``history_selective`` (experimental) returns the value sealed at or before the end.
     - Use ``OVSTAGE_ORDINAL_MAX`` for current values. A recorded later change can make a bounded snapshot fail with ``OUT_OF_RANGE``.

See :doc:`/scene/reading_attributes` for range reads.

Read Selector Patterns
--------------------------------------------

A read can select attributes with ``*`` or ``prefix*`` (for example ``primvars:*``). See :ref:`ovstage-read-patterns` for the rules.

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - A range read with a pattern fails with ``WRITE_FLOOR_VIOLATION`` while the stage-wide watermark is below the range end. This is true also when every attribute it reads has a higher watermark. A pattern can match a name that no prim has yet, and only the stage-wide watermark seals such a name.
     - Advance the watermark to the range end with ``Scope.ALL`` before the read. ``Scope.INCLUDE``, and ``Scope.EXCLUDE`` with names in its list, do not advance it.
   * - ``*`` can return internal columns. It skips hidden names (``_`` and ``usd-`` except ``usd-path``), but returns all other names that a query result reports.
     - Use a narrower prefix, or ignore names that you do not use.
   * - A range read with a pattern does not find attributes of the ``RENDERING`` population domain. The read finds names from change history, and these attributes have none. A range read of the attribute name also gives no groups for them.
     - Use a latest read. To find the changes, compare the values with the values of an earlier read.
   * - A range read gives no ``usd-path`` tombstone for a prim that the ``RENDERING`` population domain removes.
     - Compare the prims of a new query with the prims that you have.
   * - A pattern sees the two names of an aliased column as different names. After a write through one name, a pattern that matches only the other name does not find the change. If both names changed in the range, a pattern that matches both gives the same rows under each name.
     - Use a pattern that matches the name that the writer uses, or key the groups by attribute and drop the repeated rows.

Ordinals
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - The initial sealed boundary ``OVSTAGE_ORDINAL_MIN`` cannot be used for writes, deletes, or map publication.
     - Choose an ordinal strictly above the applicable write floor and satisfy the configured admission policy. On a new stage, negative ordinals above the minimum and zero are valid; see :ref:`ordinal-bounds-and-initial-state`.
   * - The default ``NONDECREASING`` policy permits equal unsealed ordinals but rejects older retained changes on the same cells. ``STRICTLY_INCREASING`` also rejects equality; ``FLOOR_ONLY`` skips retained-history conflicts.
     - Coordinate writes to shared cells and satisfy the configured :ref:`ordinal-admission-policies`; distinct ordinals alone do not order execution.

See :doc:`/concepts/async_model` for how writes and the watermark interact.

Queries
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - Filters use a closed allow-list of attributes and operators. Value comparisons are not supported, and an unsupported combination returns ``NOT_SUPPORTED`` at submission.
     - See :doc:`/scene/queries` for the allow-list. Read the values and filter in your application.
   * - Query membership is a snapshot taken when the query executes. Prims created or deleted afterward do not join or leave the handle.
     - Run a new query to see structural changes.
   * - Queries remain live until released.
     - Release queries you no longer need.
   * - The order of matched prims is unspecified.
     - Do not depend on order.

Execution
----------------------------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - Operations at the same ordinal execute in submission order. The API does not guarantee ordering across different ordinals.
     - Do not rely on cross-ordinal ordering or on concurrency.
   * - An instance of ``history_latest`` does not return ``OVSTAGE_ERROR_QUEUE_FULL`` in this release. An instance of ``history_selective`` (experimental) returns it for a mutation submitted while a floor advance runs its reconciliation, and for a stage operation submitted while a history presentation is active.
     - Bound your own submission rate, for example by waiting each tick. On ``history_selective``, retry the refused call after the floor advance or the presenting step returns. See :ref:`ovstage-selective-history`.
   * - Operation ids are never reused within a stage. An id returned by a successful submission keeps tracking state until you release it, even when the operation completed immediately.
     - Release every operation id after you read its result.
   * - Do not use stage destruction to wait for successful operation completion. See :doc:`/concepts/threading_lifetime`.
     - Wait for the operations you need before destroying the stage.

Delete
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - Deleted values are not available through a historical read. Range reads can report deletion markers while the relevant change history is available.
     - Read the range before the markers age out, or keep your own copy.
   * - Deleting a prim does not delete its descendants.
     - Query the subtree and delete it explicitly.

Hierarchy
------------------------------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - In this release, :c:func:`ovstage_compute_hierarchy()` does not provide historical transform computation or ordinal-tagged output.
     - Supply the ordinals required by the public call sequence. Do not rely on historical input selection or ordinal tagging of the result.
   * - An explicit compute updates world transforms. It does not update visibility.
     - Do not use it to refresh visibility.
   * - Automatic transform updates depend on the configured hierarchy computation model.
     - Call the explicit compute when you need a defined point. See :doc:`/scene/transforms`.

Population and Export
--------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - ``reset``, ``apply_usd_time``, and ``apply_usd_changes`` on a stage that has not been populated return success with operation id 0 and do nothing. Waiting on that id then fails.
     - Check the id before waiting, or populate first.
   * - The ordinal in an export descriptor must equal the global watermark exactly. A stale or future ordinal fails with ``INVALID_ARGUMENT``.
     - Read the watermark and pass that value. See :doc:`/scene/exporting_to_usd`.
   * - ``open_usd_from_file`` reuses a layer that is already open in the process without reloading it. A file edited on disk can come back stale.
     - Reload or remove the layer through OpenUSD before reopening.
   * - Under the ``RENDERING`` domain, opening USD, ``apply_usd_changes``, and ``apply_usd_time`` write values without recording which prims and attributes changed at their ordinal. A range read covering that ordinal does not report them, and a snapshot ending at an earlier ordinal returns the updated value instead of failing with ``OUT_OF_RANGE``.
     - Read ``RENDERING`` attributes with a current read (``OVSTAGE_ORDINAL_MAX``) after the operation's watermark advance. Do not rely on range reads to detect these writes.

Python
------------------------------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - Read and map views are not valid after :meth:`ovstage.Stage.destroy`. Python does not detect use after destroy.
     - Finish with all views before destroying the stage.
   * - A DLPack consumer view does not keep the group alive. Releasing or dropping the group frees the memory the view points to.
     - Keep the group or its ``ManagedDLTensor`` referenced and unreleased while any view is in use, or copy the data.
   * - ``with`` on a map commits on exit, even if the body raised an exception. A partly filled buffer is committed.
     - Fill the buffer completely, or avoid the context manager when the body can fail partway.
   * - ``ReadColumn.array()`` returns float16 data as ``uint16`` bit patterns.
     - View the result as ``numpy.float16``, or use ``dlpack()``.

Versioning
--------------------------------

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Limitation
     - What to do
   * - The C vtable has no size or version field. Headers newer than the binary can call slots the binary does not have, which is undefined behavior and not an error.
     - Use headers and binary from the same release.
