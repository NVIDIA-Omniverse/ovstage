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

ovstage is a standalone C and Python library that gives simulation and visualization applications one shared, in-process store for scene and simulation data. Applications can author data directly or populate it from OpenUSD, then let physics, rendering, sensors, and other consumers query, read, and update that data through a common API. The application controls execution and when updates become visible, while ovstage provides bulk data access, shared scene identities, and change tracking so each consumer can read the state it needs.

Reads and maps use CPU buffers by default. The experimental
:ref:`device-placement extension <ovstage-device-placement>` also supports CUDA
buffers; reading a copy already current on the requested device avoids migration.

In this documentation you will find getting started guides for C and Python, API
references, and example projects.

* :doc:`Start here: ovstage overview <concepts/overview>` — the data model, ordinals, watermarks, and reads.
* :doc:`Getting started in C <c_api/getting_started>` — the C API bring-up path.
* :doc:`Getting started in Python <python_api/getting_started>` — the ctypes bindings over the same C API.
* :doc:`Upgrading from 0.3 <guides/upgrading_from_0_3>` — what changed in 0.4 and how to migrate.
* :doc:`Upgrading from 0.2 <guides/upgrading_from_0_2>` — what changed in 0.3 and how to migrate.
* :doc:`Limitations <concepts/limitations>` — current behavior that may change before GA.

.. note::

   ovstage is currently **pre-release** software. The API, runtime behavior, and
   packaging can change before GA.

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

   concepts/overview
   concepts/application_flow
   concepts/async_model
   concepts/choosing_api
   concepts/error_handling
   concepts/threading_lifetime
   concepts/limitations
   concepts/string_handling
   concepts/dlpack_tensors

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: The Runtime Stage

   scene/population
   scene/path_dictionary
   scene/queries
   scene/writing_attributes
   scene/transforms
   scene/reading_attributes
   scene/device_placement
   scene/cloning
   scene/exporting_to_usd
   scene/instancing

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: Guides and Examples

   guides/runtime_loop
   guides/upgrading_from_0_3
   guides/upgrading_from_0_2
   guides/benchmarking
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
