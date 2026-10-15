.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

USD Export Example
==================

The ``examples/python/usd-export`` and ``examples/c/usd-export`` projects export
a populated hierarchy and runtime-created attributes. They need a build with
population/export support; no ``pxr`` Python package is required.

Run Python with ``uv run main.py`` from its directory. Outputs use a temporary
directory and are removed when the program exits.

Build C with CMake as described in :doc:`/c_api/getting_started`, then run
``usd-export /tmp/destination.usda``. The hierarchy is saved at that path; other
examples add suffixes such as ``.transforms.usda`` and ``.changed.usda``.

Runtime Data
------------

The function receives ``directory``, a temporary output directory. These imports
and writes create ``/World/Ball`` at ordinal 1 and make it readable. Subsequent
Python snippets run inside the same context managers:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-example-imports, export-example-setup
         :exclude-pattern: ^\s*#\s*\[/?snippet:

   .. tab-item:: C

      ``createStage()`` creates the source instance; ``check()`` and ``checkOvx()``
      stop on errors. ``waitOp()`` waits for runtime operations and releases them.

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-example-setup-c
         :exclude-pattern: ^\s*//\s*\[/?snippet:


Saving an Overlay
-----------------

Export the transform to a new layer, saving and closing it in one call:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-file

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-file-c


For the remaining examples and their contracts, see :doc:`/scene/exporting_to_usd`.
