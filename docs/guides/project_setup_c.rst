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
``<ovstage/ovstage.h>`` for the data plane and links ``ovstage::ovstage`` or
``ovstage::ovstage_static``; either CMake target provides the public include path.

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
     - The data-plane API, :c:func:`ovstage_get_path_dictionary <ovstage_get_path_dictionary>`, and the path-dictionary / DLPack **types**.
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

``ovstage::ovstage`` is the shared forwarding loader. The operating system
loads it with the application; it opens the ovstage runtime on the first
initialize/create call. ``ovstage::ovstage_static`` provides the same forwarding
behavior as a static library. Linking either target brings in the public
``include/`` tree, so
``#include <ovstage/ovstage.h>`` resolves with no extra include paths. Then follow
the minimal example's lifecycle: create → get path dictionary → intern / build
path list → query → write → advance write floor → read → release. Refer to
:doc:`/concepts/application_flow`.

Keep the complete package ``bin/`` tree, including its ``plugins/``
subdirectories, together when deploying or relocating an application.
Both public loaders use this same layout.

Linux Toolchain Requirements
----------------------------

The Linux packages target the ``manylinux_2_35`` baseline (glibc 2.35+, e.g.
Ubuntu 22.04). Linking an application against the SDK makes the linker walk the
pre-packaged runtime's dependencies, which resolve a few libraries from the
host system: ``libX11.so.6``, ``libGL.so.1``, and ``libgomp.so.1``
(Debian/Ubuntu packages ``libx11-6``, ``libgl1``, ``libgomp1``). Install those
before building, even if your own code never touches X11 or OpenGL.

The build must run with the distribution's binutils (``as``, ``ld``). GCC
locates both via ``PATH``, so a Homebrew/Linuxbrew installation that has
linked its own ``binutils`` (Homebrew's default on Linux since June 2026)
silently replaces the system tools — even when CMake selected
``/usr/bin/c++``. Depending on the Homebrew and host glibc versions, the build
then fails at compile time (``as: ... version 'GLIBC_2.xx' not found``) or at
link time with unresolved X11/GL symbols or ``GLIBC_2.xx`` version errors
reported against the pre-packaged runtime libraries. Remedies, in preference
order:

* ``brew unlink binutils``, or
* remove the Homebrew ``bin`` directory from ``PATH`` for the build shell, or
* add ``-B/usr/bin`` to the compiler flags (forces GCC to use the system
  binutils for this build only).

``find_package(ovstage)`` performs a configure-time check and warns when it
detects a Homebrew linker; pass ``-DOVSTAGE_SKIP_LINKER_CHECK=ON`` to CMake
(or set that environment variable) to silence it if you are deliberately
using a custom toolchain.

Diagnostics Caveat
------------------

:c:func:`ovstage_get_error_string <ovstage_get_error_string>` and :c:func:`ovstage_get_last_op_error <ovstage_get_last_op_error>` are
vtable-dispatched and need a live ``ovstage_instance_t*``. Before
:c:func:`ovstage_create_instance <ovstage_create_instance>` returns, you can only print the numeric status code
or use the free function :c:func:`ovstage_get_last_error() <ovstage_get_last_error>` (refer to
:doc:`/concepts/error_handling`).

Static Loader Package Root
--------------------------

When linking ``ovstage::ovstage_static``, pass the package ``bin/`` directory via
:c:func:`ovstage_config_entry_binary_package_root_path` to :c:func:`ovstage_initialize() <ovstage_initialize>`
before any other ``ovstage_*`` call. The path may include
:c:macro:`OVX_CONFIG_EXECUTABLE_DIR_TOKEN` (``"${executable_dir}"``), which the
loader substitutes with the absolute directory of the running executable — for
example, when the package lives at ``<exe_dir>/ovstage/bin``:

.. code-block:: c

   ovstage_config_entry_t entries[] = {
       ovstage_config_entry_binary_package_root_path(
           literal_to_ovx_string(OVX_CONFIG_EXECUTABLE_DIR_TOKEN "/ovstage/bin")),
       ovstage_config_entry_runtime_default_hierarchy_computation_model(
           OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_INCREMENTAL),
   };
   ovstage_config_t config = { entries, 2 };
   ovstage_initialize(&config);
   /* ... create instances, use ovstage ... */
   ovstage_shutdown();

Balance every successful :c:func:`ovstage_initialize() <ovstage_initialize>` with exactly one :c:func:`ovstage_shutdown() <ovstage_shutdown>`. Destroying a stage does not replace its matching shutdown, and shutdown does not destroy the application's stages.

The runtime-default model is process-scoped. It drives automatic transform
updates for newly created instances and can be selected explicitly:

.. code-block:: c

   ovstage_compute_hierarchy(
       stage,
       OVSTAGE_HIERARCHY_COMPUTATION_MODEL_RUNTIME_DEFAULT,
       input_ordinal,
       output_ordinal);

Choose the concrete CPU or GPU model before the first successful :c:func:`ovstage_initialize` call or any stage creation, and pass it in that initialization call's configuration. A later initialization call must match the active model. Passing ``RUNTIME_DEFAULT`` as the configured value leaves the runtime default unmodified; that config entry is ignored.

After creating an instance, :c:func:`ovstage_get_config` returns a read-only view of the configured entries. It does not fill in omitted defaults. Use the view only while the instance remains valid.

.. _configuring-ordinal-admission-c:

Configuring Ordinal Admission
-----------------------------

Pass an entry from
:c:func:`ovstage_config_entry_runtime_default_ordinal_admission_policy` to
``ovstage_initialize()`` before creating the first instance. The
:c:type:`ovstage_ordinal_admission_policy_t` choices are described in
:ref:`ordinal-admission-policies`. The default is
``OVSTAGE_ORDINAL_ADMISSION_NONDECREASING``.

.. filtered-literalinclude:: ../../tests/c/test_support_api.cpp
   :language: cpp
   :start-after: // [snippet:configure-ordinal-admission-c]
   :end-before: // [/snippet:configure-ordinal-admission-c]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

The policy is process-scoped. Omission or ``OVSTAGE_ORDINAL_ADMISSION_DEFAULT``
inherits active settings; on the first process reference it selects the nondecreasing
default. A later concrete value must match. Conflicts in either hierarchy or
admission configuration reject the whole initialization without changing state
or acquiring a reference. Balance every successful initialization with
``ovstage_shutdown()`` and release all instances, including retained handles and
attachments, before choosing a different policy. The original explicit entries
remain available through ``ovstage_get_config()``; omitted defaults are not added.

.. _configuring-population-threads-c:

Configuring Population Threads
------------------------------

Population reads what its selectors select serially by default. Pass an entry
from :c:func:`ovstage_config_entry_population_thread_limit` to
``ovstage_initialize()`` before creating the first instance to let a population
operation use more threads for those reads:

.. filtered-literalinclude:: ../../tests/c/test_support_api.cpp
   :language: cpp
   :start-after: // [snippet:configure-population-threads-c]
   :end-before: // [/snippet:configure-population-threads-c]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

The limit counts the thread that runs the operation: ``1`` is serial, ``N``
allows up to ``N`` threads, and ``OVSTAGE_POPULATION_THREAD_LIMIT_AUTO`` lets the
runtime choose, up to the concurrency the process allows. The runtime may use
fewer threads. Populated data is the same for every limit.

The limit does not cover the rest of a population operation. Opening and
composing the USD stage and populating ``OVSTAGE_POPULATION_DOMAIN_RENDERING``
keep their own concurrency, so a limit of ``1`` does not make a whole
population operation single-threaded. Which population work the limit covers
may grow between releases.

The limit is process-scoped and follows the same lifetime rules as the settings
above. ``OVSTAGE_POPULATION_THREAD_LIMIT_DEFAULT`` (``0``) does not
override the active limit; a different limit is rejected until all
initialization and instance references are released.

.. _selecting-the-implementation-c:

Selecting the Implementation
----------------------------

.. warning::

   History support is **experimental**. The ``history_selective``
   implementation, the ``ovstage.history.v1`` selection extension of
   ``ovstage_history.h`` and the ``OVSTAGE_CONFIG_RETENTION_WINDOW`` setting
   can change between releases without a deprecation period. The library does
   not guarantee source compatibility, ABI or semantics. Isolate their use
   behind an application adapter. Check availability at run time.

``ovstage_create_instance()`` creates an instance of the implementation that
the ``implementation`` member of :c:type:`ovstage_instance_desc_t` selects.
Each implementation of the library has a name and documents it. ``NULL`` or
an empty string selects the default implementation, ``history_latest``.

``history_latest`` is the default. History is off: the instance keeps the
latest value of every cell, and a read returns it. The history selection calls
of ``ovstage_history.h`` report ``OVSTAGE_ERROR_NOT_SUPPORTED`` on such an
instance. Set the member to select it by name:

.. code-block:: c

   ovstage_instance_desc_t desc = { .name = "simulation",
                                    .implementation = "history_latest" };
   ovstage_instance_t* instance = NULL;
   ovstage_create_instance(&desc, &instance);
   /* ... history is off: every read returns the latest value ... */
   ovstage_destroy_instance(instance);

``history_selective`` turns history on for the pairs the application selects.
``ovstage_enable_history()`` (``ovstage_history.h``) selects the
``(prim, attribute)`` pairs that retain their earlier sealed values. The
process key ``OVSTAGE_CONFIG_RETENTION_WINDOW``
(:c:func:`ovstage_config_entry_retention_window`, default 4) bounds the number
of sealed ordinals each selected pair retains. Every other cell keeps the
latest value. The instance serves the history extension
``ovstage.history.v1``. It does not serve the experimental device placement
extension: ``ovstage_query_placement_extension()`` reports
``OVSTAGE_ERROR_NOT_FOUND`` on it. Every other operation runs as on an
instance of ``history_latest``. See :ref:`ovstage-selective-history` for the
complete sequence and for the recovery after a failed selection:

.. code-block:: c

   ovstage_config_entry_t entry = ovstage_config_entry_retention_window(8);
   ovstage_config_t config = { &entry, 1 };
   ovstage_initialize(&config);

   ovstage_instance_desc_t desc = { .name = "simulation",
                                    .implementation = "history_selective" };
   ovstage_instance_t* instance = NULL;
   ovstage_create_instance(&desc, &instance);
   /* ... select pairs with ovstage_enable_history, then destroy the instance ... */
   ovstage_shutdown();

The selection is per instance. Instances of different implementations share
one process, so an instance of ``history_selective`` and an instance of
``history_latest`` can be live together. A name that no implementation of the
library has is rejected with ``OVSTAGE_ERROR_INVALID_ARGUMENT``, and the call
creates no instance. ``ovstage_get_last_error()`` then reports the rejected
name. The process config that ``ovstage_get_config()`` returns holds no entry
for the implementation. The retention window is a process setting and follows
the lifetime rules of the settings above. The implementation name is an
instance setting: each ``ovstage_create_instance()`` call selects the
implementation of its own instance.

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
