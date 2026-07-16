.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Getting Started in Python
=========================

.. note::

   The ``ovstage`` Python bindings are a ctypes layer over the ovstage C ABI. The minimal
   example runs against the released ``ovstage`` wheel using `uv
   <https://docs.astral.sh/uv/>`__. ovstage is pre-release: if ``uv`` cannot resolve the
   pinned ``ovstage`` wheel, the public wheel is not published yet.

The minimal example is a ``uv`` project that pins the ``ovstage`` wheel in its
``pyproject.toml``:

.. code-block:: bash

   cd examples/python/minimal
   uv run main.py

The bindings load the ovstage shared library through ctypes the first time you create a
:py:class:`~ovstage.Stage` or :py:class:`~ovstage.PathDictionary`. If you are running
against a locally built shared library instead of the wheel, put its directory on the
loader path (``LD_LIBRARY_PATH`` on Linux, ``PATH`` on Windows) or point the loader at it
with ``OVSTAGE_LIBRARY_PATH_HINT``.

Minimal Example
---------------

The minimal example creates a stage, interns paths and tokens through the
instance-owned path dictionary, writes an attribute column, advances the write floor,
and reads the latest committed data back.

.. filtered-literalinclude:: ../../examples/python/minimal/main.py
   :language: python
   :start-after: # [snippet:minimal-write-read]
   :end-before: # [/snippet:minimal-write-read]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

The example above is provided as a Python project in the ``examples/python/minimal``
directory in `the repository <https://github.com/NVIDIA-Omniverse/ovstage>`__.

Next Steps
----------

* Explore the :doc:`../examples/index`.
* Refer to the :doc:`index` for the full Python API reference.
