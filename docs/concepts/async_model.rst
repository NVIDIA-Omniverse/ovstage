.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Asynchronous Operations
=======================

Asynchronous calls return without waiting for their work to complete. Check whether the call was
accepted, then wait when you need its result. Work can finish before you call the
wait function.

Submit and Wait
-----------------------

In Python, :meth:`ovstage.Operation.wait()` waits, checks errors, and releases
operation tracking after completion. On success it returns the operation itself.
A read or map handle's :meth:`wait() <ovstage.Operation.wait>` returns that handle, so a subsequent fetch can
chain. Keep the handle in a ``with`` block while using it. A completed operation
cannot be waited again. Submission or execution failures raise
:class:`ovstage.OvstageError`. Use :meth:`ovstage.Stage.wait_op()` when
you need to inspect completion without releasing the :term:`operation ID <Operation ID>`.

Some submission calls block the caller before returning. ``write_attribute``, ``write_attributes``, ``delete_attributes``, and ``map_attribute`` wait until the query they use is ready. ``release_query`` and ``release_read`` are synchronous. See :doc:`/concepts/threading_lifetime` for details.

A mutation's ``Operation.ok`` reports only enqueue acceptance, even if execution
later fails. Retain it and call ``wait()``. Use query/read/map context managers
for ownership and explicitly observe their operations; finalizers provide cleanup,
not reliable error reporting.

Mutation calls return an :class:`ovstage.Operation`;
:meth:`ovstage.Stage.clone_async()` returns a :class:`ovstage.CloneOperation`.
Its :meth:`ovstage.CloneOperation.result()` returns the clone report.

In C, an asynchronous call returns ``ovstage_enqueue_result_t``. Check its
``status`` before using ``op_index``; a rejected call returns
``OVSTAGE_INVALID_OP_ID``. Call :c:func:`ovstage_wait_op()` to wait, then check both
its return status and ``error_op_ids``. Copy messages before the next wait on the
same thread. For a clone report, use :c:func:`ovstage_fetch_clone_result()`.
See :doc:`error_handling`.

Ordering
----------------

Writes, clones, deletes, and writes completed through mapped buffers carry an
:term:`ordinal <Ordinal>`. Operations at the same ordinal execute in submission order.
Operations at different ordinals may run concurrently and do not guarantee
execution order.

Under the ``STRICTLY_INCREASING`` policy, ordering does not allow multiple
modifications to the same cell at one ordinal. Writes, map commits, clones and
deletes must use an ordinal newer than the retained committed changes to each
affected cell, and clear applicable floors and prim-lifecycle checks. Disjoint
cells may share an ordinal. Process configuration can select ``NONDECREASING`` to
allow equal unsealed ordinals or ``FLOOR_ONLY`` to skip retained-history
conflicts; see :ref:`ordinal-admission-policies`.

Producers updating disjoint cells can proceed independently, subject to shared
write floors and prim-lifecycle checks. Producers updating the same cells must
coordinate completion or handle rejection: if ordinal 11 commits first, a later
write at 10 is rejected under both ``STRICTLY_INCREASING`` and ``NONDECREASING``.
For successive updates that must all succeed, wait for the earlier update to
complete successfully before submitting the next. An atomic ordinal counter
alone does not order submission or completion.

WRITE and MAP enqueue check known floors provisionally. A retained value
conflict can fail WRITE at execution or MAP at setup; MAP also checks again at
publication. Always wait or fetch the result before treating an enqueue as
successful. For partially failed operations, completed entries remain
committed. Repair and same-ordinal retries depend on the selected policy and
current stage state; see :doc:`error_handling` for the full recovery rules.


Advancing a :term:`watermark <Watermark>` is ordered after preceding writes in its scope at
or below the target ordinal. Reads do not automatically wait for that advance;
observe its successful completion before issuing dependent reads.

Waiting for an operation also waits for its dependencies. It does not wait for
every operation on the stage. Retain input buffers until the operation using
them completes, and release read results before overlapping writes. With a
placement availability event, operation completion can precede device completion:
keep sources and result groups alive through their device work. See
:ref:`ovstage-placement-synchronization`.

Before advancing the watermark, check every required write and :ref:`repair any partial failure <ovstage-write-recovery>`. A successful advance does not report earlier write failures. See :doc:`overview` for the complete update sequence.

Running producers ahead does not preserve earlier values for later reads.
See :doc:`overview` for the historical-read limitations.

Using ovstage with a Renderer
-----------------------------

An attached ovrtx renderer consumes completed ovstage updates. Finish the mutations for a render step, advance the write floor to cover them, and wait for that advance before rendering. After loading USD or applying structural USD changes, complete ``ovrtx_update_from_stage()`` before ``ovrtx_step_with_stage()``. See :doc:`/scene/population` for the population sequence.

If a render operation fails because the stage has unsealed changes, finish the pending mutations, advance the write floor, wait for completion, and retry. Check completion through ``ovrtx_wait_op()``; accepting a render submission does not mean that rendering succeeded.

Do not depend on overlapping rendering and stage mutations to make progress in a particular order. Complete each operation needed by the next step, and use finite wait timeouts where the calling thread must stay responsive.

Reads During Rendering
----------------------

When ovrtx renders from an attached runtime stage, it
holds an internal lock while accessing that stage. Queries and attribute reads
can complete while this lock is held. New writes, deletes, mapped writes,
clones, hierarchy computations, and watermark advances submitted through
ovstage wait until the lock is released.

Complete the required :term:`watermark <Watermark>` advances before rendering;
ovrtx rejects a rendering step if the stage still has changes above their
watermarks. The lock does not preserve historical values: queries still select
prims from the current stage, and reads have the retention limits described in
:doc:`overview`.

Shared Stage Lifetime
---------------------

An attached renderer can retain its own handle to the runtime stage. In that
case, :meth:`Stage.destroy() <ovstage.Stage.destroy>` releases the application's
handle, while the renderer's handle keeps the shared stage alive. The same rule
applies to :c:func:`ovstage_destroy_instance()` in C.

Do not use the released handle or destroy it while another thread is using it.
Pending operations remain valid through a surviving handle to the same stage;
complete and release them through that handle. Before releasing the final
handle, release all operations, query/read/map handles, and read results.
For normal application cleanup, finish and release your work before destroying
your :class:`ovstage.Stage`.

Destroying the application's stage handle does not detach an already attached ovrtx renderer. Use the renderer's public detach or destruction API when you are finished with that attachment. The destroyed application handle remains invalid; do not use it to submit or wait for more work.

Timeouts and Polling
----------------------------

Python waits and fetches take timeouts in nanoseconds. Use
:data:`ovstage.TIMEOUT_INFINITE` to wait without a time limit:

.. list-table::
   :header-rows: 1

   * - Timeout
     - Behavior
   * - ``0``
     - Return immediately. ``ErrorCode.TIMEOUT`` means the result is not ready.
   * - ``TIMEOUT_INFINITE``
     - Wait without a time limit.
   * - Any other value
     - Wait up to that many nanoseconds.

In the result from :meth:`ovstage.Stage.wait_op()`, ``lowest_pending_op_id`` identifies the earliest unfinished
operation among those the wait depends on. A timeout does not mean the operation
failed. Once it finishes, read its diagnostics and call
:meth:`ovstage.Stage.release_op()`. Do not reuse the ID after release.

In C, :c:type:`ovstage_timeout_ns_t` is an unsigned 64-bit nanosecond count,
and ``OVSTAGE_TIMEOUT_INFINITE`` disables the time limit. Release completed
operation tracking with :c:func:`ovstage_release_op()`.

.. note::

   For repeated polling in Python, use :meth:`ovstage.Stage.wait_op()` with
   ``timeout=0``. It returns the status, failed operation IDs, and the earliest
   pending ID. Check both the status and failed IDs, then release operation
   tracking after completion. Keep input buffers alive while polling.
   You can also poll with :meth:`Operation.wait(timeout=0) <ovstage.Operation.wait>`. A timeout raises
   ``OvstageError`` with ``ErrorCode.TIMEOUT`` and leaves the operation waitable,
   retaining its input buffers. Successful completion consumes the operation.

See :doc:`application_flow` for the write-and-wait sequence in Python and C, and
:doc:`error_handling` for the error-checking helpers.

.. _ordinal-bounds-and-initial-state:

Ordinal bounds and initial state
--------------------------------

Ordinals are signed 64-bit integers. Use ``OVSTAGE_ORDINAL_MIN`` and
``OVSTAGE_ORDINAL_MAX`` in C, or ``ovstage.ORDINAL_MIN`` and
``ovstage.ORDINAL_MAX`` in Python, for the bounds (``INT64_MIN`` and
``INT64_MAX``). Prefer these named bounds over integer literals or the
underlying integer-type limits in application code.

A new stage is empty and sealed through ``OVSTAGE_ORDINAL_MIN``, inclusively.
Its global floor query returns ``OVSTAGE_ORDINAL_MIN``, and an empty snapshot
at that ordinal succeeds. Writes at the minimum are rejected; the writable
range is ``OVSTAGE_ORDINAL_MIN + 1`` through ``OVSTAGE_ORDINAL_MAX``.

Negative ordinals and zero follow the same admission and sealing rules as
positive ordinals. For example, a new stage can populate or write at zero,
then seal through zero and read that data. The global floor query returns
zero after that advance, and subsequent writes at or below zero fail.

For current-value reads, use the maximum:

.. code-block:: c

   ovstage_ordinal_range_t current = {0};
   current.end_ordinal = OVSTAGE_ORDINAL_MAX;

.. code-block:: python

   import ovstage

   current = ovstage.OrdinalRange.latest(ovstage.ORDINAL_MAX)

The maximum requests current values; selected changes must still be sealed.
