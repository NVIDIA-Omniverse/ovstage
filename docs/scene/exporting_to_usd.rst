.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Exporting ovstage to USD
========================

Export writes selected :term:`runtime stage <Runtime stage>` values into USD.
Use it to save simulation results or create a layer containing selected updates.
It does not reconstruct the original USD layers or continuously synchronize them.

The snippets come from the :doc:`USD export example </examples/usd_export>`.
Its runtime-created ``/World/Ball`` has a transform, temperature, radius, and
recorded ``Sphere`` type. The examples export at ordinal 1 after advancing the
watermark to 1; the changed-property example then writes at ordinal 2.
Python's ``directory`` is a temporary output directory. C uses ``prefix`` to
name output files. The typed-hierarchy and mesh snippets use a separate
USD-populated stage.

Export uses runtime values at the requested :term:`ordinal`, including writes
made after population. It does not reread values from the USD source.

:ref:`Population conversions <population-implicit-conversions>` carry through
to export. For example, a schema-declared ``int[]`` populated from ``uint[]``
exports as ``int[]``. A ``double`` converted to ``float`` retains only float
precision. Values omitted during population can be exported only if they were
added to the runtime stage another way.

Choosing a Destination
----------------------

* **Save once:** :func:`population.export_to_usd_file() <ovstage.population.export_to_usd_file>` creates an empty destination,
  exports, saves, and closes it. A successful save replaces any existing stored
  content. For populated USD subtrees, prefer
  :func:`population.export_typed_hierarchy_to_usd_file() <ovstage.population.export_typed_hierarchy_to_usd_file>`. Neither needs ``pxr``
  Python bindings.
* **Accumulate several exports:** use :class:`population.ExportDestination <ovstage.population.ExportDestination>` and call
  :meth:`destination.save() <ovstage.population.ExportDestination.save>` explicitly. ``ExportDestinationMode.EMPTY`` starts empty;
  ``OPEN_EXISTING`` preserves an existing layer stack and authors its root layer.
  Closing or leaving a ``with`` block does not save.
* **Edit a host-owned USD object:** :func:`population.export_to_usd_layer() <ovstage.population.export_to_usd_layer>` authors
  the specified layer. :func:`population.export_to_usd_stage() <ovstage.population.export_to_usd_stage>` opens the root-layer
  identifier and uses the default edit target; it does not preserve a host's
  custom edit target. The host must save the result.

Python's in-memory wrappers pass identifiers to the native API, not ``pxr``
objects. A layer or stage from an unrelated USD build, including standalone
``usd-core``, is not compatible just because its identifier matches.

For a single saved layer, export the transform and inspect the report:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-example-imports, export-file

   .. tab-item:: C

      ``check()`` and ``checkOvx()`` stop on errors; ``waitOp()`` waits for runtime
      operations and releases them. ``createStage()`` creates the source instance.

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-file-c

Destination Lifetime
--------------------

Keep the source stage alive until its reusable destinations are closed. Only
one active destination in the process can own a canonical identifier. Use
``with`` or :meth:`close() <ovstage.population.ExportDestination.close>`; dropping a live destination attempts cleanup without
saving and emits ``ResourceWarning``.

A reusable destination remains open after :meth:`save() <ovstage.population.ExportDestination.save>`. A failed export leaves
its prior in-memory state unchanged. A failed save may already have changed
storage; portable atomic replacement is not guaranteed.

Direct layer/stage-identifier exports have different rules. Synchronous calls
edit in place, bypass the population/export queue, and provide no rollback;
serialize access yourself. During an asynchronous export, the host must not
read or write the destination USD object until the operation completes.

Create and close a destination explicitly when you need to control its lifetime.
This example discards its export by closing without saving:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-lifetime

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-lifetime-c

Export a Typed USD Hierarchy
----------------------------

For a :term:`populated <Population>` subtree, the typed-hierarchy helper uses
recorded USD types and schemas. These examples preserve Sphere and Cube types,
each prim's applied Physics API schema, and supported schema-declared attributes.
The source scene contains ``/World/Props/Ball`` (Sphere with
``PhysicsCollisionAPI``), ``/World/Props/Crate`` (Cube with
``PhysicsRigidBodyAPI``), and prims outside that subtree. Here ``destination``
is the output file path, and C calls the populated stage ``runtime``. Export
only the subtree:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: typed-hierarchy-python-example

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: typed-hierarchy-c-example

The helper selects the root and descendants, creates ``DEF`` prims using
``usd-prim-type``, applies recognized ``usd-schemas``, and exports supported
schema-declared attributes under their existing names. It also handles
``omni:xform``. For an instance root with synthetic ``XformInstance`` or
``ScopeInstance`` type, it recovers the authored USD type and schema-declared
attributes from ``usd-metadata:typeName`` when present (``PHYSICS`` or selector
population). A root populated only by ``RENDERING`` lacks that metadata and
exports as a typeless prim when its runtime type is synthetic.
The synthetic ``GeomInstanceAPI``
and ``GeomInstanceRootAPI`` tags are not applied as USD schemas. This exports
the root's selected values without recreating its instance reference.

``RENDERING`` adds ``MaterialBindingAPI`` on supported geometry and ``DomeLight``,
even without an authored material binding. Typed export applies this recognized API along
with recorded Physics APIs, so exported ``apiSchemas`` can contain more names
than were authored on the source USD prims.
Custom attributes require ``include_custom_namespaces=[...]``.

In C, keep the initializer's root ``ovx_string_t`` and its character buffer valid
through the synchronous export or successful asynchronous submission. The
asynchronous call copies the descriptor and its referenced data.

Export Runtime-Created Data with Rules
--------------------------------------

Data created by direct writes may lack USD type and schema metadata. Use
explicit rules to choose its USD representation. First write the example's
transform and scalar attributes, then advance the :term:`watermark <Watermark>`.
The later runtime-data snippets run inside the same Python context managers:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-example-setup
         :exclude-pattern: ^\s*#\s*\[/?snippet:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-example-setup-c
         :exclude-pattern: ^\s*//\s*\[/?snippet:

Export the transform as USD transform operations on an ``OVER`` prim:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-file

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-file-c

This creates an overlay layer, not a standalone typed scene. The recorded
``Sphere`` type is used later when an export requests ``DEF`` prims.

Reusable Destination
--------------------

Accumulate a transform and a temperature in the same destination, then save
both together. Closing without :meth:`save() <ovstage.population.ExportDestination.save>` discards unsaved changes:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-reuse

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-reuse-c


Copy Custom Attributes Without Renaming
---------------------------------------

An empty destination name reuses the source name. This glob rule copies
``userProperties:*`` without listing each attribute and enables type inference
for custom data. The example saves the baseline temperature of 20:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-custom

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-custom-c


Map Runtime Data to a USD Schema
--------------------------------

Map ``solver:collisionRadius`` to ``Sphere.radius``. The rule names the USD type;
``DEF`` mode uses the recorded ``usd-prim-type`` to create the Sphere:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-rule, export-rename

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-rule-c, export-rename-c

Value Representation
^^^^^^^^^^^^^^^^^^^^

**Booleans:** scalar and array values accept both ``{bool, 8, 1}`` and
``{uint, 8, 1}``. Use ``{bool, 8, 1}`` for boolean metadata: a single zero byte
with ``{uint, 8, 1}`` is interpreted as an empty string, not ``False``.

**Quaternions:** :attr:`ovstage.AttributeSemantic.QUATERNION` uses four floating-point
components in ``(x, y, z, w)`` order, with the real component last. The 16-, 32-,
and 64-bit forms export as ``quath``, ``quatf``, and ``quatd``, including arrays.
This applies to explicit export types, schema attributes, and inferred types.
An explicit quaternion rule with :attr:`ovstage.AttributeSemantic.NONE` instead
uses ``(w, x, y, z)``. If you reordered components to work around older exports,
remove that workaround when using ``QUATERNION``.

**Empty arrays:** selected token, asset, and numeric tuple arrays keep their USD
array type and empty value. Values matching schema defaults may still be omitted.
In C, an empty array may have a null data pointer; a nonempty array needs valid
storage.

**Root selection:** selecting ``/`` includes the whole stage. The root itself
supplies only explicitly selected layer metadata; it does not export as a prim
or supply prim attributes, transforms, API schemas, or prim metadata. Layer
metadata must match both the description's prim predicate and the rule's paths.

Export a Typed Mesh Subtree
---------------------------

The typed helper also exports mesh geometry. This example selects a subtree
containing ``Floor`` and ``Dynamic``, two triangle meshes with different Physics
API schemas. Both have points, face counts, and face indices in the source USD.
Python writes into ``temp_dir``, its temporary output directory. C adds a suffix
to ``destination``, the hierarchy output path:


.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-mesh

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-mesh-c

Each mesh keeps its own recorded schemas: the static mesh has collision only,
and the dynamic mesh also has ``PhysicsRigidBodyAPI``. Token and token-array
values require interned token identifiers and a schema or rule that supplies the USD ``token`` or ``token[]`` type. Mesh examples
include ``orientation`` and ``subdivisionScheme``.

A UTF-8 byte attribute with ``AttributeSemantic.STRING`` exports as one USD
``string`` under a string rule or custom type inference. Every byte is part of
the value: do not append a C-style NUL terminator. USD cannot store embedded
NUL bytes, so such a value is unsupported (an error for a ``REQUIRED`` rule).
Without a semantic, the existing byte-string path still accepts one trailing
NUL as a terminator.

Typed export resolves token ID 0 as the empty token and skips it when it matches
the schema default, unless ``EXPORT_DEFAULT_VALUE`` requests that value.
Distant-light purpose retains authored or inherited non-default values.

Relationships and connections need explicit property rules. Single-target
connections written directly to ovstage can be exported. Population does not
yet create those columns from USD connections, and multiple connection targets
cannot yet be represented without losing information.

Export Only Changed Properties
------------------------------

After exporting a baseline, select properties changed in
``(since_ordinal, ordinal]``. The lower bound is excluded and the upper bound
included. Keep the baseline in the layer stack when consuming the update layer.

The custom-attribute example saved temperature 20 at ordinal 1. Change it to
24.5 at ordinal 2, then export only that change:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-changed

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-changed-c


Export Options
--------------

Pass the current global :term:`watermark <Watermark>` as the export's source
:term:`ordinal <Ordinal>`; stale or future ordinals are rejected. Export builds
stage-wide snapshot context at that ordinal before applying its selection.
An unrelated attribute with a recorded change above the watermark can therefore
make even a narrowly selected export fail with ``INVALID_ARGUMENT``. Wait for
pending writers and seal their changes before exporting. A newer ordinal cannot
bypass this requirement until the global watermark reaches it; historical
payload versions are not available.

In C, initialize descriptors with
:c:func:`ovstage_population_export_desc_init_snapshot() <ovstage_population_export_desc_init_snapshot>` or
:c:func:`ovstage_population_export_desc_init() <ovstage_population_export_desc_init>`. ``struct_size`` identifies the header
layout; it does not make older layouts compatible automatically. Python wrappers
initialize the descriptor for you.

Selection
^^^^^^^^^

``selection`` normally uses ``EXPLICIT_PREDICATE`` to select current values by
conditions and rules. ``CHANGED_PROPERTIES_SINCE_ORDINAL`` also restricts ordinary
properties to changes in ``(since_ordinal, ordinal]``. It writes current USD
default values, not intermediate time samples or a complete standalone scene.

``prim_predicate`` and ``property_predicate`` use the builders described in
:doc:`population`, but export supports a subset. Prim conditions can match
paths, subtrees, types, and schemas. Property conditions can match names,
namespaces, schema declarations, and attribute/relationship kinds. Combine
supported conditions with :meth:`and_() <ovstage.population.PrimPredicate.and_>`, :meth:`or_() <ovstage.population.PrimPredicate.or_>`, and :meth:`not_() <ovstage.population.PrimPredicate.not_>`. Conditions that
require information absent from the runtime stage, such as :meth:`is_authored() <ovstage.population.PropertyPredicate.is_authored>`,
are rejected with ``NOT_SUPPORTED``.
Rules determine how selected data is written to USD; schema-declared export
can include supported properties without individual attribute rules.

Export only ``userProperties:temperature`` on ``/World/Ball``. The predicate
selects the property; the rule supplies its USD representation:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-selection

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-selection-c

Rule Tables
^^^^^^^^^^^

.. list-table::
   :header-rows: 1

   * - Rule
     - Authors
     - Important controls
   * - Prim
     - Prim specs and optional type names
     - Exact/prefix path, ``OVER``/``DEF``, unknown-type metadata key
   * - API schema
     - Single- or multiple-apply API schemas
     - Schema name, instance name, required flag
   * - Attribute
     - Values and recognized transforms
     - Exact/glob source name, rename, explicit/schema/inferred USD type
   * - Property edge
     - Relationships, material bindings, or connections
     - Explicit property kind and destination value type for connections
   * - Metadata
     - Prim metadata, layer metadata/custom data, or references
     - Explicit destination kind and optional key rename

Set ``population.OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED`` when a rule that
cannot match, convert, or author must fail the export. Optional rules increase
the skipped or unsupported counters instead. Custom attribute type inference
requires ``population.OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE``;
otherwise supply an explicit USD type.

For example, this required attribute rule renames the solver radius and gives
it the USD ``double`` type. The mapping example above uses this rule:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-rule

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-rule-c

Layer and Transform Options
^^^^^^^^^^^^^^^^^^^^^^^^^^^

``layer_mode`` defaults to ``OVER`` for overlay layers. ``DEF`` creates
prim definitions, choosing types from prim rules or ``usd-prim-type``.

Use ``SourceApiSchemaPolicy.APPLY_RECORDED`` to apply each prim's recognized
``usd-schemas`` and ``ExportProjection.SCHEMA_DECLARED`` to export attributes
declared by those schemas or the concrete type. Explicit rules take precedence
and remain necessary for renaming, custom namespaces, relationships, connections,
and metadata. Use explicit schema rules when the destination should have different
schemas. ``unknown_metadata_policy`` controls unrecognized or malformed metadata.

The default ``transform_policy`` converts supported local transforms into USD
transform operations when exact decomposition is possible. ``USD_MATRIX_OP``
writes a matrix operation; ``NONE`` omits recognized transform attributes.
Unsupported states are reported rather than approximated.

Where no public Python enum is provided, the option names above abbreviate
constants in ``ovstage.population``: for example,
``OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF`` and
``OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_MATRIX_OP``. See
:doc:`/python_api/index` for full names.

To preserve the local transform as a matrix operation on a typed prim, export
``omni:xform`` with ``DEF`` and ``USD_MATRIX_OP``:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-matrix

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-matrix-c

Reports and Failures
--------------------

Reports count examined, exported, skipped, and unsupported items.
``source_states_unavailable`` counts a rejected stale or future source ordinal.
Stage-wide snapshot reads can also fail while unsealed changes are recorded,
including outside the export selection; see `Export Options`_ above.
Use required rules for values that must export successfully. Python raises
:class:`ovstage.OvstageError` on a failed blocking call or completed asynchronous
operation; C callers check the returned status.

Inspect the earlier transform export's report, then try a stale ordinal to see
failure handling. The current watermark is 1, so ordinal 0 is rejected:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-report

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-report-c

Asynchronous Convenience API
----------------------------

The ``_async`` functions copy the descriptor and its referenced data, then
queue work on the source stage's population queue. A file export completes only
after saving. Both examples export ``omni:xform`` at ordinal 1 and wait for the
export report:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/usd-export/main.py
         :language: python
         :snippets: export-async

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/usd-export/main.cpp
         :language: cpp
         :snippets: export-async-c

Wait for every accepted asynchronous export or destination operation using its
export-specific wait. A timeout leaves it pending; wait again. Creation, export,
save, and destruction share the population queue and can be submitted in order
before waiting. Creation reserves a handle immediately, but successful completion
is required before treating the destination as open.

Do not mix synchronous access with pending asynchronous work on the same
destination. An export wait reports only that export or destination operation.
Use the population wait separately to observe unrelated population operations
and their failures.

Limitations
-----------

* **Current values only.** If ``radius`` changes from 1 to 2 to 3, a changed-property
  export writes 3. It does not write both historical edits or reconstruct earlier
  scene structure.
* **Schema policy is explicit.** The low-level default uses explicit schema
  rules. Use ``SourceApiSchemaPolicy.APPLY_RECORDED`` or the typed helper to keep
  recognized recorded schemas.
* **Property kinds are explicit.** Relationship and material-binding rules require
  the relationship-path semantic. A single-path column (``is_array=False``)
  exports as a relationship with that one target. Connection rules require a single target and
  the connection semantic. A zero-initialized property rule is invalid. Optional
  unsupported rules are counted; required ones fail.
* **Connections have limits.** A direct-write single-target connection can be
  exported alongside a value on the same attribute. Population does not create
  connection columns from USD, and multiple targets have no lossless runtime form.
* **Changed properties are not a structural diff.** A changed ``radius`` can
  produce just that value, but a broad changed-prim rule may also author the prim
  definition even if its type and hierarchy did not change. Use attribute rules
  for value overlays.
* **Layer composition is not copied.** ``OPEN_EXISTING`` and in-memory export
  preserve unrelated destination opinions, but do not reconstruct source layers
  or select values by their layer of origin. One-shot file export starts empty
  and replaces stored content.

Further Reading
---------------

* :doc:`population` — load and update USD in the opposite direction and compose
  shared prim/property predicates.
* :doc:`writing_attributes` — author the runtime columns selected for export.
* :doc:`queries` — inspect the runtime state that predicates select.
* :doc:`/concepts/async_model` — operation ownership and waiting.
* :doc:`/concepts/string_handling` — C string-view lifetime and ownership.
* :doc:`/guides/project_setup_c` and :doc:`/guides/project_setup_python` —
  build, link, and package setup for the public C and Python surfaces.
* :doc:`/concepts/error_handling` — status and error-string handling.
