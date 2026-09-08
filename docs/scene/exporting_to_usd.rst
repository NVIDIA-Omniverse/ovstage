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

The population export API (``ovstage_population_export.h``) authors a selected
slice of ovstage state into USD. Use it to bake simulation results, produce a
sparse overlay, or persist application-selected properties without passing
OpenUSD C++ objects across the public ABI.

.. note::

   Export is a snapshot-authoring operation, not a live synchronization system.
   USD-to-ovstage population is intentionally lossy, so export cannot reconstruct
   every opinion from the original USD source.

Destination Ownership and Persistence
-------------------------------------

The public C ABI addresses USD storage with strings so no OpenUSD object crosses
the boundary. Choose the destination form from the required lifetime:

* **One snapshot, one durable file:**
  ``ovstage_population_export_to_usd_file`` and Python
  ``population.export_to_usd_file`` create an empty private destination, export
  once, save, and close. For populated USD hierarchies, Python
  ``population.export_typed_hierarchy_to_usd_file`` supplies the common typed
  export policies automatically. Existing stored content is not read; a
  successful save replaces it. This workflow requires no ``pxr`` Python bindings.
* **Several exports, one durable file:** create an
  ``ovstage_population_export_destination_handle_t`` or Python
  ``ExportDestination``. Exports accumulate in memory until an explicit
  ``*_destination_save`` / ``destination.save()``. ``EMPTY`` starts without
  reading storage; ``OPEN_EXISTING`` opens and preserves an existing layer stack
  and authors its root layer. Closing or context-manager exit never saves.

Every destination is opened and owned by ovstage. There is no route that authors
into a USD layer or stage owned by the calling application: OpenUSD does not
promise that a layer or stage identifier means the same thing in two USD
runtimes, so such a route would only work when the caller happens to share
ovstage's USD build. Hand off through storage instead — export to a file, then
open or reload that file in the host's own USD runtime.

Reusable destinations are bound to their source ovstage instance, which must
outlive them. Only one active destination may own a canonical identifier in the
process. A successful save writes the complete current destination and leaves it
open for later export/save calls, but OpenUSD does not promise portable atomic
replacement; a failed save may have modified storage. A failed export leaves
the reusable destination's in-memory state unchanged.
Dropping a live Python ``ExportDestination`` queues a best-effort no-save release
and emits ``ResourceWarning``; use ``with`` or call ``close()`` explicitly instead.

Export a Typed USD Hierarchy
----------------------------

When the runtime stage was populated from USD, use the typed-hierarchy helper to
export an ordinary subtree without rebuilding its USD representation as rule
tables. This example preserves the Sphere and Cube types, each prim's own applied
Physics API schema, and the supported attributes declared by those schemas:

.. filtered-literalinclude:: ../../tests/python/test_population_export.py
   :language: python
   :start-after: # [snippet:export-typed-hierarchy-to-usd-file]
   :end-before: # [/snippet:export-typed-hierarchy-to-usd-file]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

The helper selects ``root_path`` and its descendants, authors typed ``DEF``
prims from ``usd-prim-type``, applies each prim's recognized ``usd-schemas``, and
projects supported schema-declared attributes under their source names. It also
handles recognized ``omni:xform`` values. Application-owned custom attributes
remain excluded unless their namespaces are named with
``include_custom_namespaces=[...]``.

Export Runtime-Created Data with Rules
--------------------------------------

Runtime-created columns may not carry enough USD representation metadata for the
typed helper. This example writes local transforms, seals the ordinal, then
exports and saves the recognized ``omni:xform`` column. The descriptor defaults
produce sparse ``OVER`` prims and exact USD xform ops:

.. filtered-literalinclude:: ../../tests/python/test_population_export.py
   :language: python
   :start-after: # [snippet:export-runtime-to-usd]
   :end-before: # [/snippet:export-runtime-to-usd]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

The destination is replaced by a layer containing this export. This is an
overlay, not a standalone typed scene. Use the typed-hierarchy helper when the
source carries USD type and schema metadata; use explicit rules when runtime
columns must be mapped to a different USD representation.

Reusable Destination
--------------------

Use a reusable destination when several selected exports must accumulate before
one save, or when existing composed content must be preserved. Saving is always
explicit; leaving the context without ``save()`` discards unsaved changes:

.. filtered-literalinclude:: ../../tests/python/test_population_export.py
   :language: python
   :start-after: # [snippet:export-reusable-destination]
   :end-before: # [/snippet:export-reusable-destination]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

Copy Custom Attributes Without Renaming
---------------------------------------

An empty destination name already means "reuse the source name." A glob rule can
therefore publish an application-owned namespace without listing every column.
Custom type inference is explicit because arbitrary runtime storage does not
otherwise imply a USD value type:

.. filtered-literalinclude:: ../../tests/python/test_population_export.py
   :language: python
   :start-after: # [snippet:export-custom-attributes-to-usd]
   :end-before: # [/snippet:export-custom-attributes-to-usd]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

Map Runtime Data to a USD Schema
--------------------------------

Explicit source and destination names matter when the runtime model differs from
the USD schema. Here a solver-owned radius column is mapped onto ``Sphere.radius``.
``DEF`` mode reads the source ``usd-prim-type`` metadata to create a durable
Sphere, while the rule states the destination attribute's USD type explicitly:

.. filtered-literalinclude:: ../../tests/python/test_population_export.py
   :language: python
   :start-after: # [snippet:export-schema-attribute-to-usd]
   :end-before: # [/snippet:export-schema-attribute-to-usd]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

Export a Typed Mesh Subtree
---------------------------

The same helper handles a common geometry subtree without an attribute allowlist
or blanket API-schema rules. The tested source contains two Meshes with geometry
and different Physics APIs; the call is only the root selection:

.. filtered-literalinclude:: ../../tests/python/test_population_export.py
   :language: python
   :start-after: # [snippet:export-homogeneous-mesh-subtree]
   :end-before: # [/snippet:export-homogeneous-mesh-subtree]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

The exporter derives each Mesh type and preserves the APIs recorded on that
specific prim: the static Mesh remains collision-only while the dynamic Mesh
also receives ``PhysicsRigidBodyAPI``. Token-valued properties such as
``orientation`` and ``subdivisionScheme`` export when their runtime values are
pre-interned token-ID rows and the selected schema or an explicit rule supplies
the token type. Relationships and connections still require property-edge
rules; positive connection export remains unavailable.


Export Only Changed Properties
------------------------------

After a baseline export, a later file can contain only ordinary properties
dirtied in the open interval ``(since_ordinal, ordinal]``. The result is an
overlay: whoever consumes it must compose it over the baseline export:

.. filtered-literalinclude:: ../../tests/python/test_population_export.py
   :language: python
   :start-after: # [snippet:export-changed-properties-to-usd]
   :end-before: # [/snippet:export-changed-properties-to-usd]
   :exclude-pattern: ^\s*#\s*\[/?snippet:
   :dedent:

Metadata-Driven C Descriptor
----------------------------

The C API expresses the same metadata-driven policy through the public
``ovstage_population_export_desc_init_typed_hierarchy`` convenience initializer.
It initializes a snapshot descriptor, selects the root and descendants, chooses
``DEF`` mode, applies recorded schemas, and selects schema-declared projection.
The caller-owned root ``ovx_string_t`` object and the character bytes referenced
by that view remain borrowed by the descriptor through a synchronous call or
successful async enqueue; do not destroy or change either sooner.

.. filtered-literalinclude:: ../../tests/c/test_population_export.cpp
   :language: cpp
   :start-after: // [snippet:export-typed-hierarchy-to-usd-c]
   :end-before: // [/snippet:export-typed-hierarchy-to-usd-c]
   :exclude-pattern: ^\s*//\s*\[/?snippet:
   :dedent:

Shared population predicates select source prims and properties. Rule tables are
destination-authoring instructions for renaming, custom data, and other explicit
mappings. Schema-declared projection can author supported properties without a
matching attribute rule.

Descriptor Model
----------------

Initialize every descriptor with
``ovstage_population_export_desc_init_snapshot`` or
``ovstage_population_export_desc_init``. A snapshot descriptor records the
current committed ordinal expected by the caller. ``struct_size`` identifies
this header revision; it is not an automatic compatibility mode for older
descriptor layouts. Export rejects stale and future ordinals rather than
silently reading a different snapshot.

Selection
^^^^^^^^^

``EXPLICIT_PREDICATE`` exports the current committed state selected by the
predicates and rules. ``CHANGED_PROPERTIES_SINCE_ORDINAL`` further intersects
property authoring with the open interval ``(since_ordinal, ordinal]``. It is
useful for incremental overlays, but it does not make the output a complete
standalone description: selected values are authored as current USD default
opinions, never as intermediate time samples.

``prim_predicate`` and ``property_predicate`` use the same immutable predicate
graphs as generic USD-to-ovstage population. Compose path, concrete type,
applied-schema, property-name, namespace, and property-kind leaves with
``AND``, ``OR``, and ``NOT``. Predicates select source candidates; rule-local
path and name matching routes selected data to destination opinions.

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

Set ``OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED`` when failure to match, convert,
or author a rule must fail the operation. Optional rules instead contribute to
the report's skipped or unsupported counters. Type inference for custom
attributes is opt-in through ``RULE_INFER_CUSTOM_TYPE``; an explicit USD type is
usually clearer at a public boundary.

Layer and Transform Policy
^^^^^^^^^^^^^^^^^^^^^^^^^^

``LAYER_MODE_OVER`` is the conservative default for sparse overlays.
``LAYER_MODE_DEF`` creates durable definitions and uses a prim rule or readable
``usd-prim-type`` metadata to choose type names.

``SOURCE_API_SCHEMAS_APPLY_RECORDED`` applies recognized effective schemas from
``usd-schemas`` on each selected prim. ``PROJECTION_SCHEMA_DECLARED`` then
authors selected runtime attributes declared by the concrete type or those
applied schemas. Explicit rules still take precedence and remain necessary for
renaming, custom namespaces, relationships, connections, and metadata.

The default transform policy converts recognized local transforms into USD
xform ops when the value has an exact supported decomposition.
``TRANSFORM_USD_MATRIX_OP`` authors an explicit matrix op, while
``TRANSFORM_NONE`` omits recognized transform columns. Unsupported transform
states are reported rather than silently approximated.

Reports and Failures
--------------------

The report summarizes examined, exported, skipped, and unsupported items. Its
``source_states_unavailable`` counter records a rejected stale or future source
ordinal.
Use required rules and the returned ``ovstage_api_status_t`` for correctness
requirements. In Python, a failing blocking export or terminal async wait raises
``OvstageError``.

Asynchronous Convenience API
----------------------------

The async functions deep-copy the descriptor graph and enqueue work on the
source instance's population FIFO. The one-shot file operation does not complete
until the file has been saved:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_export.py
         :language: python
         :start-after: # [snippet:export-runtime-to-usd-async]
         :end-before: # [/snippet:export-runtime-to-usd-async]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_export.cpp
         :language: cpp
         :start-after: // [snippet:export-runtime-to-usd-async-c]
         :end-before: // [/snippet:export-runtime-to-usd-async-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Every accepted async operation must receive an export-specific terminal wait. A
timeout does not consume the result, so wait again. Reusable-destination create,
export, save, and destroy operations share the source instance's population
FIFO; they may be enqueued in order before waiting. The create call reserves a
handle synchronously, but only its terminal wait proves that opening succeeded.
Do not mix synchronous calls with pending async work on the same destination.
An export-specific wait observes only its export/destination operation. Use the
generic population wait separately to observe and drain unrelated population
operations (including their failures).

Limitations
-----------

Export authors the runtime representation available at one committed ordinal.
It is not a reversible copy of the USD file that originally populated ovstage.
The following limitations are important when choosing rules and interpreting the
result.

Export reads one snapshot, not its history
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Export reads the values present at ``ordinal``. ``since_ordinal`` limits which
dirty properties are selected, but it does not provide every value that existed
between the two ordinals. For example, if ``radius`` changed from 1 to 2 and then
to 3, an incremental export authors the current value 3, not two historical edits.
Historical payload or topology addressing is not available.

Applied API schemas require an explicit policy
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The low-level descriptor defaults to explicit API-schema rules. Set
``SOURCE_API_SCHEMAS_APPLY_RECORDED``, or use the Python typed-hierarchy helper,
to preserve each selected prim's recognized ``usd-schemas``. Unrecognized or
malformed schema metadata follows ``unknown_metadata_policy``. Explicit
API-schema rules remain necessary when the destination schema assignment should
differ from the source.

Some USD property representations cannot be exported
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Pre-interned token-ID scalar and array rows can be authored as USD token and
token-array values when a selected schema or explicit rule supplies the token
type. For example, a Mesh's token-valued ``subdivisionScheme`` exports through a
matching attribute rule. Attribute connection targets, such as a shader
``inputs:roughness`` connection, still cannot be authored by a positive connection
rule, and a value plus its connection cannot be round-tripped together.

Relationship and material-binding target lists are supported when the source
column carries the documented relationship-path semantic. Always choose
relationship, connection, or material binding explicitly; a zero-initialized
property rule is invalid. Optional unsupported rules increase the report's
skipped or unsupported counters. Mark a rule required when this must instead
fail the export.

Changed-property mode is precise for values, not prim structure
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Changed-property selection can identify supported dirty attribute values. It
cannot always distinguish a changed prim spec or topology from an ordinary value
change. For example, changing only ``/World/Ball.radius`` can correctly produce a
sparse ``radius`` opinion, while a broad changed prim rule may also re-author the
``/World/Ball`` prim spec even though its type and hierarchy did not change. Use
changed attribute rules for value overlays; do not rely on changed prim rules as
an exact structural layer diff.

Reusable ``OPEN_EXISTING`` destinations preserve
unrelated destination opinions, but export is not a general USD composition
copier or source-provenance filter. One-shot file export intentionally starts
empty and replaces the stored destination.

Where to Go Next
----------------

* :doc:`population` — load and update USD in the opposite direction and compose
  shared prim/property predicates.
* :doc:`writing_attributes` — author the runtime columns selected for export.
* :doc:`queries` — inspect the runtime state that predicates select.
* :doc:`/concepts/async_model` — operation ownership and waiting.
* :doc:`/concepts/string_handling` — C string-view lifetime and ownership.
* :doc:`/guides/project_setup_c` and :doc:`/guides/project_setup_python` —
  build, link, and package setup for the public C and Python surfaces.
* :doc:`/concepts/error_handling` — status and error-string handling.
