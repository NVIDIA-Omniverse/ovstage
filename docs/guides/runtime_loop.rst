.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Runtime Loop
============

This guide drives an ovstage scene headlessly — no renderer attached: load a USD
scene into the runtime table through :doc:`population </scene/population>`, read prims
back to confirm, update the live stage, and re-read. The application owns the
ordinal lifecycle throughout: populate at one ordinal, then write updates at
higher ordinals, sealing each tick with ``advance_write_floor``.

.. note::

   This walkthrough uses the population capability. The runtime-loop examples
   are **pre-release and provisional** ("Draft — API in flux"); the
   transform-write recipe below uses the ``omni:xform`` attribute and
   ``MATRIX`` semantic and is subject to change.

Setup
-----

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/runtime-loop/main.py
         :language: python
         :start-after: # [snippet:setup]
         :end-before: # [/snippet:setup]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/runtime-loop/main.cpp
         :language: cpp
         :start-after: // [snippet:setup]
         :end-before: // [/snippet:setup]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Two Update Paths
----------------

After a scene is live there are two ways to change it:

1. **Write into the runtime table** at a new ordinal — a direct edit with no USD
   round-trip (for example, animate a prim's ``omni:xform`` transform over
   frames).
2. **Edit the USD source** with ``add_usd_reference`` and propagate it through
   with ``apply_usd_changes``.

1. Populate
-----------

Load the scene at ordinal 1 and seal it:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/runtime-loop/main.py
         :language: python
         :start-after: # [snippet:populate]
         :end-before: # [/snippet:populate]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/runtime-loop/main.cpp
         :language: cpp
         :start-after: // [snippet:populate]
         :end-before: // [/snippet:populate]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

2. Read Back to Confirm
-----------------------

Prove the populate landed by reading the reserved ``usd-prim-type`` metadata over
the scene query and resolving each token to its type name:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/runtime-loop/main.py
         :language: python
         :start-after: # [snippet:read-populated]
         :end-before: # [/snippet:read-populated]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/runtime-loop/main.cpp
         :language: cpp
         :start-after: // [snippet:read-populated]
         :end-before: // [/snippet:read-populated]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

.. note::

   This example reads back the transform it wrote itself. Reading a *populated*
   implementation-defined derived scene transform (``omni:fabric:localMatrix`` /
   ``omni:fabric:worldMatrix``) back through
   ``read_attributes`` is not part of this validated flow.

3. Update the Table
-------------------

Animate a prim by writing ``omni:xform`` (a 4×4 matrix, ``MATRIX`` semantic)
straight into the table over successive ordinals, advancing the floor each step:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/runtime-loop/main.py
         :language: python
         :start-after: # [snippet:update-table]
         :end-before: # [/snippet:update-table]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/runtime-loop/main.cpp
         :language: cpp
         :start-after: // [snippet:update-table]
         :end-before: // [/snippet:update-table]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

4. Edit the USD Source
----------------------

Add a reference onto a prim, propagate it into the runtime table at a fresh
ordinal, and re-read the prim types:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/runtime-loop/main.py
         :language: python
         :start-after: # [snippet:update-usd]
         :end-before: # [/snippet:update-usd]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/runtime-loop/main.cpp
         :language: cpp
         :start-after: // [snippet:update-usd]
         :end-before: // [/snippet:update-usd]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Expected Output
---------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: text

         populated prim types: Xform, Mesh, Mesh
         final Torus xform translation (row [3][0:3]): [100.  25.   0.]
         after USD edit, prim types: Xform, Mesh, Mesh, Cube

   .. tab-item:: C

      .. code-block:: text

         populated prim types: Xform Mesh Mesh
         final Torus xform translation (row [3][0:3]): 100.0 25.0 0.0
         after USD edit, prim types: Xform Mesh Mesh Cube

Where to Go Next
----------------

- :doc:`/scene/population` — the population API in detail.
- :doc:`/scene/writing_attributes` — the table-write path and attribute semantics.
- :doc:`/scene/reading_attributes` — reading metadata and columns back.
