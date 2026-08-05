.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Application Flow
================

ovstage is a data stage, not a renderer. A program creates an instance,
identifies prims and attributes through the instance-owned
:doc:`path dictionary </scene/path_dictionary>`, writes attribute columns at
explicit *ordinals*, seals them by advancing the write floor, and reads them
back. Every step rides on the same asynchronous submit/observe model: enqueues
are synchronous and return an ``op_index`` immediately, while the actual work
runs later and is observed with ``ovstage_wait_op`` or a ``fetch_*`` call.

Two cross-cutting rules govern every stage:

- **Asynchronous enqueue/observe** — an enqueue returns an ``op_index``
  immediately; nothing has executed until you wait or fetch. Refer to
  :doc:`async_model`.
- **Ordinals and the write floor** — writes carry an ``ordinal``; advancing the
  write floor seals everything at or below it. Reads target sealed data at or
  below the write floor; queries resolve against the latest committed state.

Canonical Lifecycle
-------------------

The minimal program follows this order:

1. ``ovstage_create_instance`` — create the stage.
2. ``ovstage_get_path_dictionary`` — obtain the instance-owned dictionary.
3. Intern a token / build a prim-path list.
4. ``ovstage_query_from_path_list`` — open a :doc:`query </scene/queries>` over those prims.
5. ``ovstage_write_attribute`` — :doc:`write </scene/writing_attributes>` a column at an ``ordinal``.
6. ``ovstage_advance_write_floor`` — seal the ordinal so it becomes readable.
7. ``ovstage_read_attributes`` / ``ovstage_fetch_read_next`` — :doc:`read </scene/reading_attributes>` the column back.
8. Release in reverse: ``ovstage_release_group`` → ``ovstage_release_read`` →
   release the path list → ``ovstage_destroy_instance``.

The path dictionary is instance-owned and must never be freed by the caller.

The write → seal → read core, in both languages:

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

Python vs. C
------------

.. list-table::
   :header-rows: 1

   * - Concern
     - Python
     - C
   * - Instance Lifetime
     - Managed by the ``Stage`` context manager.
     - ``ovstage_create_instance`` / ``ovstage_destroy_instance``.
   * - Enqueue
     - Methods return handle objects (``Query``, ``Read``, ``Map``).
     - Functions return ``ovstage_enqueue_result_t`` (status + ``op_index``).
   * - Observe
     - ``.wait(timeout=...)`` on the handle.
     - ``ovstage_wait_op`` / ``ovstage_fetch_*``.
   * - Attribute Data
     - DLPack-compatible arrays (NumPy, etc.).
     - ``DLTensor`` payloads (refer to :doc:`dlpack_tensors`).
   * - Errors
     - Raise ``ovstage.OvstageError`` / ``ovstage.OvxError``.
     - Return status codes; inspect with the diagnostics accessors.

Common Failure Modes
--------------------

- **Reading selected unsealed state fails** with
  ``OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION``. Advance the write floor first
  (step 6).
- **A fixed range rejects a later change to the same selected key.** If a
  selected ``(attribute, path)`` also has a retained change after the range end,
  the read fails with ``OVSTAGE_ERROR_OUT_OF_RANGE`` whether or not that later
  change is sealed. Widen or rebase the range, repoll from a newer cursor, or
  request current state when that is what the consumer wants. Advancing the
  floor alone does not resolve this error.
- **Writing at or below the write floor is rejected** with
  ``OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION``. Choose an ordinal above the current
  floor. (Advancing the floor backwards is *not* an error — it clamps.)

.. note::

   This build retains only the **latest committed** state. Fixed ranges select
   the keys that changed, not historical payloads, and return
   ``OVSTAGE_ERROR_OUT_OF_RANGE`` when a selected key changed again after the
   range end.

Where to Go Next
----------------

- :doc:`async_model` — enqueue vs. execution, op ids, waits, and timeouts.
- :doc:`/scene/path_dictionary` — interning tokens, paths, and path lists.
- :doc:`/scene/writing_attributes` / :doc:`/scene/reading_attributes` — the write and read paths in detail.
- :doc:`error_handling` — status codes, per-op errors, and Python exceptions.
- :doc:`/scene/population` — ingesting USD content into the stage.
