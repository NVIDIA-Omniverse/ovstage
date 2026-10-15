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
========================

This page shows how to create an ovstage instance, write
:term:`attributes <Attribute>`, and read them back. See :doc:`overview` for the
data model and :doc:`async_model` for waiting on operations.

Create, Write, and Read
-------------------------------

1. Create the stage with the :class:`ovstage.Stage` context manager.
2. Obtain its :class:`ovstage.PathDictionary` and build a list of :term:`prims <Prim>`.
3. Open a query over the list with :meth:`ovstage.Stage.query_from_path_list()`.
4. Write values at an :term:`ordinal <Ordinal>` that clears the current
   :term:`watermark <Watermark>` and satisfies :ref:`ordinal-admission-policies`.
   Wait for the write to complete and check for errors.
5. Advance the watermark and wait for completion.
6. Read the values, then release the results before updating the same data.
7. Release the read and query handles and the path list before destroying the stage.
   In C, also release tracking for completed operations. Do not destroy the
   instance-owned path dictionary separately.

In Python, ``Operation.ok`` checks only enqueue acceptance. Call ``wait()``
on mutations and observe query/read/map operations inside their context managers.
Cleanup is not a substitute for checking completion errors.

The snippets below write three temperature values at ordinal 1, advance the
watermark, and read them back. ``query`` selects three prims; ``attr`` identifies
``temperature``. Python imports NumPy as ``np`` and ``OrdinalRange`` from
``ovstage``. The C API example uses C++; its ``waitOp()`` helper waits, checks
errors, and releases operation tracking.

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

      The C example releases the read handle separately during cleanup:

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:overview-release-read]
         :end-before: // [/snippet:overview-release-read]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Python releases the read when its ``with`` block exits. Within that block,
:meth:`read.wait().fetch_next() <ovstage.Read.fetch_next>` can combine the wait and fetch; :meth:`wait() <ovstage.Read.wait>` returns
the read handle. In both languages,
releasing a read handle does not release its returned groups of data. Release
those explicitly, as shown above. This example returns one group; general readers
must fetch all groups. See :doc:`/scene/reading_attributes`.

Further Reading
-----------------------

* :doc:`/scene/path_dictionary` — create path lists and resolve names.
* :doc:`error_handling` — check errors and recover from failed writes.
* :doc:`/scene/population` — load USD content into the stage.
