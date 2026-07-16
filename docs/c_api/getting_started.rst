.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Getting Started in C
====================

The C API is the primary public surface of ovstage. The fastest way to get running is the
minimal C/C++ example: it interns paths through the path dictionary, creates an ovstage
instance, writes an attribute column, advances the write floor, and reads the latest
committed data back.

Building the Minimal Example
----------------------------

The example builds standalone with CMake. On first configure it locates an ovstage package
using ``find_package(ovstage)`` and otherwise fetches the released package (refer to
``examples/c/cmake/ovstage.cmake``).

.. code-block:: bash

   cd examples/c/minimal

   # Linux
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ./build/minimal

On Windows, configure and build the same way (``cmake -B build``, then ``cmake --build
build --config Release``) and put the ovstage package ``bin/`` directory on ``PATH`` before
running; on Linux the build sets an rpath onto the package ``bin/`` so the binary runs with
no environment setup. Refer to ``examples/c/minimal/README.md`` for details.

Expected output:

.. code-block:: text

   attribute token <N> = 'temperature'
   read back ordinal 1: 1.0 2.0 3.0

The source for this example lives at ``examples/c/minimal/main.cpp`` in the public ovstage
tree.

Minimal Example
---------------

.. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
   :language: cpp
   :start-after: // [snippet:minimal-write-read]
   :end-before: // [/snippet:minimal-write-read]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

Next Steps
----------

* Explore the :doc:`../examples/index`.
* Refer to the :doc:`index` for the full C API reference.
