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

``ovstage_clone`` copies the subtree under a source prim to one or more new
target paths in a single ordinal-keyed call. Passing several targets in one call
is the **multi-environment pattern**: stamp out N copies of a prototype subtree —
for example, one scene or robot per reinforcement-learning environment — in a
single enqueue.

.. note::

   The clone API is **pre-release and provisional** ("Draft — API in flux").
   Treat the exact symbols and argument ordering as subject to change. Clone
   operates on populated content (see :doc:`population`).

Cloning to Multiple Targets
---------------------------

Like a write, clone carries an ``ordinal`` and is sealed by the write floor.
Choose an ordinal above the current floor, then advance the floor afterward to
make the clones readable:

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

Semantics
---------

- **Create-only, all-or-nothing.** The source must already exist and every
  target must be new. A batch containing any pre-existing target clones nothing.
- **Ordinal-keyed.** Pick an ordinal strictly above the write floor (and above
  the seal of every attribute the clone reproduces); advance the floor
  afterward to read the clones.
- **Asynchronous.** Clone is an enqueue returning an ``op_index``; nothing
  exists until you observe it — C ``ovstage_wait_op`` + ``ovstage_release_op``,
  Python ``.wait()`` (``Stage.clone`` blocks and raises ``OvstageError``;
  ``Stage.clone_async`` returns an ``Operation``).
- **Internal paths are rebased; external paths remain shared.** Relationship
  targets, path values, and attribute connections that point inside the source
  subtree are retargeted to each clone. Paths outside the subtree are unchanged.
- **Change tracking.** Cloned attribute values, including relationship targets,
  are ordinal-change-tracked. Attribute connections and scene hierarchy changes,
  such as parent child lists, are not.

Where to Go Next
----------------

- :doc:`/concepts/async_model` — driving the clone enqueue to completion.
- :doc:`/scene/population` — ingesting USD content before cloning.
- :doc:`/guides/runtime_loop` — a full populate/read/update loop.
