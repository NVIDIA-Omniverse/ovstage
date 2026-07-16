.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Asynchronous Submit/Observe Model
=================================

ovstage is an asynchronous, ordinal-keyed submit/observe system. State-mutating
and data-producing calls **enqueue** synchronously — they return an
``ovstage_enqueue_result_t`` (status + ``op_index``) immediately — while the real
work runs later. This lets the CPU run ahead of execution: a producer can push
many ordinals of data while consumers **observe** at their own cadence.

Enqueue vs. Execution
---------------------

- An enqueue returning ``OVSTAGE_OK`` means the op was *accepted*, not
  *completed*. Check ``status == OVSTAGE_OK`` before using ``op_index``; a
  non-OK enqueue yields ``OVSTAGE_INVALID_OP_ID``.
- Per-op execution errors surface later, at ``ovstage_wait_op`` (or the matching
  ``ovstage_fetch_*``), not at enqueue time. Refer to :doc:`error_handling`.

Ordinal Ordering and Concurrency
--------------------------------

Writes, deletes, and map commits carry an explicit ``ordinal``:

- **Same-ordinal** ops execute in submission order.
- **Different-ordinal** ops are independent and can run concurrently.

A ``ovstage_wait_op`` that returns ``OVSTAGE_OK`` does **not** imply all your
enqueues finished — ops in other ordinal buckets can still be in flight. Wait on
the specific ``op_id`` whose result you need. ``ovstage_wait_op`` waits for the
whole dependency chain up to and including that ``op_id``.

Blocking, Polling, and Timeouts
-------------------------------

``ovstage_wait_op(instance, op_id, timeout, &wait_result)`` (and the
``ovstage_fetch_*`` calls) share one timeout convention:

.. list-table::
   :header-rows: 1

   * - ``timeout``
     - Behavior
   * - ``0``
     - Poll — return immediately; ``OVSTAGE_ERROR_TIMEOUT`` means "not ready yet."
   * - ``OVSTAGE_TIMEOUT_INFINITE``
     - Block until the op completes.
   * - other (nanoseconds)
     - Wait up to that long, then report ``OVSTAGE_ERROR_TIMEOUT``.

``ovstage_timeout_ns_t`` is a ``uint64_t`` nanosecond count and
``OVSTAGE_TIMEOUT_INFINITE`` is ``~0ULL``. ``OVSTAGE_ERROR_TIMEOUT`` is a
"not-ready" signal, distinct from the terminal ``OVSTAGE_ERROR_OP_FAILED``. On a
timeout, ``wait_result.lowest_pending_op_id`` reports the lowest still-pending op
in the chain — a partial-progress cursor.

After an op completes, release its tracking with ``ovstage_release_op``;
the id must not be reused afterward.

Running the CPU Ahead
---------------------

To run ahead, keep enqueuing across ordinals without blocking, and only wait or
fetch when you actually need a result. Python mirrors the same model — enqueue
methods return handles whose ``.wait(timeout=...)`` polls (``timeout=0``) or
blocks (``TIMEOUT_INFINITE``) and raises ``ovstage.OvstageError`` on op failure:

.. filtered-literalinclude:: ../../examples/python/minimal/main.py
   :language: python
   :start-after: # [snippet:nonblocking-poll]
   :end-before: # [/snippet:nonblocking-poll]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

In C, the same shape is: enqueue, check the enqueue status, then
``ovstage_wait_op`` on the ``op_index`` and inspect per-op errors:

.. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
   :language: cpp
   :start-after: // [snippet:enqueue-wait-error]
   :end-before: // [/snippet:enqueue-wait-error]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

.. note::

   This build retains only the **latest committed** state — do not design async
   flows that read back older ordinals.

Where to Go Next
----------------

- :doc:`error_handling` — enqueue vs. per-op errors and the diagnostics accessors.
- :doc:`dlpack_tensors` — CPU/GPU tensor exchange and ``cuda_sync`` synchronization.
- :doc:`application_flow` — where waiting fits in the overall lifecycle.
