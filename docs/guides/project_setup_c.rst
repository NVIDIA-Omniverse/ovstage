.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Set Up a C/C++ Project
======================

ovstage exposes a pure **C API**. A C/C++ consumer includes
``<ovstage/ovstage.h>`` for the data plane, adds ovstage's ``include/`` directory
to its include path, and links the ovstage shared library.

.. note::

   The examples build standalone with CMake against the released ovstage package
   using ``find_package(ovstage)`` (refer to ``examples/c/cmake/ovstage.cmake``). ovstage
   is pre-release; if the pinned package cannot be fetched, the public package is
   not published yet. Refer to :doc:`/c_api/getting_started`.

Headers
-------

.. list-table::
   :header-rows: 1

   * - Include
     - Provides
   * - ``<ovstage/ovstage.h>``
     - The data-plane API, ``ovstage_get_path_dictionary``, and the path-dictionary / DLPack **types**.
   * - ``<ovx/path_dictionary/path_dictionary.h>`` + ``path_dictionary_utils.h``
     - The path-dictionary **functions** (interning tokens, building path lists). Include these directly to *call* them.
   * - ``<dlpack/dlpack.h>``
     - The ``DLTensor`` types for attribute data.

Build Wiring
------------

The public examples wire up ovstage with a small CMake helper,
``examples/c/cmake/ovstage.cmake``:

.. code-block:: cmake

   list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")
   include(ovstage)
   ovstage_fetch()                 # find_package(ovstage), else fetch the release

   add_executable(myapp main.cpp)
   target_link_libraries(myapp PRIVATE ovstage::ovstage)
   ovstage_setup_runtime(myapp)    # rpath (Linux) / package bin/ on PATH (Windows)

Linking ``ovstage::ovstage`` brings in the public ``include/`` tree, so
``#include <ovstage/ovstage.h>`` resolves with no extra include paths. Then follow
the minimal example's lifecycle: create → get path dictionary → intern / build
path list → query → write → advance write floor → read → release. Refer to
:doc:`/concepts/application_flow`.

Diagnostics Caveat
------------------

``ovstage_get_error_string`` and ``ovstage_get_last_op_error`` are
vtable-dispatched and need a live ``ovstage_instance_t*``. Before
``ovstage_create_instance`` returns, you can only print the numeric status code
or use the free function ``ovstage_get_last_error()`` (refer to
:doc:`/concepts/error_handling`).

Static Loader Package Root
--------------------------

When linking ``ovstage::ovstage_static``, pass the package ``bin/`` directory via
:c:func:`ovstage_config_entry_binary_package_root_path` to ``ovstage_initialize()``
before any other ``ovstage_*`` call. The path may include
:c:macro:`OVX_CONFIG_EXECUTABLE_DIR_TOKEN` (``"${executable_dir}"``), which the
loader substitutes with the absolute directory of the running executable — for
example, when the package lives at ``<exe_dir>/ovstage/bin``:

.. code-block:: c

   ovstage_config_entry_t entries[] = {
       ovstage_config_entry_binary_package_root_path(
           literal_to_ovx_string(OVX_CONFIG_EXECUTABLE_DIR_TOKEN "/ovstage/bin")),
   };
   ovstage_config_t config = { entries, 1 };
   ovstage_initialize(&config);

Minimal Program
---------------

.. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
   :language: cpp
   :start-after: // [snippet:minimal-write-read]
   :end-before: // [/snippet:minimal-write-read]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

Where to Go Next
----------------

- :doc:`/c_api/getting_started` — build and run the minimal example.
- :doc:`/c_api/index` — the full C API reference.
- :doc:`/concepts/application_flow` — the end-to-end lifecycle.
