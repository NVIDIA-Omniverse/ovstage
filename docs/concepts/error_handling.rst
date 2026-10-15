.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Error Handling and Diagnostics
======================================

Check synchronous return values immediately. For asynchronous calls, check both
submission and completion. A successful submission does not mean the operation
succeeded.

Checking Errors
-----------------------

Python raises :class:`ovstage.OvstageError` for stage errors and
:class:`ovstage.OvxError` for path-dictionary errors. Both derive from ``RuntimeError``
and carry a numeric ``code`` and a message. Some invalid Python arguments raise ``TypeError`` or ``ValueError``
before calling the native API. On success, :meth:`wait() <ovstage.Operation.wait>` returns the operation or
handle it was called on.

In C, check the status returned by the call. After :c:func:`ovstage_wait_op()`,
also inspect every :term:`operation ID <Operation ID>` in ``error_op_ids``. Retrieve its message
before the next wait on the same thread, which invalidates the list and messages.
Release operation tracking only after reading the diagnostics.

The boundary where a failure is discovered affects its reported code. A rejected
enqueue can return a specific status such as ``WRITE_FLOOR_VIOLATION`` or
``OUT_OF_RANGE`` directly. For an accepted operation that fails during execution,
``wait_op`` and mutation ``wait()`` generally report ``OP_FAILED``; inspect the
diagnostic for the underlying reason.

Reads can validate an already-ready query at enqueue, or defer validation until
their producer runs. A fetch that waits for a failed producer can report
``OP_FAILED``; a fetch after that producer has completed can expose its specific
recorded status. Resolve the query first when using ``OUT_OF_RANGE`` to choose
a current-state fallback, and still handle deferred producer failures.

The snippets below show Python catching a failed write and the C++ helper used
by the C examples to wait, check errors, and release operation tracking.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:error-handling]
         :end-before: # [/snippet:error-handling]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      The synchronous status helper used by ``waitOp()`` is:

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:check-sync-error]
         :end-before: // [/snippet:check-sync-error]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

      The wait helper checks both the returned status and failed operation IDs:

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:enqueue-wait-error]
         :end-before: // [/snippet:enqueue-wait-error]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Status Codes
--------------------

Use :class:`ovstage.ErrorCode` names, not numeric values. C uses the corresponding
constants in :c:type:`ovstage_api_status_t`: ``OVSTAGE_OK`` for ``ErrorCode.OK``,
and ``OVSTAGE_ERROR_*`` for the other members.

.. list-table::
   :header-rows: 1
   :widths: 42 58

   * - Status
     - Meaning
   * - ``ErrorCode.OK``
     - The call succeeded. For an enqueue, this means it was accepted.
   * - ``ErrorCode.INVALID_ARGUMENT``
     - An argument is invalid.
   * - ``ErrorCode.INVALID_HANDLE``
     - The handle is invalid or has been released.
   * - ``ErrorCode.NOT_FOUND``
     - The requested item was not found.
   * - ``ErrorCode.PRIM_NOT_FOUND``
     - Despite its name, this reports an existing prim rejected by ``PrimMode.INSERT``.
   * - ``ErrorCode.WRITE_FLOOR_VIOLATION``
     - A mutation conflicts with an applicable watermark or retained value/prim-lifecycle
       change under the configured admission policy, or a read selects an unsealed
       change at or before its requested end. The end itself is not compared with
       the watermark.
   * - ``ErrorCode.NOT_SUPPORTED``
     - This build does not support the operation or requested data.
   * - ``ErrorCode.QUEUE_FULL``
     - The submission queue is full.
   * - ``ErrorCode.END_OF_ITERATION``
     - There are no more results to fetch.
   * - ``ErrorCode.OUT_OF_MEMORY``
     - An allocation or size calculation failed recoverably.
   * - ``ErrorCode.LAYOUT_CHANGED``
     - The attribute layout changed during a map operation.
   * - ``ErrorCode.TIMEOUT``
     - The wait or fetch did not finish within its timeout.
   * - ``ErrorCode.OP_FAILED``
     - An asynchronous operation failed. Inspect its diagnostic.
   * - ``ErrorCode.OUT_OF_RANGE``
     - A selected attribute on a queried path has a recorded change after the
       requested end. The older value is unavailable even after sealing the later
       change. Request a newer end or current state; see :doc:`/scene/reading_attributes`.
   * - ``ErrorCode.INTERNAL``
     - An internal error occurred.

:meth:`ovstage.Read.fetch_next()` and :meth:`ovstage.Map.fetch_next()` return
``None`` at the end of iteration rather than raising an exception.

See :doc:`overview` for diagrams explaining :term:`watermark <Watermark>` and range-read
errors. A watermark advance to a lower ordinal has no effect; it does not move
the watermark backwards or report a watermark violation.

Recovering from a Failed Write
--------------------------------------

A failed batch can leave some :term:`changes <Change>` applied:

* Successfully completed entries remain applied when a later entry fails.
  The API does not return a list of completed entries. Repair a batch that may
  have partly succeeded using a newer :term:`ordinal <Ordinal>`.
* If no entry completed, a corrected retry at the same ordinal may be accepted,
  subject to the current watermark and other write rules. This includes a
  one-entry write whose change was not applied successfully.
* A failed write can leave a :term:`prim <Prim>` or :term:`attribute <Attribute>` visible to queries.
  Visibility alone does not prove a change was recorded. ``PrimMode.INSERT`` can reject
  this existing target; ``PrimMode.UPSERT`` can repair it.

Reserved metadata entries can complete before ordinary attribute entries
during execution and remain committed if a later entry fails. Allocating ordinary attribute storage
alone does not count as a successful write.

Under ``STRICTLY_INCREASING``, replaying completed cells at their recorded
ordinal is rejected. Disjoint unfinished cells may still admit a same-ordinal
retry; ``NONDECREASING`` and ``FLOOR_ONLY`` may also allow replay of completed
cells. Every retry must satisfy current watermarks, the selected policy, and
stage state. Failure does not reserve an ordinal for later retry. See
:ref:`ordinal-admission-policies`.

.. note::

   A successful repair can record attribute changes without recording creation
   of a prim left present by the earlier failure. Do not infer creation events
   from query visibility. The :meth:`ovstage.Stage.write_attributes()` reference
   describes the full recovery contract.

In C, a synchronous recoverable allocation failure returns
``OVSTAGE_ERROR_OUT_OF_MEMORY``. A failure during an enqueued write is reported
as ``OVSTAGE_ERROR_OP_FAILED`` through the wait, with the allocation error in
the operation diagnostic.

.. warning::

   If an entry was applied successfully but the runtime cannot record its
   committed prims or attributes, ordinal ownership, and change records
   consistently, the implementation terminates the process. This is not a returned status or a
   Python exception.

Diagnostic Messages
---------------------------

Python exceptions carry the error message. In C, use the accessor that matches
the failure:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Accessor
     - Result and lifetime
   * - :c:func:`ovstage_get_error_string()`
     - Static, non-null ``const char*`` for a status code. Requires a live instance.
   * - :c:func:`ovstage_get_last_op_error()`
     - Borrowed ``ovx_string_t`` for a failed ID from the last wait. Copy before another wait on
       the same thread or releasing operation tracking.
   * - :c:func:`ovstage_get_last_error()`
     - Borrowed ``ovx_string_t``, available without a live instance, including after
       instance creation fails. Copy it before subsequent calls replace it.

An unknown or non-failed ID returns an empty view from
:c:func:`ovstage_get_last_op_error() <ovstage_get_last_op_error>`. See :doc:`string_handling` for copying returned
strings and :doc:`async_model` for timeouts.

Map Commit Failures
-------------------

A rejected :c:func:`ovstage_unmap_group <ovstage_unmap_group>` enqueue leaves the map and group unchanged.
Except for :ref:`availability-event record failures <ovstage-placement-failures>`,
after acceptance, execution failure makes the map terminal; final unmap reports
that failure while releasing it. Repair uses a new map or write, not another
commit attempt in the failed session. A failed write does not commit new prims
or attributes, schema or semantic changes, hierarchy connections, or value and
prim-lifecycle change records. It also records no new ordinal ownership. Groups
committed earlier remain committed, including their structural changes.
Successful final unmap commits only remaining groups and does not repeat changes
from earlier commits.

Map commits are not transactional and provide no rollback guarantee. A failed
write may have modified some stored rows; these partial changes can be observable
even though the write was not recorded as successful. Do not infer which rows
completed from those changes or treat final unmap as an atomic commit of the
whole session.

An unplaced map uses CPU storage, so any such unmap that still has payload to
commit rejects a non-zero ``cuda_stream`` or ``cuda_event``. That rejection is
an accepted failure and is therefore terminal for the session: finalize with
:c:func:`ovstage_unmap_attribute <ovstage_unmap_attribute>` and start a new map to retry. A cleanup-only close
of :c:func:`ovstage_unmap_attribute <ovstage_unmap_attribute>` -- every group already committed by
:c:func:`ovstage_unmap_group <ovstage_unmap_group>` -- has no payload and never inspects the sync, so this
is not a general argument check.

.. _ovstage-placement-failures:

Placement Availability Failures
-------------------------------

The experimental placement extension has a post-commit exception: if CUDA
refuses to record ``availability_event``, an otherwise successful write or map
commit reports ``OP_FAILED`` while its values, change records, and ordinal
ownership remain committed. Retrying must satisfy the configured admission
policy; ``STRICTLY_INCREASING`` requires a newer ordinal for those cells.
A failed event record on ``unmap_group`` leaves the session open, while final
``unmap_attribute`` still consumes its handle. A placed read fails instead of
returning payload. An operation completed during enqueue can report this
failure synchronously after its effects have occurred.

The event remains untouched and cannot prove device completion. After observing
the failure, retain write sources until synchronizing the specific placement
stream, or the recording device for a default/per-thread stream. An ordinary
synchronous rejection owes no event record; do not blindly wait on the event
after rejection. See :ref:`ovstage-placement-synchronization` and the
:class:`ovstage.Placement` reference for ordering, lifetime, and allocation-failure
details. Event completion alone never replaces checking operation status.

Query Refresh Failures
----------------------

In this implementation, a failed query refresh after a structural change marks
all ready query handles on the stage as failed, including unrelated queries.
Release and recreate those handles; retrying them does not restore readiness.
The refresh failure is not reported on the operation that caused the change,
which can still report success. It first surfaces when an affected handle is
used, currently without a diagnostic message.
