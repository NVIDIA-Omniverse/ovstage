.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Set Up a Python Project
=======================

The ``ovstage`` Python package binds the ovstage C data plane through ctypes. Its
shared loader must be discoverable when the first native operation runs. Programs use ``Stage`` and
:class:`PathDictionary <ovstage.PathDictionary>` (both context managers) plus ``numpy`` for tensor attribute
I/O.

.. note::

   ovstage is pre-release. The examples consume ``ovstage`` as a published wheel
   using `uv <https://docs.astral.sh/uv/>`__; if the pinned wheel cannot be
   resolved, the public wheel is not published yet.

Requirements
------------

- **Python 3.10–3.14** (``requires-python = ">=3.10,<3.15"``). Recreate the
  environment with a supported interpreter rather than editing the constraint.
- **NumPy**, for tensor I/O (:meth:`write_attribute(tensors=...) <ovstage.Stage.write_attribute>`, :meth:`column.array(...) <ovstage.ReadColumn.array>`):

  .. code-block:: bash

     pip install numpy

Making ``ovstage`` Importable
-----------------------------

Depend on the published ``ovstage`` wheel — for example a ``uv`` project that pins
it in ``pyproject.toml`` (the pattern the ``examples/python`` projects use), then
run with ``uv run``. Alternatively, put the ``ovstage`` package on ``PYTHONPATH``
directly.

Loading the Shared Library
--------------------------

The bindings open the ovstage shared loader (``ovstage-dynamic.dll`` on Windows,
``libovstage-dynamic.so`` on Linux) the first time you construct a ``Stage`` or
:class:`PathDictionary <ovstage.PathDictionary>`. The loader opens the ovstage runtime on the first
initialize/create call. The wheel bundles both and its ``<package>/bin`` layout
is searched automatically, so no extra
setup is needed. If you are instead running against a locally built shared
library, make it discoverable:

.. code-block:: bash

   # Linux
   export LD_LIBRARY_PATH=/path/to/ovstage/lib:$LD_LIBRARY_PATH
   # Windows (PowerShell)
   #   $env:PATH = "C:\path\to\ovstage\bin;$env:PATH"

Alternatively, set the ``OVSTAGE_LIBRARY_PATH_HINT`` environment variable.

Configuring Transform Updates
-----------------------------

Pass a :py:class:`~ovstage.StageConfig` when creating the stage to select the
hierarchy computation model used for automatic transform updates:

.. filtered-literalinclude:: ../../tests/python/test_config.py
   :language: python
   :start-after: # [snippet:configure-transform-updates]
   :end-before: # [/snippet:configure-transform-updates]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

The setting is process-scoped. Configured stages may coexist when their
concrete settings match; creating a stage with a conflicting setting while
another stage remains live raises :py:class:`~ovstage.OvstageError`.
``RUNTIME_DEFAULT`` is meaningful as a manual selector. As a configured value,
it does not override the active process default; a fresh process defaults to
CPU incremental.

``stage.config`` returns a detached copy of the process configuration captured
by that stage. It contains the explicit entries from the initialization that
established the active process state; it does not materialize omitted defaults.

Configuring Ordinal Admission
-----------------------------

Use :py:class:`~ovstage.OrdinalAdmissionPolicy` to choose how mutations are
checked against retained value and prim-lifecycle changes:

.. filtered-literalinclude:: ../../tests/python/test_config.py
   :language: python
   :start-after: # [snippet:configure-ordinal-admission]
   :end-before: # [/snippet:configure-ordinal-admission]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

The default is ``NONDECREASING``, which permits repeated writes
at the same unsealed ordinal, while ``FLOOR_ONLY`` skips retained-history
conflicts. See :ref:`ordinal-admission-policies` for the comparison rules.

This setting shares the process lifetime of the hierarchy configuration.
Omission, ``None`` and ``DEFAULT`` inherit the active policy; they select the
nondecreasing policy only when starting a fresh lifetime. Matching concrete
configurations may coexist. A conflicting policy raises
:py:class:`~ovstage.OvstageError` without acquiring an initialization reference
or changing either setting. Close all stages and release any other initialization
references and retained instance owners before selecting another policy.
``stage.config`` reports the original explicit entries, so an omitted policy
remains ``None`` and an explicit ``DEFAULT`` remains ``DEFAULT``.

.. _configuring-population-threads-python:

Configuring Population Threads
------------------------------

Population reads what its selectors select serially by default. Set
:py:attr:`~ovstage.StageConfig.population_thread_limit` to let a population
operation use more threads for those reads:

.. filtered-literalinclude:: ../../tests/python/test_config.py
   :language: python
   :start-after: # [snippet:configure-population-threads]
   :end-before: # [/snippet:configure-population-threads]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

The limit counts the thread that runs the operation: ``1`` is serial, ``N``
allows up to ``N`` threads, and :py:data:`~ovstage.POPULATION_THREAD_LIMIT_AUTO`
lets the runtime choose, up to the concurrency the process allows. The runtime
may use fewer threads. Populated data is the same for every limit.

The limit does not cover the rest of a population operation. Opening and
composing the USD stage and populating the rendering domain keep their own
concurrency, so a limit of ``1`` does not make a whole population operation
single-threaded. Which population work the limit covers may grow between
releases.

The limit shares the process lifetime of the settings above. ``None`` and ``0``
keep the active limit; a different limit raises :py:class:`~ovstage.OvstageError`
until every stage and initialization reference is released.

.. _selecting-the-implementation-python:

Selecting the Implementation
----------------------------

.. warning::

   History support is **experimental**. The ``history_selective``
   implementation, the ``ovstage.history.v1`` selection extension behind
   :py:meth:`~ovstage.Stage.enable_history` and the
   :py:attr:`~ovstage.StageConfig.retention_window` setting can change between
   releases without a deprecation period. The library does not guarantee
   source compatibility, ABI or semantics. Isolate their use behind an
   application adapter. Check availability at run time.

A stage is an instance of the implementation that
:py:attr:`~ovstage.StageConfig.implementation` selects. Each implementation
of the library has a name and documents it. ``None`` or an empty name selects
the default implementation, ``history_latest``.

``history_latest`` is the default. History is off: the stage keeps the latest
value of every cell, and a read returns it.
:py:meth:`~ovstage.Stage.enable_history` raises
:py:class:`~ovstage.OvstageError` with ``NOT_SUPPORTED`` on such a stage. Set
the field to select it by name:

.. code-block:: python

   import ovstage

   # Select the default implementation for this stage by name. History is off.
   config = ovstage.StageConfig(implementation="history_latest")
   with ovstage.Stage("simulation", config=config) as stage:
       assert stage.config.implementation == "history_latest"

:py:attr:`~ovstage.Stage.config` works on every stage and reports the name
the stage was created with.

``history_selective`` turns history on for the pairs the application selects.
:py:meth:`~ovstage.Stage.enable_history` selects the ``(prim, attribute)``
pairs that retain their earlier sealed values.
:py:attr:`~ovstage.StageConfig.retention_window`
(``OVSTAGE_CONFIG_RETENTION_WINDOW``, default 4) bounds the number of sealed
ordinals each selected pair retains. Every other cell keeps the latest value.
The stage serves the history extension ``ovstage.history.v1``. It does not
serve the experimental device placement extension: the ``placement`` inputs
raise :py:class:`~ovstage.OvstageError` on it. Every other operation runs as
on a stage of ``history_latest``. See :ref:`ovstage-selective-history` for the
complete sequence and for the recovery after a failed selection:

.. code-block:: python

   config = ovstage.StageConfig(implementation="history_selective", retention_window=8)
   with ovstage.Stage("simulation", config=config) as stage:
       assert stage.config.implementation == "history_selective"
       # ... select pairs with stage.enable_history(query, attributes) ...

The selection is per stage. Stages of different implementations share one
process, so a stage of ``history_selective`` and a stage of ``history_latest``
can be open together. A value that is not a string or ``None`` raises
:py:class:`TypeError`. A name that no implementation of the library has raises
:py:class:`~ovstage.OvstageError` with ``INVALID_ARGUMENT``, and no stage is
created. The retention window shares the process lifetime of the settings
above.

Minimal Program
---------------

.. filtered-literalinclude:: ../../examples/python/minimal/main.py
   :language: python
   :start-after: # [snippet:minimal-write-read]
   :end-before: # [/snippet:minimal-write-read]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

Where to Go Next
----------------

- :doc:`/python_api/getting_started` — run the minimal example.
- :doc:`/python_api/index` — the full Python API reference.
- :doc:`/concepts/application_flow` — the end-to-end lifecycle.
