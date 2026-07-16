.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Instancing Queries
==================

``ovstage_instancing.h`` provides high-level scene-graph instancing queries over
populated content.

.. note::

   **Draft — API in flux.** Instancing queries run over populated content
   (see :doc:`population`). This surface is provisional and can change.

C API
-----

The C header exposes three functions:

.. list-table::
   :header-rows: 1

   * - Function
     - Takes
     - Returns
   * - ``ovstage_instancing_get_instance_roots``
     - A prototype-root prim path
     - The instance roots that reference that prototype — a caller-owned ``ovx_primpath_list_t``.
   * - ``ovstage_instancing_get_prototype_root``
     - An instance-root prim path
     - The prototype root for that instance — a single ``ovx_primpath_t``.
   * - ``ovstage_instancing_get_prototype_roots``
     - (the stage)
     - All prototype roots — a caller-owned ``ovx_primpath_list_t``.

The two ``*_roots`` functions return an ``ovx_primpath_list_t`` **owned by the
caller**: release it through the :doc:`path dictionary </scene/path_dictionary>`
when you are done (the same refcount rule as any ``ovx_primpath_list_t`` you
obtain). ``ovstage_instancing_get_prototype_root`` instead returns a single
``ovx_primpath_t``, a dictionary-lifetime handle that does **not** require
per-result release.

Python API
----------

The ``ovstage.instancing`` module exposes the same three synchronous queries as
``get_prototype_roots``, ``get_prototype_root``, and ``get_instance_roots``.
It accepts and returns path strings; returned native path lists are converted
and released before each function returns, so Python callers do not manage
path-list references.

.. filtered-literalinclude:: ../../examples/python/queries/main.py
   :language: python
   :start-after: # [snippet:instancing-queries]
   :end-before: # [/snippet:instancing-queries]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

For the full C signatures, see :doc:`/c_api/index`; the Python callables are
listed in :doc:`/python_api/index`.

Where to Go Next
----------------

- :doc:`population` — populate USD content before querying instances.
- :doc:`/scene/path_dictionary` — releasing the returned path lists.
