.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Benchmarking
============

The ovstage C package and the Python wheel include ``benchmark.ovstage``, a
`Google Benchmark <https://github.com/google/benchmark>`_ executable that measures
ovstage operations through the public C API.

.. list-table::
   :header-rows: 1

   * - Package
     - Location
   * - C package (``.zip``)
     - ``bin/benchmark.ovstage`` (Linux), ``bin\benchmark.ovstage.exe`` (Windows)
   * - Python wheel
     - ``ovstage/bin/benchmark.ovstage`` (Linux), ``ovstage\bin\benchmark.ovstage.exe`` (Windows), under ``site-packages``

``benchmark.ovstage`` ships for Linux x86_64 and Windows x86_64. It is not included for
Linux aarch64 at this time.

In the commands below, ``<ovstage-package>`` is the extracted C package or, for the wheel,
the ``ovstage`` directory under ``site-packages``.

Running
-------

On Linux, run it directly. It finds the libraries it needs inside the package, with no
environment setup:

.. code-block:: bash

   <ovstage-package>/bin/benchmark.ovstage --benchmark_list_tests

On Windows, put the package ``bin\``, ``bin\plugins\`` and each immediate subdirectory of
``bin\plugins\`` on ``PATH`` first. ``ovstage.dll`` depends on libraries in the plugin
directories, and without them on ``PATH`` the executable exits before printing anything:

.. code-block:: powershell

   $pluginDirs = Get-ChildItem -Directory "<ovstage-package>\bin\plugins" | ForEach-Object { $_.FullName }
   $env:PATH = (@("<ovstage-package>\bin", "<ovstage-package>\bin\plugins") + $pluginDirs + $env:PATH) -join ";"
   benchmark.ovstage.exe --benchmark_list_tests

Benchmarks with ``Gpu`` in their name use CUDA source tensors and need a CUDA-capable GPU.

Selecting and Running Benchmarks
--------------------------------

``benchmark.ovstage`` accepts the standard Google Benchmark command-line options. The most
useful are:

.. list-table::
   :header-rows: 1

   * - Option
     - Effect
   * - ``--benchmark_list_tests``
     - Print every benchmark instance without running it.
   * - ``--benchmark_filter=<regex>``
     - Run only the instances whose names match the regular expression. A leading ``-``
       excludes the matches instead.
   * - ``--benchmark_dry_run=true``
     - Run each instance for a single iteration, to check that it runs.
   * - ``--benchmark_repetitions=<n>``
     - Repeat each instance ``n`` times and report aggregates.
   * - ``--benchmark_out=<file>`` / ``--benchmark_out_format=json``
     - Also write the results to a file.

For example, to run one family for a single iteration:

.. code-block:: bash

   <ovstage-package>/bin/benchmark.ovstage --benchmark_filter='^BM_PathDict_CreateDestroyChurn(/|$)' --benchmark_dry_run=true

Benchmark Names
---------------

Each name is a family followed by its parameters, for example
``BM_OVStage_LargeLoadedStage_SparseUpdate_AllFloor/stage_prims:1000000/updated_prims:500/...``.
The families fall into three groups:

.. list-table::
   :header-rows: 1

   * - Prefix
     - Measures
   * - ``BM_OVStage_``, other than ``BM_OVStage_IsaacLab_``
     - Individual operations: writes, reads, queries, map and unmap, cloning, and concurrent use.
   * - ``BM_OVStage_IsaacLab_``
     - Transform workloads modeled on Isaac Lab simulation steps.
   * - ``BM_PathDict_``
     - Path dictionary operations.

To run every benchmark except the Isaac Lab workloads, use
``--benchmark_filter=-BM_OVStage_IsaacLab_``.
