.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Queries
=======

A query selects the set of prims that subsequent reads, writes, and maps operate
over. The simplest query is built directly from an interned prim-path list;
queries resolve against the **latest committed** state.

Query From a Path List
----------------------

Build an immutable prim-path list through the
:doc:`path dictionary </scene/path_dictionary>`, then open a query over it:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:path-list-query]
         :end-before: # [/snippet:path-list-query]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:path-list-query]
         :end-before: // [/snippet:path-list-query]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

The resulting query handle is what you pass to
:doc:`writing_attributes`, :doc:`reading_attributes`, and the map path.

Filtering by Predicate
----------------------

Queries can be narrowed with predicates. An ``ovstage_predicate_t`` names an
attribute (as ``ovx_string_or_token_t``), a comparison ``op``
(``ovstage_filter_op_t``), and one or more values; an ``ovstage_filter_t`` is a
set of predicates. The built-in metadata attributes are filterable, so you can
select, for example, all prims of a given ``usd-prim-type`` or under a given
``usd-parent``. Refer to :doc:`/c_api/index` for the predicate and filter type
signatures.

Built-in Metadata Attributes
----------------------------

The following reserved attributes are auto-maintained and usable both in reads
and in filter predicates:

.. list-table::
   :header-rows: 1

   * - Attribute
     - Contents
   * - ``usd-path``
     - The prim's path.
   * - ``usd-prim-type``
     - The prim's type name.
   * - ``usd-schemas``
     - Applied API schemas.
   * - ``usd-parent``
     - The parent prim.
   * - ``usd-children``
     - The child prims.
   * - ``usd-active``
     - Whether the prim is active. **Not supported** — a live prim is always
       active, so predicates naming it return ``NOT_SUPPORTED``; subject to
       removal in a future release.

``usd-path`` PREFIX Convention
------------------------------

``PREFIX`` is **byte-prefix** matching on the path string. A value without a
trailing ``/`` can match unrelated paths that share the same opening bytes — for
example, ``PREFIX "/World"`` also matches ``/Worldwide``. Append a trailing
``/`` to scope to the subtree under that path: ``PREFIX "/World/"`` matches only
paths under ``/World/``.

Where to Go Next
----------------

- :doc:`reading_attributes` — read columns over a query.
- :doc:`writing_attributes` — write columns over a query.
- :doc:`/scene/path_dictionary` — build the path list a query is opened from.
