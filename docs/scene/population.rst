.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Population (USD → ovstage)
==========================

The population API (``ovstage_population.h``) is the bridge that ingests composed
USD content into the runtime stage. It is a self-contained C API with its own
asynchronous model — ``ovstage_population_enqueue_result_t`` and
``ovstage_population_wait_op`` — that runs parallel to the data-plane
:doc:`submit/observe model </concepts/async_model>`.

.. note::

   These entry points are **pre-release and provisional** ("Draft — API in
   flux").

Loading a Scene
---------------

``ovstage_population_open_usd_from_file`` (and ``_from_string``) loads a USD
scene into the runtime table in one op at a chosen ordinal. As with any write,
seal the ordinal with ``advance_write_floor`` before reading the populated prims
back:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/runtime-loop/main.py
         :language: python
         :start-after: # [snippet:populate]
         :end-before: # [/snippet:populate]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/runtime-loop/main.cpp
         :language: cpp
         :start-after: // [snippet:populate]
         :end-before: // [/snippet:populate]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Population Domains
------------------

Loads take a population **domain** bitmask (``PopulationDomain`` /
``ovstage_population_domain_t``: ``NONE``, ``RENDERING``, ``PHYSICS``, ``ALL``)
selecting which consumer's attribute set to populate.

Editing the USD Source
----------------------

After a scene is live you can edit the USD source and propagate the change into
the runtime table at a fresh ordinal:

- ``ovstage_population_add_usd_reference_from_file`` / ``_from_string`` — add a
  reference onto an existing prim.
- ``ovstage_population_remove_usd_reference`` / ``ovstage_population_reset_usd`` —
  remove a reference or reset.
- ``ovstage_population_apply_usd_changes`` — propagate pending USD edits into the
  table (at a new ordinal).
- ``ovstage_population_apply_usd_time`` — apply a USD time sample.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/runtime-loop/main.py
         :language: python
         :start-after: # [snippet:update-usd]
         :end-before: # [/snippet:update-usd]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/runtime-loop/main.cpp
         :language: cpp
         :start-after: // [snippet:update-usd]
         :end-before: // [/snippet:update-usd]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Python ↔ C Name Mapping
------------------------

Several Python wrappers use shorter names than their C entry points:

.. list-table::
   :header-rows: 1

   * - Python (``ovstage.population``)
     - C entry point
   * - ``open_usd`` / ``open_usd_async``
     - ``ovstage_population_open_usd_from_file``
   * - ``add_usd_reference``
     - ``ovstage_population_add_usd_reference_from_file``
   * - ``remove_usd``
     - ``ovstage_population_remove_usd_reference``
   * - ``update_from_usd_time``
     - ``ovstage_population_apply_usd_time``

The ``time_code`` parameter on ``open_usd*`` / ``update_from_usd_time`` is in
**seconds** — it maps to the C ``time`` parameter, converted internally via the
stage's ``timeCodesPerSecond``. ``math.nan`` (the ``open_usd*`` default)
evaluates at USD's Default time code.

Ordinal Ownership
-----------------

Population never opens or seals an ordinal for you — the application owns the
ordinal lifecycle. Pass the current ordinal to each call and seal each tick with
``advance_write_floor``. Keep ordinals monotonic (populate at 1, then writes and
USD edits at higher ordinals); an operation at or below the floor is a write-floor
violation.

Where to Go Next
----------------

- :doc:`/guides/runtime_loop` — the full populate → read → update → read loop.
- :doc:`reading_attributes` — reading ``usd-prim-type`` and other populated metadata back.
- :doc:`instancing` — discovering instance and prototype roots in populated content.
