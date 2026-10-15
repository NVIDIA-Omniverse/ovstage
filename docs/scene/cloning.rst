.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Cloning Subtrees
================

:meth:`ovstage.Stage.clone()` copies a subtree to one or more target paths.
For example, copy a robot subtree to create several simulation environments
in one call. The source is a :term:`prim <Prim>` path; each target becomes the
root of a copy.

.. note::

   The clone API is **pre-release and provisional** ("Draft — API in flux").
   Treat the exact symbols and argument ordering as subject to change. Clone
   copies runtime stage content, whether populated from USD or created through
   attribute writes. See :doc:`population` for loading USD content.

Cloning to Multiple Targets
---------------------------

Like a write, clone carries an ``ordinal``. It must clear the applicable floors
and satisfy the configured :ref:`ordinal admission policy
<ordinal-admission-policies>` for target cells and prim lifecycle. Copied cells
are committed at that ordinal: the strict policy requires a newer ordinal for
later customization, even before sealing. ``NONDECREASING`` permits customization
at the same unsealed ordinal; ``FLOOR_ONLY`` skips retained-history admission.
Neither relaxes the clone's operation floors. Its ordinal must exceed the global
floor and effective floors of source columns, even with no targets. Those source
checks are independent of target admission; fresh targets do not bypass them.
Target cells must also clear their operation floors and satisfy the configured
policy, including prim-lifecycle admission under the two history policies.
Advance the floor to make completed clones readable:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:clone-subtree-multienv]
         :end-before: # [/snippet:clone-subtree-multienv]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:clone-subtree-multienv]
         :end-before: // [/snippet:clone-subtree-multienv]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Clone Rules
-----------

* The source must exist and every target must be new. A batch containing an
  existing target creates nothing.
* Empty, invalid, or duplicate target paths fail at submission with
  ``ErrorCode.INVALID_ARGUMENT``. A property path cannot hold child prims.
* Nested targets can be accepted, but a target that also names a prim created
  by another target fails with ``ErrorCode.OP_FAILED`` before cloning. For
  example, targets ``/World/T`` and ``/World/T/Inner`` conflict when the source
  contains a child ``Inner``.
* Relationship targets, path values, and attribute connections inside the source
  subtree are remapped into each copy. Paths outside it remain unchanged.
* Every copied column has :term:`change <Change>` records, including prim type,
  applied schemas, ancestor types, connection-only attributes, and each stored
  type of a mixed-type attribute name. If a name has both a value and a
  connection, it is tracked under the value type. Parent-child lists are not
  change-tracked. Watermarks on copied metadata also gate the clone.
* Cloned prims attach to their remapped hierarchy parent. If that parent is
  absent, they attach to the nearest existing ancestor.

A new target root can still have existing descendants, for example after an
older root was deleted. Cloning merges into those descendants. They keep
attributes absent from the source, combine applied schemas and ancestor types,
and retain their type when the source has none. A conflicting attribute or prim
type, or a missing connection-only destination attribute, fails the clone before
creation. If the source has both a value and connection under one name, the
connection is copied only if the existing prim already has one. Delete existing
descendants first when you need an exact copy.


These snippets demonstrate duplicate-target rejection. The runnable example
has already created ``/World/Proto/Rig`` with one prim at ordinal 2 and advanced
the watermark to 3. The repeated target is rejected before cloning starts:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: setup, clone-duplicate-targets

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: clone-duplicate-targets-c

Waiting and Results
-------------------

:meth:`ovstage.Stage.clone()` blocks and returns :class:`ovstage.CloneResult`.
:meth:`ovstage.Stage.clone_async()` returns :class:`ovstage.CloneOperation`.
Work can execute before you wait; :meth:`wait() <ovstage.CloneOperation.wait>` observes completion.
Use :meth:`stage.clone_async(...).wait().result() <ovstage.Stage.clone_async>` to get the report.

The report contains ``prims_cloned`` across all targets, ``target_count``,
``source_prim_count`` including the source root, and ``ordinal``.
``prims_cloned`` sums the targets' hierarchy subtree sizes after cloning,
including existing attached descendants. Nested targets can count a prim more
than once. For disjoint targets with no existing descendants, it equals
``source_prim_count * target_count``.

In C, wait with :c:func:`ovstage_wait_op()`, fetch the report with
:c:func:`ovstage_fetch_clone_result()`, then release operation tracking with
:c:func:`ovstage_release_op()`. Fetching does not release the report; it remains
available until the operation is released.


These snippets submit a clone of that single-prim source, then wait and inspect
its report. Python caches the report before releasing operation tracking; C
fetches the report before releasing it:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: clone-async-report

   .. tab-item:: C

      ``check()`` checks API status; ``checkWait()`` checks operation errors.

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: clone-async-report-c

Clone Failures
--------------

A failed wait reports ``ErrorCode.OP_FAILED`` and a message. Fetching the report
of a failed operation exposes the more specific code:

* ``ErrorCode.NOT_FOUND`` — missing source.
* ``ErrorCode.PRIM_NOT_FOUND`` — a target already exists.
* ``ErrorCode.WRITE_FLOOR_VIOLATION`` — the ordinal conflicts with an affected
  watermark or retained target-cell/prim-lifecycle change under the configured
  admission policy. Execution failures can surface as ``OP_FAILED`` from wait;
  inspect the diagnostic as described in :doc:`/concepts/error_handling`.

Python's :meth:`CloneOperation.wait() <ovstage.CloneOperation.wait>` and :meth:`result() <ovstage.CloneOperation.result>` release native operation
tracking on completion. To inspect a specific failure code, call
:meth:`ovstage.Stage.fetch_clone_result()` before that release. After a failed
:meth:`wait() <ovstage.CloneOperation.wait>`, the native report is no longer available. Successful reports are
cached by ``CloneOperation``. C callers can wait and fetch before releasing.


These snippets attempt to clone a missing source and fetch the specific
``NOT_FOUND`` failure before releasing the operation. Fetching with the default
infinite timeout waits for completion, so a separate wait is unnecessary:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: clone-specific-failure

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: clone-specific-failure-c

Reading the Completion Report
-----------------------------

These snippets use a separate setup: ``/World/A`` contains one prim with a
value written at ordinal 1, and the watermark is 1. Python shows both the
blocking report and the asynchronous operation's cached result; C fetches before
releasing operation tracking:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_clone.py
         :language: python
         :start-after: # [snippet:clone-result]
         :end-before: # [/snippet:clone-result]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_clone.cpp
         :language: cpp
         :start-after: // [snippet:clone-result-c]
         :end-before: // [/snippet:clone-result-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Further Reading
---------------

- :doc:`/concepts/async_model` — driving the clone enqueue to completion.
- :doc:`/scene/population` — ingesting USD content before cloning.
- :doc:`/guides/runtime_loop` — a full populate/read/update loop.
