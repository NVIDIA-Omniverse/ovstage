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

A query selects the :term:`prims <Prim>` that are affected by a read, write, or map operation.

First create a query by supplying explicit paths or finding prims that match
conditions. Pass that query to :meth:`stage.read_attributes() <ovstage.Stage.read_attributes>`,
:meth:`stage.write_attribute() <ovstage.Stage.write_attribute>`, or :meth:`stage.map_attribute() <ovstage.Stage.map_attribute>` to operate on those
prims.

You can call :meth:`query.result() <ovstage.Query.result>` after a :meth:`query.wait() <ovstage.Query.wait>` to
inspect the selection, such as its prim count.

Release the query when it is no longer needed.
Python's ``with`` statement handles this for you.

Creating a Query From a List of Paths
-------------------------------------

Use this when you already know the paths of the prims you want to read or write.
Store those paths in a list through the
:doc:`path dictionary </scene/path_dictionary>`, then pass the list to
:meth:`stage.query_from_path_list() <ovstage.Stage.query_from_path_list>`. This creates a query containing the supplied
paths without checking whether the prims exist.

These snippets create a path list and a query for use in later operations:

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

Creating a Query by Filtering Prims
-----------------------------------

Use a filter when you want ovstage to find prims that meet conditions, such as
having an attribute or belonging to a particular prim type. Pass a
:class:`ovstage.Filter` to :meth:`ovstage.Stage.query()`. ovstage checks prims
in the :term:`runtime stage <Runtime stage>` and includes those that match in
the resulting query.
Omitting the filter selects all prims.

Each :class:`ovstage.Predicate` describes one condition: an attribute to check,
a :class:`ovstage.FilterOp` operation, and any values needed by that operation.
All predicates in a filter must match. Built-in metadata attributes let you
check a prim's type (``usd-prim-type``), path (``usd-path``), or parent
(``usd-parent``).

Filtered queries find prims in the current scene state. They do not reconstruct
an earlier scene state from an :term:`ordinal <Ordinal>`.

Example: Find Mesh Prims in a Subtree
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Combine a type match with a path prefix. These snippets select ``Mesh`` prims
below ``/World/Group``. The trailing slash excludes the group itself and avoids
matching names such as ``/World/GroupExtra``:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/queries/main.py
         :language: python
         :snippets: query-filter-imports, query-type-subtree

   .. tab-item:: C

      The ``waitOp()`` helper waits, checks errors, and
      releases operation tracking; ``check()`` checks synchronous status codes.

      .. filtered-literalinclude:: ../../examples/c/queries/main.cpp
         :language: cpp
         :start-after: // [snippet:query-type-subtree-c]
         :end-before: // [/snippet:query-type-subtree-c]
         :dedent:

Common Patterns
---------------

Checking Whether a Prim Exists
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use ``FilterOp.IN`` on ``usd-path`` to match one exact path, then check whether
:meth:`query.result().total_prim_count <ovstage.Query.result>` is nonzero. Do not use
:meth:`query_from_path_list() <ovstage.Stage.query_from_path_list>` as an existence check: a path-list query can include
paths that do not exist yet.

These snippets check whether ``/World/Anchor`` exists:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/queries/main.py
         :language: python
         :start-after: # [snippet:query-prim-exists]
         :end-before: # [/snippet:query-prim-exists]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/queries/main.cpp
         :language: cpp
         :start-after: // [snippet:query-prim-exists-c]
         :end-before: // [/snippet:query-prim-exists-c]
         :dedent:

Checking Whether an Attribute Exists on a Prim
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Combine the exact path condition with ``FilterOp.HAS`` for the attribute.
Both conditions must match. ``HAS`` checks presence, not the attribute's value;
a value of zero or an empty array still counts as present.

These snippets check whether ``/World/Anchor`` has ``example:count``. The result
is false if either the prim or the attribute is absent:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/queries/main.py
         :language: python
         :start-after: # [snippet:query-attribute-exists]
         :end-before: # [/snippet:query-attribute-exists]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/queries/main.cpp
         :language: cpp
         :start-after: // [snippet:query-attribute-exists-c]
         :end-before: // [/snippet:query-attribute-exists-c]
         :dedent:

Both checks inspect current scene state and do not require a watermark advance.
Presence does not guarantee that a value is ready to read; reads still follow
the :term:`watermark <Watermark>` rules. A later operation can change the scene
after the query completes.

Selecting Children
~~~~~~~~~~~~~~~~~~

Filter ``usd-parent`` by ``/World/Group`` to select prims immediately below
that prim, such as ``/World/Group/Left``. This excludes deeper descendants such
as ``/World/Group/Left/Child``. To include all descendants, use ``FilterOp.PREFIX``
on ``usd-path`` with ``/World/Group/``.

These snippets count the direct children of ``/World/Group``:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/queries/main.py
         :language: python
         :start-after: # [snippet:query-direct-children]
         :end-before: # [/snippet:query-direct-children]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/queries/main.cpp
         :language: cpp
         :start-after: // [snippet:query-direct-children-c]
         :end-before: // [/snippet:query-direct-children-c]
         :dedent:

Requiring Several Attributes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use one ``HAS`` predicate for each required attribute. These snippets select
prims with both ``points`` and ``faceVertexCounts``. Neither predicate checks
the attribute's type, length, or value. An empty array still counts as present:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/queries/main.py
         :language: python
         :start-after: # [snippet:query-required-attributes]
         :end-before: # [/snippet:query-required-attributes]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/queries/main.cpp
         :language: cpp
         :start-after: // [snippet:query-required-attributes-c]
         :end-before: // [/snippet:query-required-attributes-c]
         :dedent:

Listing the Matched Paths
~~~~~~~~~~~~~~~~~~~~~~~~~

Read ``usd-path`` for the selected prims, then resolve the returned path
identifiers through the :doc:`path_dictionary`. A returned group can include a
data index map, which tells you which tensor row holds each prim's path
identifier. The snippets below handle this mapping when present, fetch every
group, and release each group after use.

These snippets list the direct children of ``/World/Group``. Python uses the
stage's :class:`PathDictionary <ovstage.PathDictionary>` as ``paths`` and imports ``OrdinalRange`` from
``ovstage``. C uses the stage dictionary as ``dict``; its ``resolvePath()`` helper
joins the path's component tokens into a string. Both examples sort the resulting
strings for display; query order is not a path-sort guarantee.

The snapshot read below uses end ordinal 2 from the example's update cycle. ``usd-path``
is derived from current structure and is not watermark-gated; the ordinal does
not select historical paths:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/queries/main.py
         :language: python
         :start-after: # [snippet:query-matched-paths]
         :end-before: # [/snippet:query-matched-paths]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/queries/main.cpp
         :language: cpp
         :start-after: // [snippet:query-matched-paths-c]
         :end-before: // [/snippet:query-matched-paths-c]
         :dedent:

Selecting Prims With an Applied API Schema
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use ``FilterOp.CONTAINS`` on ``usd-schemas`` to select prims with an applied
API schema. These snippets find prims with ``ShadowAPI``, which is applied to
``/World/Group/Right`` in the example scene:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/queries/main.py
         :language: python
         :snippets: query-filter-imports, query-applied-schema

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/queries/main.cpp
         :language: cpp
         :snippets: query-applied-schema-c

Listing the Applied API Schemas on a Prim
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Select the prim by its exact path, then read ``usd-schemas``. It returns an
array of token identifiers; resolve each through the :doc:`path_dictionary`.
The names describe schemas present in the runtime stage, including any added
during population.

These snippets list the schemas on ``/World/Group/Right``. Python uses the
stage dictionary as ``paths`` and imports ``OrdinalRange`` from ``ovstage``.
The C helper ``tokenName()`` resolves a token through that dictionary.
The snapshot uses end ordinal 2, after the example has advanced the
:term:`watermark <Watermark>`. A later recorded schema change makes this snapshot
unavailable; use ``UINT64_MAX`` (``(1 << 64) - 1`` in Python) for current values:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/queries/main.py
         :language: python
         :snippets: query-list-schemas

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/queries/main.cpp
         :language: cpp
         :snippets: query-list-schemas-c

Supported Predicates
--------------------

The current implementation supports these combinations. The operator names are
members of ``FilterOp`` in Python.

.. list-table::
   :header-rows: 1
   :widths: 28 20 52

   * - Attribute
     - Operator
     - Matches
   * - Any supported attribute
     - ``HAS``
     - Prims where the attribute is present; no value comparison.
   * - ``usd-path``
     - ``IN``
     - Any of the exact paths listed. Use several values to check a set of candidate paths.
   * - ``usd-path``
     - ``PREFIX``
     - Paths starting with a listed byte prefix; see the trailing-slash rule below.
   * - ``usd-parent``
     - ``IN``
     - Direct children of any listed parent.
   * - ``usd-children``
     - ``CONTAINS``
     - Prims with any listed child path.
   * - ``usd-prim-type``
     - ``IN``
     - Prims with any listed type, such as ``Mesh`` or ``Cube``.
   * - ``usd-schemas``
     - ``CONTAINS``
     - Prims with any listed applied API schema, such as ``PhysicsRigidBodyAPI``.
   * - ``usd-ancestor-types``
     - ``CONTAINS``
     - Prims with any listed ancestor schema type, such as ``Gprim`` or ``Boundable``.

**Any versus all:** values within one ``IN`` predicate match any listed value.
Separate predicates must all match. For example, one ``usd-prim-type`` predicate
with ``["Mesh", "Cube"]`` matches either type. Two separate type predicates
requiring ``Mesh`` and ``Cube`` cannot both match a prim.
For applied schemas and ancestor types, one ``CONTAINS`` predicate with several
names matches any;
use separate predicates to require all of them.

Other combinations fail with ``ErrorCode.NOT_SUPPORTED``. In particular, the
current query implementation cannot compare an ordinary attribute's value,
such as ``mass > 10``. Read the values and filter them in your application.
``usd-active`` is unsupported even with ``HAS``.

:meth:`query.result().attributes <ovstage.Query.result>` lists discovered attribute tokens. Resolve them
with the path dictionary. The ``attrs=`` argument to :meth:`Stage.query() <ovstage.Stage.query>` limits
that discovery; it does not require the attributes to exist. Use ``HAS`` for
that requirement.

Built-in Metadata Attributes
----------------------------

These reserved attributes describe the current scene. All except ``usd-active``
can be read and used in filters:

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
   * - ``usd-ancestor-types``
     - The types the prim's type derives from, such as ``Gprim`` and ``Boundable``
       for a ``Mesh``. Use ``CONTAINS`` to select prims by a base type.
   * - ``usd-parent``
     - The parent prim.
   * - ``usd-children``
     - The child prims.
   * - ``usd-active``
     - Whether the prim is active. **Not supported** — a live prim is always
       active, so predicates naming it return ``ErrorCode.NOT_SUPPORTED``; subject to
       removal in a future release.

A query that names no path restriction spans every prim on the stage. With the
``RENDERING`` domain populated, that currently includes a few prims the runtime
maintains for its own bookkeeping (see :ref:`population-runtime-owned-prims`, a
temporary limitation of that domain). A ``HAS`` predicate on ``usd-prim-type``
matches those too, so ``total_prim_count`` on such a query is not the number of prims
in the loaded scene. Scope to the scene with a ``usd-path`` ``PREFIX``, or match
``usd-prim-type`` ``IN`` the types you expect, when the count matters.

``usd-path`` PREFIX Convention
------------------------------

``FilterOp.PREFIX`` compares the opening bytes of the path string. A value without a
trailing ``/`` can match unrelated paths that share the same opening bytes — for
example, ``PREFIX "/World"`` also matches ``/Worldwide``. Append a trailing
``/`` to scope to the subtree under that path: ``PREFIX "/World/"`` matches only
descendants of ``/World``; it does not match ``/World`` itself.

Further Reading
---------------

- :doc:`reading_attributes` — read columns over a query.
- :doc:`writing_attributes` — write columns over a query.
- :doc:`/scene/path_dictionary` — build the path list a query is opened from.
