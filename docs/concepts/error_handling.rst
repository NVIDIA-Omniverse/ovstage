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
==============================

Most synchronous ovstage calls return an ``ovstage_api_status_t``:
``OVSTAGE_OK`` (0) is success and any non-zero value is an error. Asynchronous
enqueues instead return an ``ovstage_enqueue_result_t``, and completion or
per-op failures surface later through ``ovstage_wait_op``. Distinguish enqueue
success from execution success: ``OVSTAGE_OK`` at enqueue means *accepted*, not
*completed*.

Status Codes
------------

.. list-table::
   :header-rows: 1

   * - Code
     - Value
     - Meaning
   * - ``OVSTAGE_OK``
     - 0
     - Success.
   * - ``OVSTAGE_ERROR_INVALID_ARGUMENT``
     - 1
     - A required argument was invalid.
   * - ``OVSTAGE_ERROR_INVALID_HANDLE``
     - 2
     - A handle did not refer to a live object.
   * - ``OVSTAGE_ERROR_NOT_FOUND``
     - 3
     - The requested item was not found.
   * - ``OVSTAGE_ERROR_PRIM_NOT_FOUND``
     - 4
     - In INSERT (create-only) mode, a prim already exists at the target path. (A missing item is ``OVSTAGE_ERROR_NOT_FOUND`` above.)
   * - ``OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION``
     - 5
     - A write/apply targeted an ordinal at or below the write floor.
   * - ``OVSTAGE_ERROR_NOT_SUPPORTED``
     - 6
     - The operation is not supported by this build.
   * - ``OVSTAGE_ERROR_QUEUE_FULL``
     - 7
     - The submission queue is full.
   * - ``OVSTAGE_ERROR_END_OF_ITERATION``
     - 8
     - No further groups to fetch.
   * - ``OVSTAGE_ERROR_OUT_OF_MEMORY``
     - 9
     - Allocation failed.
   * - ``OVSTAGE_ERROR_LAYOUT_CHANGED``
     - 10
     - The underlying column layout changed.
   * - ``OVSTAGE_ERROR_TIMEOUT``
     - 11
     - Not ready within the timeout (a wait signal, not a failure).
   * - ``OVSTAGE_ERROR_OP_FAILED``
     - 12
     - The op executed but failed.
   * - ``OVSTAGE_ERROR_INTERNAL``
     - 99
     - An internal error occurred.

Turning Codes into Strings
--------------------------

Three diagnostic accessors exist, with distinct return types and lifetimes:

.. list-table::
   :header-rows: 1

   * - Accessor
     - Returns
     - Use
   * - ``ovstage_get_error_string(instance, code)``
     - static ``const char*`` (never ``NULL``)
     - Human-readable text for a status code. Vtable-dispatched, so it needs a live instance.
   * - ``ovstage_get_last_op_error(instance, op_id)``
     - ``ovx_string_t`` (``{NULL,0}`` if the id is unknown or did not fail)
     - The message for a specific failed op. Transient — copy it to retain (refer to :doc:`string_handling`).
   * - ``ovstage_get_last_error(void)``
     - ``ovx_string_t``
     - A free function readable even when ``ovstage_create_instance`` itself failed.

Checking a Synchronous Call
---------------------------

.. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
   :language: cpp
   :start-after: // [snippet:check-sync-error]
   :end-before: // [/snippet:check-sync-error]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

Inspecting Per-Op Errors After a Wait
-------------------------------------

After a wait, inspect ``ovstage_op_wait_result_t.error_op_ids`` /
``error_op_id_count`` and call ``ovstage_get_last_op_error`` for each failed id
*immediately* — the message is invalidated by the next wait on the same thread,
so copy it if you need to keep it.

.. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
   :language: cpp
   :start-after: // [snippet:enqueue-wait-error]
   :end-before: // [/snippet:enqueue-wait-error]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

Errors in Python
----------------

Python raises exceptions instead of returning codes:
``ovstage.OvstageError`` for stage operations and ``ovstage.OvxError`` for
path-dictionary operations. Both subclass ``RuntimeError`` and carry a numeric
``code`` and a message; asynchronous errors are raised from ``.wait()``.

.. filtered-literalinclude:: ../../examples/python/minimal/main.py
   :language: python
   :start-after: # [snippet:error-handling]
   :end-before: # [/snippet:error-handling]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

Notes
-----

- ``OVSTAGE_ERROR_TIMEOUT`` from ``ovstage_wait_op`` is "not ready yet," not a
  failure; ``wait_result.lowest_pending_op_id`` reports the lowest still-pending
  op in the chain.
- ``ovstage_advance_write_floor`` never raises ``WRITE_FLOOR_VIOLATION`` —
  backwards advances clamp rather than error.

Where to Go Next
----------------

- :doc:`string_handling` — printing and copying the ``ovx_string_t`` values these accessors return.
- :doc:`async_model` — the enqueue/wait lifecycle that surfaces per-op errors.
