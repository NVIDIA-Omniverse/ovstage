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

The ``ovstage`` Python package binds the ovstage C data plane through ctypes, so the
ovstage shared library must be loadable at import time. Programs use ``Stage`` and
``PathDictionary`` (both context managers) plus ``numpy`` for tensor attribute
I/O.

.. note::

   ovstage is pre-release. The examples consume ``ovstage`` as a published wheel
   using `uv <https://docs.astral.sh/uv/>`__; if the pinned wheel cannot be
   resolved, the public wheel is not published yet.

Requirements
------------

- **Python 3.10–3.13** (``requires-python = ">=3.10,<3.14"``). Recreate the
  environment with a supported interpreter rather than editing the constraint.
- **NumPy**, for tensor I/O (``write_attribute(tensors=...)``, ``group.array(...)``):

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

The bindings load the ovstage shared library (``ovstage.dll`` on Windows) the
first time you construct a ``Stage`` or ``PathDictionary``. The wheel bundles the
library and its ``<package>/bin`` layout is searched automatically, so no extra
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
