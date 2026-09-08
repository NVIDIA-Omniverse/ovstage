.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Examples
========

Runnable example projects live under ``examples/`` in
`the repository <https://github.com/NVIDIA-Omniverse/ovstage>`__, in both C and Python.
Each example is self-contained and is the source of truth for the code snippets
referenced by the ovstage skills.

.. grid:: 1 2 2 2
   :gutter: 3

   .. grid-item-card:: Minimal :bdg-primary:`C`
      :class-card: sd-shadow-sm

      Create an instance, obtain its path dictionary, intern paths and tokens, write
      an attribute column, advance the write floor, and read it back.

      +++

      .. rst-class:: sd-text-secondary sd-fs-6

      ``examples/c/minimal``

      :doc:`Open the walkthrough → <../c_api/getting_started>`

   .. grid-item-card:: Minimal :bdg-primary:`Python`
      :class-card: sd-shadow-sm

      The Python equivalent of the C minimal example, using the ctypes bindings.

      +++

      .. rst-class:: sd-text-secondary sd-fs-6

      ``examples/python/minimal``

      :doc:`Open the walkthrough → <../python_api/getting_started>`

   .. grid-item-card:: Runtime Loop :bdg-primary:`C`
      :class-card: sd-shadow-sm

      A minimal per-frame runtime loop: populate a USD stage, then advance ordinals
      and exchange changed data across simulation steps.

      +++

      .. rst-class:: sd-text-secondary sd-fs-6

      ``examples/c/runtime-loop``

      :doc:`Read the guide → <../guides/runtime_loop>`

   .. grid-item-card:: Runtime Loop :bdg-primary:`Python`
      :class-card: sd-shadow-sm

      A per-frame runtime loop driven from Python.

      +++

      .. rst-class:: sd-text-secondary sd-fs-6

      ``examples/python/runtime-loop``

      :doc:`Read the guide → <../guides/runtime_loop>`

   .. grid-item-card:: Authoring Hierarchy :bdg-primary:`C`
      :class-card: sd-shadow-sm

      Author and clone a hierarchy without USD, then explicitly derive world
      transforms with a supported GPU computation model.

      +++

      .. rst-class:: sd-text-secondary sd-fs-6

      ``examples/c/authoring-hierarchy``

      :doc:`Read the transform contract → <../scene/transforms>`

   .. grid-item-card:: Authoring Hierarchy :bdg-primary:`Python`
      :class-card: sd-shadow-sm

      The Python equivalent, including absent and stale derived-world rows
      before hierarchy computation.

      +++

      .. rst-class:: sd-text-secondary sd-fs-6

      ``examples/python/authoring-hierarchy``

      :doc:`Read the transform contract → <../scene/transforms>`

   .. grid-item-card:: USD Export :bdg-primary:`C`
      :class-card: sd-shadow-sm

      Populate ovstage from USD, select ``/World/Props`` with a shared
      population predicate, and export its typed hierarchy to a one-shot saved
      file through the public C ABI without rule tables.

      +++

      .. rst-class:: sd-text-secondary sd-fs-6

      ``examples/c/usd-export``

      :doc:`Read the guide → <../scene/exporting_to_usd>`

   .. grid-item-card:: USD Export :bdg-primary:`Python`
      :class-card: sd-shadow-sm

      Populate ovstage from USD and export the typed ``/World/Props`` hierarchy
      with the metadata-driven helper, preserving recorded prim types, applied
      schemas, and supported schema-declared attributes in a one-shot file.

      +++

      .. rst-class:: sd-text-secondary sd-fs-6

      ``examples/python/usd-export``

      :doc:`Read the guide → <../scene/exporting_to_usd>`
