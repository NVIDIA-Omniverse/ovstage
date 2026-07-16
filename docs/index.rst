.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

NVIDIA ovstage
==============

**ovstage** is a vectorized, GPU-native runtime stage for OpenUSD data — a shared,
high-performance data substrate for OV Libraries spanning physics, rendering, sensors,
animation, and graph. It provides a unified **C API** for reading, writing, querying, and
managing simulation data such as transforms, velocities, materials, hierarchy, and
metadata across CPU and GPU memory, with zero-copy data paths and DLPack tensor
interchange.

In this documentation you will find getting started guides for C and Python, API
references, and example projects.

* :doc:`Getting started in C <c_api/getting_started>` — the validated bring-up path today.
* :doc:`Getting started in Python <python_api/getting_started>` — the ctypes bindings over the same C API.

.. note::

   ovstage is currently **pre-release** software. The API, runtime behavior, and
   packaging can change. Standalone public packages, prebuilt binaries, and Python
   wheels are still in progress.

Execution Model
---------------

ovstage uses an asynchronous, **ordinal-keyed** submit/observe model:

* **Enqueue (synchronous).** State-mutating and data-producing calls return an
  ``ovstage_enqueue_result_t`` (status + ``op_index``) immediately; the work is queued
  and runs later.
* **Ordinal-keyed write ordering.** Writes, deletes, and map commits carry an explicit
  ``ordinal``. Same-ordinal ops run in submission order; different-ordinal ops are
  independent and can run concurrently.
* **Reads and queries are independent.** Reads target sealed data at or below the
  immutable write floor. Queries resolve against the latest committed state.
* **Zero-copy by default**, with DLPack ``DLTensor`` for tensor interchange.

Support
-------

Report documentation issues and runtime issues through the
`NVIDIA Omniverse developer forum <https://forums.developer.nvidia.com/c/omniverse/300>`_.

License
-------

The software and materials are governed by the `NVIDIA Software License Agreement <https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-software-license-agreement/>`_ and the `Product Specific Terms for NVIDIA AI Products <https://www.nvidia.com/en-us/agreements/enterprise-software/product-specific-terms-for-ai-products/>`_.

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: Getting Started

   c_api/getting_started
   python_api/getting_started
   guides/project_setup_c
   guides/project_setup_python

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: Concepts

   concepts/application_flow
   concepts/async_model
   concepts/error_handling
   concepts/string_handling
   concepts/dlpack_tensors

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: Scene Data

   scene/path_dictionary
   scene/queries
   scene/writing_attributes
   scene/reading_attributes
   scene/cloning
   scene/population
   scene/instancing

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: Guides and Examples

   guides/runtime_loop
   examples/index

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: C API

   c_api/index

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: Python API

   python_api/index

.. toctree::
   :hidden:
   :caption: Links

   GitHub Repository <https://github.com/NVIDIA-Omniverse/ovstage>

Indices and Tables
==================

* :ref:`genindex`
* :ref:`search`
