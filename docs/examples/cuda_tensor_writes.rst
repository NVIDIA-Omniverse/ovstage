.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

CUDA Tensor Writes
==========================

The write-flavors examples show CUDA input followed by CPU read-back. Both
synchronize the producer before the write and keep its buffer alive until the
write completes. A CUDA-capable GPU and working NVIDIA driver are required.

Python
--------------

The Python source is ``examples/python/write-flavors/main.py``. It runs the CPU
workflow first and attempts the CUDA section when Warp and a CUDA device are
available. Install ``warp-lang`` alongside the example's dependencies and run
``main.py``. If CUDA is unavailable, the program reports that GPU ingest was
skipped; a successful CPU run does not validate the GPU section.

This excerpt writes a Warp CUDA array into the selected prims:

.. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
   :language: python
   :start-after: # [snippet:dlpack-cuda-ingest]
   :end-before: # [/snippet:dlpack-cuda-ingest]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

C API
-------------

The C++ source is ``examples/c/write-flavors/cuda_ingest.cpp``. Enable its optional
CMake target with the CUDA Toolkit installed:

.. code-block:: bash

   cmake -S examples/c/write-flavors -B build/cuda-ingest -DOVSTAGE_EXAMPLE_CUDA=ON
   cmake --build build/cuda-ingest --target cuda-ingest
   ./build/cuda-ingest/cuda-ingest

Run these commands from the public repository root. The executable returns 0 when
CPU read-back matches the input, 1 for a value mismatch, and 77 when no CUDA device
can be used. ``checkCuda()`` throws on CUDA errors; ``waitOp()`` checks ovstage
submission and execution errors.

This excerpt constructs the CUDA tensor descriptor and keeps its allocation
alive through the write:

.. filtered-literalinclude:: ../../examples/c/write-flavors/cuda_ingest.cpp
   :language: cpp
   :start-after: // [snippet:dlpack-cuda-ingest-c]
   :end-before: // [/snippet:dlpack-cuda-ingest-c]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

See :doc:`/scene/device_placement` for the experimental GPU-to-GPU workflow,
extension access, event ordering, and buffer lifetimes.
