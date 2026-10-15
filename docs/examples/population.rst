.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Population Example
==================

The Python and C projects populate a small USD scene, inspect values and
metadata, sample an animated value, and recover from a failed load.

* Python: ``examples/python/population`` — run ``uv run main.py``.
* C: ``examples/c/population`` — configure with CMake, build, and run
  ``population`` from the build directory. See :doc:`/c_api/getting_started`
  for package setup.

The adjacent ``scene.usda`` contains ``/World/Body``, a Cube with
``PhysicsRigidBodyAPI``. Its properties include a temperature, a token, two
points, an empty token, and bounds. The prim and temperature property have
custom metadata. ``/World`` has a translation of 10 along X.

Loading the Scene
-----------------

Create the runtime stage and load the physics domain. Later Python snippets run
inside the same context managers; keep the C stage alive until the example ends.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-imports, population-setup

   .. tab-item:: C

      ``waitPop()`` checks population completion; ``waitOp()`` waits for runtime
      operations and releases them. ``check()`` and ``checkOvx()`` stop on errors.

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-setup-c

Selecting Values and Metadata
-----------------------------

Select the Cube's properties and metadata, then populate at ordinal 2.
The complete program first populates the physics domain at ordinal 1.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-paths, population-property-metadata, population-stage-metadata

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-paths-c, population-property-metadata-c, population-stage-metadata-c


Sampling Animation
------------------

The scene uses 24 time codes per second. Its temperature changes from 20 at
time code 0 to 30 at time code 24. Sample one second and read the result:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-time

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-time-c


See :doc:`/scene/population` for the remaining snippets and their API contracts.
