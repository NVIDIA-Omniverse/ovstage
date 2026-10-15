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

:term:`Population` transfers selected, composed USD data into the
:term:`runtime stage <Runtime stage>`. Use ``ovstage.population`` to load a
scene, apply USD edits, or sample animated values at an :term:`ordinal <Ordinal>`.
Population has its own operation queue and waits, separate from direct
attribute reads and writes. See :doc:`/concepts/async_model`.

.. note::

   These entry points are **pre-release and provisional** ("Draft — API in
   flux").

Supply an ordinal above the applicable write floors and wait for population to
complete. Apply any direct writes that override populated values afterward,
using a newer ordinal. Once all updates are complete, advance the write floor
and wait for completion before reading or rendering the results. See
:ref:`population-ordinal-ownership` for sequencing and
:ref:`ordinal-admission-policies` for policy configuration.

Loading a Scene
---------------

Use :func:`ovstage.population.open_usd()` for a file or
:func:`ovstage.population.open_usd_from_string()` for USDA text. Both block
until population completes; their ``_async`` variants return an operation.
Advance the :term:`watermark <Watermark>` and wait before reading the values.
The following examples load a scene at a chosen ordinal:

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

A population domain is a predefined selection of USD prims and properties
needed by a consuming library. Pass ``PopulationDomain`` values through the
``domains`` argument when loading USD:

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Domain
     - What it selects
   * - ``RENDERING``
     - Scene data for rendering, including meshes, lights, materials, and cameras.
   * - ``PHYSICS``
     - Scene data for physics, including bodies, colliders, joints, materials,
       and the geometry and hierarchy they need.
   * - ``ALL``
     - Both ``RENDERING`` and ``PHYSICS``. This does not mean every USD property.
   * - ``NONE``
     - No predefined selection. Use explicit selectors to choose the prims and
       properties to populate.

Combine domains with bitwise OR, for example
``PopulationDomain.RENDERING | PopulationDomain.PHYSICS``. Selecting a domain
loads data; it does not start a renderer or run a physics simulation. Explicit
selectors add to the domain's selection, as described below.

.. _population-physics-selection:

Physics Selection
~~~~~~~~~~~~~~~~~

``PHYSICS`` selects prims with supported physics types or applied APIs, including
scenes, rigid bodies, colliders, joints, articulations, and physics materials.
It also includes supporting prims needed to interpret that data:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Data
     - What population includes
   * - Physics properties
     - Attributes and relationships whose names are declared by the supported
       physics schemas. Unrelated geometry and display properties inherited
       from base schemas are excluded unless a separate rule includes them.
   * - Shape geometry
     - Selected geometry properties for colliders and other supported physics
       roles, such as mesh points and topology or primitive dimensions. A plain
       render mesh is not selected just because it has geometry.
   * - Relationship targets and subsets
     - Supporting geometry named by physics relationships, and ``GeomSubset``
       children of colliders, including their indices and element type.
   * - Materials
     - Physics material properties and bindings, including prims that author
       ``material:binding:physics`` to a physics material even when those prims
       have no physics API themselves.
   * - Point instancers
     - ``PointInstancer`` prims whose prototypes contain physics data. Includes
       their schema properties except ``extent``, plus ``inactiveIds`` metadata.
   * - Hierarchy and metadata
     - Ancestors and transform data, as described in
       :ref:`population-ancestors-transforms`. Stage metadata includes
       ``metersPerUnit``, ``kilogramsPerUnit``, and ``upAxis``. On prims selected
       by a physics schema, population also requests
       ``customData:physics:localSpaceVelocities``.

Selected attributes can use schema defaults without an authored value. Some
physics defaults are computed instead: for example, an unauthored scene gravity
uses the stage's units and up axis. Population also resolves physics material
bindings and supported physics collections for consumers.

Applying an arbitrary custom API does not make its properties part of the
physics domain. Add explicit selectors for application attributes. Those
selectors extend the physics selection; they do not restrict it.

The examples below use ``scene.usda`` from the
:doc:`population example </examples/population>`. It contains a rigid-body Cube
at ``/World/Body`` below an Xform at ``/World``. Load its physics data first.
The later snippets use the same ``stage`` and ``paths`` inside the Python
context managers; in C, keep ``stage`` and ``dict`` until the end of the example.
``scene`` names the adjacent ``scene.usda`` file:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-imports, population-setup

   .. tab-item:: C

      ``waitPop()`` checks population completion; ``waitOp()`` waits for runtime
      operations and releases them. ``check()`` and ``checkOvx()`` stop on errors.

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-setup-c

.. _population-rendering-selection:

Rendering Selection
~~~~~~~~~~~~~~~~~~~

``RENDERING`` builds the scene data needed by the renderer. It handles supported
geometry, lights, cameras, materials, volumes, and render settings, together
with their transforms, hierarchy, and instances. It does not copy every USD
prim and property unchanged.

Property selection depends on the kind of prim:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Data
     - What population includes
   * - Geometry, lights, and cameras
     - The attributes handled for that prim type. Values can come from
       authored opinions, schema defaults,
       or rendering-specific defaults. Some runtime attributes are derived,
       such as combined transforms.
   * - Applied APIs
     - Properties of the APIs supported by the relevant rendering handler.
       Registering and applying an arbitrary API does not guarantee that all
       its properties are populated.
   * - Primvars
     - Supported primvars are collected separately. They are not restricted
       to a fixed list of schema attributes.
   * - Materials and shaders
     - Material networks, shader inputs and outputs, and their connections
       use material-specific population. These properties need not be declared
       in a registered USD schema.
   * - ``RenderSettings``, ``RenderProduct``, and ``RenderVar``
     - Their supported rendering properties and an additional pass over
       supported authored attributes.
   * - Other custom attributes
     - Generally omitted unless explicitly selected or the extra pass
       described below is enabled.

Schema-defined properties can therefore be present even when they are absent
from the USDA text. Conversely, an authored custom property can be absent from
the runtime stage. Attribute presence is not evidence that the value was authored.

For application data, set a population description's ``domains`` to
``PopulationDomain.RENDERING`` and provide explicit selectors as described
below. The selectors add properties; they do not narrow the rendering
domain's selection.

A boolean ``populateAllAuthoredAttributes`` entry in the root or session layer's
``customLayerData``, set to true, enables an extra pass over authored attributes on prims
already populated. The hosting application can also enable this behavior.
The extra pass skips attributes
already present or without values, ``xformOp*`` and ``primvars:*`` attributes,
shader prims, instance proxies, and unsupported types, including asset arrays,
string arrays, and time codes.
Use selectors when you need to name exactly which additional properties to copy.

Selecting Prims and Properties
------------------------------

Use :class:`ovstage.population.Desc` for more control. It contains ``domains``
and a list of :class:`ovstage.population.Selector` objects. The ``_with_desc``
functions accept several descriptions; they combine the domains with OR and
append the selectors in order.

Each selector pairs a prim condition with a property condition. It selects
which :term:`prims <Prim>` to populate and which of their properties to include.
If several selectors match, their property selections are combined. An omitted
property condition selects no properties. Selectors
can also request prim and property metadata.

Build conditions with :class:`ovstage.population.PrimPredicate` and
:class:`ovstage.population.PropertyPredicate`:

* Multiple values in one condition mean “match any.” For example,
  :meth:`has_type("Cube", "Sphere") <ovstage.population.PrimPredicate.has_type>` matches either type.
* Combine conditions with :meth:`and_() <ovstage.population.PrimPredicate.and_>`, :meth:`or_() <ovstage.population.PrimPredicate.or_>`, and :meth:`not_() <ovstage.population.PrimPredicate.not_>`.
* :meth:`has_property() <ovstage.population.PrimPredicate.has_property>` selects a prim by a condition on its properties.

Unsupported kinds, cycles, or graphs exceeding the depth or node limits fail
at submission with a diagnostic. In C, predicate nodes and string arrays are
borrowed and must remain valid through the call. Python builders retain them.

For the remaining examples, select ``/World/Body`` and name the properties to
populate. The description also requests metadata, explained below. Populate
these values at ordinal 2, after the physics-domain load at ordinal 1:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-paths, population-property-metadata, population-stage-metadata

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-paths-c, population-property-metadata-c, population-stage-metadata-c

See :class:`ovstage.population.PrimPredicateKind` and
:class:`ovstage.population.PropertyPredicateKind` for the supported conditions.

Which Properties Become Runtime Attributes?
-------------------------------------------

Selecting a prim does not select all its properties. Domains choose the
properties they need; explicit selectors add the properties their conditions
match. Schema membership is not required for an explicit selection: the example
above includes the custom ``temperature`` attribute by name.

For explicit selectors and the ``PHYSICS`` domain, population reads the selected
properties through USD:

* A selected attribute is populated if USD resolves a value and ovstage supports
  its value type. The value can come from an authored default, a time sample, or
  a registered schema's default. It need not be authored in the scene file.
* A schema-declared attribute with no resolved value is skipped. Merely
  declaring or selecting an attribute does not create a runtime value.
* A selected relationship is populated only if it has authored targets.
  An explicitly authored empty target list is included as an empty array.

Schema defaults require the schema definitions to be registered with the USD
runtime used by population. For additional schema families, call
:func:`ovstage.population.register_usd_schemas()` before any code in the process
first reads USD schema definitions, including population, export, or another USD
consumer. Registration makes those definitions available; it does not select
their properties for population.

Domains can also produce derived data or supply domain-specific defaults.
See :ref:`population-rendering-selection` for the rendering domain’s selection
and conversion rules.

The example scene declares ``/World/Body`` as a Cube but never authors ``size``.
The selector above explicitly requests both ``size`` and the custom
``temperature`` attribute. Reading them returns the Cube schema's default size
of 2 and the authored temperature of 20:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-default, population-numeric

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-default-c, population-numeric-c

USD Value Types
---------------

The USD value type determines the runtime type. For schema attributes, this is
the type declared by the schema:

1. Numeric values retain their scalar type and bit width. DLPack lanes record
   the component count.
2. Arrays use the element's dtype with ``is_array=True``. Other values use one
   fixed-size value per prim.
3. Strings and paths become 64-bit identifiers resolved through the
   :doc:`path_dictionary`.
4. Numeric values authored in a different type are converted to the declared
   type when possible; see `Values Authored in Another Type Than Declared`_.

The tables describe storage types and canonical write dtypes. Returned reads
and maps expand component widths into trailing dimensions with scalar lanes;
see :doc:`/concepts/dlpack_tensors`.

The tables use ``{code, bits, lanes}`` for :class:`ovstage.DLDataType` and
:class:`ovstage.AttributeSemantic` names for semantics.

.. _population-implicit-conversions:

Implicit Conversions
~~~~~~~~~~~~~~~~~~~~

Population converts some USD values for the :term:`runtime stage`:

* **Strings and paths:** store identifiers from the :doc:`path_dictionary`.
  Exceptions store text bytes; see `Properties That Do Not Follow Their USD Type`_.
* **Assets:** store identifiers for the authored path and the path USD resolved.
  Generic attribute population does not resolve the asset again; rendering
  material inputs have the UDIM exception described below.
* **Empty strings:** use identifier 0 when stored as identifiers. See `Empty Values`_.
* **USD roles:** become attribute semantics. For example, ``point3f`` uses
  three floats with ``POINT`` semantic.
* **Schema-specific properties:** can use a different type or semantic.
  See `Properties That Do Not Follow Their USD Type`_.
* **Numeric type mismatches:** convert to the schema's declared type when possible.
  See `Values Authored in Another Type Than Declared`_.
* **Transforms:** combine USD transform operations into ``omni:xform``.
  See :doc:`transforms`.

:doc:`Export <exporting_to_usd>` uses the stored values, including these conversions.

Numeric scalars
~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 30 25 12 33

   * - USD type
     - dtype
     - ``is_array``
     - semantic
   * - ``bool``
     - ``{bool, 8, 1}``
     - no
     - ``NONE``
   * - ``uchar``
     - ``{uint, 8, 1}``
     - no
     - ``NONE``
   * - ``int``
     - ``{int, 32, 1}``
     - no
     - ``NONE``
   * - ``int64``
     - ``{int, 64, 1}``
     - no
     - ``NONE``
   * - ``uint``
     - ``{uint, 32, 1}``
     - no
     - ``NONE``
   * - ``uint64``
     - ``{uint, 64, 1}``
     - no
     - ``NONE``
   * - ``half``
     - ``{float, 16, 1}``
     - no
     - ``NONE``
   * - ``float``
     - ``{float, 32, 1}``
     - no
     - ``NONE``
   * - ``double``
     - ``{float, 64, 1}``
     - no
     - ``NONE``
   * - ``half2, half3, half4``
     - ``{float, 16, 2|3|4}``
     - no
     - ``NONE``
   * - ``float2, float3, float4``
     - ``{float, 32, 2|3|4}``
     - no
     - ``NONE``
   * - ``double2, double3, double4``
     - ``{float, 64, 2|3|4}``
     - no
     - ``NONE``
   * - ``int2, int3, int4``
     - ``{int, 32, 2|3|4}``
     - no
     - ``NONE``
   * - ``quath``, ``quatf``, ``quatd``
     - ``{float, 16|32|64, 4}``
     - no
     - ``QUATERNION``
   * - ``matrix2d``, ``matrix3d``, ``matrix4d``
     - ``{float, 64, 4|9|16}``
     - no
     - ``MATRIX``
   * - ``timecode``
     - ``{float, 64, 1}``
     - no
     - ``TIME_CODE``

USD roles change the semantic, not the storage type. For example, ``point3f``
uses ``{float, 32, 3}`` with ``POINT``. The corresponding roles are ``NORMAL``
for ``normal3f``, ``VECTOR`` for ``vector3f``, ``COLOR`` for ``color3f``, and
``TEXTURE_COORDINATE`` for ``texCoord2f`` and ``texCoord3f``.
``frame4d`` uses the same storage as ``matrix4d`` but has semantic ``FRAME``
instead of ``MATRIX``.

The scene authors ``float temperature = 20`` on ``/World/Body``. After selecting
that property and populating at ordinal 2, read its value:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-numeric

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-numeric-c

String-like scalars
~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 30 25 12 33

   * - USD type
     - dtype
     - ``is_array``
     - semantic
   * - ``token``
     - ``{uint, 64, 1}``
     - no
     - ``TOKEN_ID``
   * - ``string``
     - ``{uint, 64, 1}``
     - no
     - ``NONE``
   * - ``asset``
     - ``{uint, 64, 2}``
     - no
     - ``ASSET_PATH_ID``
   * - ``pathExpression``
     - ``{uint, 64, 1}``
     - no
     - ``PATH_EXPRESSION_STRING``

An ``asset`` stores an ``(authored, resolved)`` pair of identifiers. The resolved
value is whatever USD supplied to generic attribute population; that path does
not resolve the asset again. Rendering material inputs containing ``<UDIM>`` are
an exception: rendering derives a resolved pattern from a tile that resolves.
If no tile resolves, the resolved half is empty, even if USD supplied an anchored
pattern; the authored half is retained. A ``string`` has semantic ``NONE``, so
runtime semantic alone cannot distinguish it from ``uint64``. Consult the USD
type when that distinction matters.

The authored ``token state = "ready"`` becomes an identifier. Resolve it through
the path dictionary while the read group is alive:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-strings

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-strings-c

Arrays
~~~~~~

A USD array populates with ``is_array=True`` and its element's dtype and semantic:
``float3[]`` is ``{float, 32, 3}``, and ``matrix4d[]`` is ``{float, 64, 16}`` with
``MATRIX``.

``token[]``, ``string[]``, and ``pathExpression[]`` store one identifier per
element. ``asset[]`` stores one pair per element. Their tensor element counts
therefore match the USD array lengths.

Relationship targets use path identifiers with ``{uint, 64, 1}``,
``RELATIONSHIP_PATH_ID``, and ``is_array=True``. An authored relationship with
no targets is present as an empty value, rather than absent.

The scene authors two points in ``point3f[] points``. Read the array from the
one-prim query and view its three components per point:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-arrays

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-arrays-c

Empty Values
~~~~~~~~~~~~

Where the interned representation applies, empty tokens, strings, path
expressions, and either half of an asset pair use identifier 0. A text-backed
exception such as ``UsdRenderVar.sourceName`` uses a zero-length STRING value. Empty array elements keep their position. An authored empty value
is distinct from an absent property.

``opaque`` has no value to store and is skipped without a diagnostic.

The authored empty token ``emptyName`` is present with identifier 0:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-empty

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-empty-c

An authored ``float[] emptyArray = []`` also produces a read group. Its tensor
has zero elements; an absent property would produce no group:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-empty-array

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-empty-array-c

Properties That Do Not Follow Their USD Type
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The following schema properties use fixed runtime types so that population
domains agree when sharing a stage:

.. list-table::
   :header-rows: 1
   :widths: 34 20 24 22

   * - Property (where it applies)
     - USD type
     - populated as
     - instead of
   * - ``extent`` (any ``UsdGeomImageable``)
     - ``float3[]``, two points
     - ``{float, 64, 6}``, ``is_array=False``, ``NONE``
     - ``{float, 32, 3}`` array
   * - ``normals`` (``UsdGeomCurves``, ``UsdGeomPoints``)
     - ``normal3f[]``
     - ``POINT``
     - ``NORMAL``
   * - ``inputs:color`` (``UsdLuxLightAPI``), ``inputs:shadow:color`` (``UsdLuxShadowAPI``); ``inputs:shaping:focusTint`` (``UsdLuxShapingAPI`` on a ``DistantLight``, ``DiskLight``, ``RectLight``, ``SphereLight``, ``CylinderLight`` or ``DomeLight``)
     - ``color3f``
     - ``VECTOR``
     - ``COLOR``
   * - Scalar ``string`` attributes on ``UsdRenderSettings``, ``UsdRenderProduct``,
       or ``UsdRenderVar``, including ``sourceName`` and schema defaults. Excludes
       ``xformOp*`` and ``primvars:*``.
     - ``string``
     - ``{uint, 8, 1}``, ``is_array = true``, ``STRING``
     - interned ``{uint, 64, 1}``
   * - On ``UsdGeomCamera``: ``sensorModelPluginName``, ``sensorModelConfig``,
       ``sensorModelSignals``, and ``crossCameraReferenceName``. Also includes
       ``string`` attributes declared by applied ``OmniSensor*`` schemas on
       cameras or ``OmniSensor``-family prims, and by applied
       ``OmniLensDistortion*`` or ``PhysicalCameraResponsivityAPI`` schemas on cameras.
     - ``string``
     - ``{uint, 8, 1}``, ``is_array = true``, ``STRING``
     - interned ``{uint, 64, 1}``
   * - Scalar ``string`` attributes matching ``inputs:*`` or
       ``omni:simready:nonvisual:*`` on ``UsdShadeMaterial``, ``UsdShadeNodeGraph``,
       or ``UsdShadeShader``. Also ``config:*`` on ``UsdShadeMaterial`` and
       ``info:*sourceCode`` on ``UsdShadeShader``.
     - ``string``
     - ``{uint, 8, 1}``, ``is_array = true``, ``STRING``
     - interned ``{uint, 64, 1}``
   * - Scalar ``string`` primvars with a value, on any prim
     - ``string``
     - ``{uint, 8, 1}``, ``is_array = true``, ``STRING``
     - interned ``{uint, 64, 1}``
   * - ``semantic:<instance>:params:semanticType``, ``semantic:<instance>:params:semanticData`` (applied ``SemanticsAPI:<instance>``)
     - ``string``
     - ``{uint, 64, 1}``, ``TOKEN_ID``
     - ``{uint, 64, 1}``, ``NONE``
   * - ``omni:rtx:domeLight:size`` (``OmniFiniteDomeLightAPI``); ``nerf:crop:minBounds``, ``nerf:crop:maxBounds``, ``nerf:offset`` (``OmniNerfVolumeAPI``); ``omni:nurec:crop:minBounds``, ``omni:nurec:crop:maxBounds``, ``omni:nurec:offset`` (``OmniNuRecVolumeAPI``)
     - ``float3``
     - ``VECTOR``
     - ``NONE``

``extent`` and text stored as bytes change dtype or shape. The other exceptions
change only the semantic. Each rule applies only to the schemas or prim types
listed above. ``string[]`` attributes and primvars still store identifiers.

These schema-specific rules do not apply to arbitrary properties with the same
name. When rendering populates a non-schema ``color3f``, ``point3f``, ``normal3f``,
``vector3f``, or ``texCoord3f`` property, its authored role is preserved through
value edits. This does not depend on the property's ``custom`` flag. Schema
properties retain their required rendering representation; for example, a light's
``inputs:color`` still uses ``VECTOR`` even if authored with ``custom = true``.

If a value cannot use the required representation (for example, an authored
``token`` where a rule requires ``string`` bytes), population skips the value and
logs a warning. The attribute keeps its required type and its last written value;
this conversion mismatch does not fail the population operation.

Values Authored in Another Type Than Declared
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When an authored numeric value differs from its schema's declared type,
population tries to convert it to that type. Attributes without a schema
declaration use their authored type, subject to the representation rules above.

Conversion requires matching shapes: scalar to scalar, array to array, or
vectors with the same number of components. Supported types are:

* Scalars and their arrays: ``bool``, ``uchar``, ``int``, ``uint``, ``int64``,
  ``uint64``, ``half``, ``float``, and ``double``.
* Vectors and their arrays: 2, 3, or 4 components of ``int``, ``half``, ``float``,
  or ``double``, including role types such as ``point3f`` and ``color3f``.

Each component converts using USD's numeric conversion rules:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Authored as, declared as
     - Result
   * - integer or ``bool``, integer or ``bool``
     - the same value if it is in the declared type's range (``0`` or ``1`` for ``bool``);
       otherwise no conversion
   * - floating point, integer or ``bool``
     - the value truncated toward zero if that is in the declared type's range;
       otherwise, and for an infinity or NaN, no conversion
   * - any, floating point
     - the value rounded to the declared type. A value beyond the declared type's range
       becomes an infinity, and a nonzero value too small for it becomes zero.
       Infinities and NaN convert unchanged.

Every array element and vector component must convert successfully. If conversion
fails, population skips the whole value and logs a warning; the attribute keeps
its declared type and its last written value. Shape mismatches and nonnumeric
type mismatches involving ``token``, ``string``, or ``asset`` follow the same rule.
The mismatch does not fail population, so consumers must not assume that a
successful population operation updated every value.

This differs from re-authoring the property's type: population replaces a column
when its declared type or role changes, using ``FORCE`` attribute replacement.
An unconvertible value does not trigger that replacement.

Read ``extent`` to see the fixed-size representation of its two bounds:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-extent

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-extent-c

Stage Metadata
--------------

Population writes requested stage metadata to ``/`` as attributes named
``usd-metadata:<path>``. Domains may request fields; descriptions request them
through ``stage_metadata_paths``. Unrequested fields are not populated.

A metadata path names a field and optional dictionary keys separated by ``:``:

* ``timeCodesPerSecond`` selects one field.
* ``customLayerData:myTool:version`` selects a nested value.
* ``customLayerData`` selects all values in that dictionary, each stored under
  its own full path.

Missing paths produce no value. Leading, trailing, or repeated ``:`` characters
create empty components and are rejected.

The description above requests ``timeCodesPerSecond``. Read the field from
the root path. This scene uses 24 time codes per second:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-stage-metadata-read

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-stage-metadata-read-c

Prim and Property Metadata
--------------------------

Selectors use ``prim_metadata_paths`` and ``property_metadata_paths`` to request
metadata for selected prims and properties. They use the same path syntax:

* Prim metadata becomes ``usd-metadata:<path>``.
* Property metadata becomes ``<property>:usd-metadata:<path>``.

:meth:`has_metadata() <ovstage.population.PrimPredicate.has_metadata>` matches the presence of any requested path, including a value
of ``False``. It can be used in prim or property conditions. A dictionary field
matches when any value beneath it resolves, including schema-provided metadata.
Name a specific key, such as ``customData:myTool:export``, when that is what you
need to select.

The selector above requests the prim label and each selected property’s
``customData:unit``. Read the populated temperature unit:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-property-metadata-read

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-property-metadata-read-c

The same selector requested ``customData:label`` on the prim itself. Read that
value without the property-name prefix:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-prim-metadata-read

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-prim-metadata-read-c

Reserved Prim Attributes
------------------------

An ordinary populated prim with a USD type name carries ``usd-prim-type``. A typeless prim
is still populated but has no ``usd-prim-type``; there is no synthetic type name.
These metadata rules also apply when a selector requests no properties.

Outside ``RENDERING``, ``usd-schemas`` contains the applied API names, or an empty
set when none apply. ``HAS`` therefore matches even an empty set; use
``CONTAINS`` to select a particular API. ``RENDERING`` also adds the schemas and
tags described below, including on geometry without authored APIs; it can omit
``usd-schemas`` when neither applied APIs nor rendering tags are present.

A multiple-apply schema records both its instance name and bare name. For
example, ``CollectionAPI:parts`` also adds ``CollectionAPI``. Typed export skips
that accompanying bare name rather than treating it as an additional API instance.

For ordinary prims populated through selectors or ``PHYSICS``, these names
come from USD's type token and composed applied-API list. Use those identifiers,
rather than C++ or Python class names:

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - USD declaration
     - Runtime name
   * - ``def Cube "Body"``
     - ``usd-prim-type`` contains ``Cube``.
   * - ``apiSchemas = ["PhysicsRigidBodyAPI"]``
     - ``usd-schemas`` includes ``PhysicsRigidBodyAPI``.
   * - ``apiSchemas = ["CollectionAPI:parts"]``
     - ``usd-schemas`` includes ``CollectionAPI:parts`` and ``CollectionAPI``.

For example, use ``Cube``, not ``UsdGeomCube`` or ``UsdGeom.Cube``, as the type
name in a population predicate. The applied-API list is composed by USD, so it
can include APIs supplied by schema definitions as well as those written in
an ``apiSchemas`` statement. Do not rely on its runtime array order.

``usd-ancestor-types`` lists the USD types the prim derives from, excluding
its own type. These are schema ancestors, not parent prims: a ``Mesh`` includes
``PointBased``, ``Gprim``, ``Boundable``, ``Xformable``, ``Imageable``,
``Typed``, ``SchemaBase``, and ``TfType::_Root``.
Use ``CONTAINS`` to select prims by a base type. Typeless and unknown types
have no ancestor set in non-rendering population, even if ``fallbackPrimTypes``
provides a fallback. Live type edits update or remove the set with the prim type.

Outside authored ``PointInstancer`` descendants, instance roots use the following
metadata in every population domain, with the ``RENDERING``-only exception below:

- ``usd-prim-type`` is ``XformInstance``, or ``ScopeInstance`` when it is a ``Scope``,
  whatever its authored type.
- ``usd-schemas`` also carries ``GeomInstanceAPI``, and ``GeomInstanceRootAPI`` when the
  instance root is outside a prototype. No USD schema applies these names.
- ``usd-ancestor-types`` is ``Xformable`` alone.

With ``RENDERING`` alone, a ``Scope`` and a non-``Scope`` instance sharing a prototype
may both receive the instance type selected from the first instance encountered.
``PHYSICS`` and selector population classify each root separately, including when
combined with ``RENDERING``. Consumers must therefore not assume identical
``usd-prim-type`` values across these modes for such a shared prototype.

When ``PHYSICS`` or a selector populates an instanceable prim authored beneath
a ``PointInstancer``, it keeps its authored type and applied schemas, including
alongside ``RENDERING``. At that authored path it receives neither instance tag
nor the rendering-derived tags listed below. It still links its prototype and
remains discoverable through the :doc:`instancing` queries. Without ``RENDERING``,
it also receives its normal USD ancestor set. With ``RENDERING`` selected,
population does not synthesize ``usd-ancestor-types`` at these authored paths;
use their type or APIs rather than relying on an ancestor set. Rendering uses
relocated prototype geometry, whose metadata is a separate representation.

An instance root's authored properties are populated as for any other prim. A root
populated by ``PHYSICS`` or a selector also carries its composed authored type
as ``usd-metadata:typeName``, even when the root itself matches no selector.

Every prim the ``PHYSICS`` domain populates also carries ``usd-metadata:typeName``: its
composed USD type name, as authored, including a type no loaded schema declares.
This includes structural ancestors populated to reach physics content. On an
instance root it is the authored type, not ``XformInstance`` or ``ScopeInstance``. Live
edits update it, and a prim that loses its type name loses the attribute. A prim with no
type name carries none. A desc selector publishes the same attribute when it selects the
``typeName`` metadata path.

Wherever the ``PHYSICS`` domain or a selector publishes a ``PointInstancer``'s
``prototypes`` relationship, population also publishes its authored targets, in authored
order, as ``prototypes:usd-targets``: the list ``protoIndices`` indexes, whichever domains are
selected. Live edits update it, including an edit that clears the targets.

When the ``RENDERING`` domain is selected, the following hold for every prim it populates,
including prims a selector or the ``PHYSICS`` domain also populates:

- A ``PointInstancer``'s ``prototypes`` lists the geometry ``RENDERING`` renders for its
  prototypes, at paths beneath the instancer, not the authored relationship's targets.
  ``protoIndices`` does not index that list. The authored targets are in
  ``prototypes:usd-targets`` when ``PHYSICS`` or a selector also publishes the instancer's
  ``prototypes``. Its ``extent`` is the bounds of those prototypes, not the authored
  ``extent``.
- ``usd-schemas`` includes ``MaterialBindingAPI`` on supported geometry and ``DomeLight``, even
  without an authored material binding. This is a recognized USD API and typed
  export applies it. Instance roots are excluded; material-binding subsets are
  included only under a non-instance ``Mesh``.
- ``usd-schemas`` also carries rendering tags that are not applied USD APIs:

  - ``PrimvarsAPI`` on a prim with a primvar that has a value, other than a string array,
    or with an authored ``normals`` attribute.
  - ``MeshSubdivision`` on a ``Mesh`` whose subdivision scheme, boundary or face-varying
    interpolation, or triangle subdivision rule differs from its default, or that has
    creases or corners, and on every ``TetMesh``.
  - ``SkelBindingAPI_Geom`` on a ``Mesh`` outside a prototype, or on curves, with a
    ``primvars:skel:jointIndices`` attribute.
  - ``SkelBindingAPI_Skel`` on a non-instance ``Skeleton``.
  - ``GeomPrototypeAPI`` on supported geometry inside a USD prototype, excluding
    the prototype root and instance roots.
  - Light-attribute tags such as ``LightVisibleInPrimaryRayAPI``,
    ``LightGhostFactorAPI``, ``LightDisableFogInteractionAPI``,
    ``LightIsProjectorAPI``, ``LightProjectorDirectionTextureAPI``,
    ``LightEnableCausticsAPI``, ``LightUsdLuxVersionAPI``, ``DomeLightFiniteAPI``,
    and ``LightShapingForwardAxisAPI``, when their rendering attributes apply.

These tags are preserved when ``RENDERING`` is combined with selectors or
``PHYSICS``. They are not added at authored paths beneath a ``PointInstancer``.

Rendering population supplies the ancestor set when selected, including alongside
selectors, except at the authored ``PointInstancer`` descendant paths described
above. Its other current exceptions are:

* Typeless prims carry ``Imageable``.
* Instance proxies carry only ``Xformable`` (instance-root rules are above).
* ``GeomSubset`` and USD prototype prims carry no ancestor types.
* Unknown types resolved to geometry through ``fallbackPrimTypes`` carry that
  fallback's ancestors. With rendering alone the prim type is also the fallback;
  a selector can retain the source type name instead.
* ``LODSelector``, which has no USD schema, carries ``Imageable``.

Writing ``usd-prim-type`` does not derive a new ancestor set. If the new type
already appears in ``usd-ancestor-types``, it is removed from that set because a
prim's own type is excluded. When changing a type directly, also write its
matching ancestor set. Both this set and ``usd-schemas`` are replaced as a whole
by a write. Typed export does
not author ``usd-ancestor-types``. Custom properties named after USD types or
API schemas, such as ``Boundable`` on a Mesh, are unsupported.

.. _population-instance-types:

USD scene-graph instance roots use ``XformInstance`` or ``ScopeInstance`` as
described above. With ``RENDERING`` population (including ``ALL``), their
populated contents can also use types such as ``MeshInstance`` and
``CubeInstance``: a ``Mesh`` beneath an instance root can become ``MeshInstance``. You author ordinary USD prims
with references and ``instanceable = true``; population generates these runtime
types. You do not author ``MeshInstance`` or ``XformInstance`` in USD.

A ``PointInstancer`` does not generate one such prim per point. Instanceable
prims authored beneath it keep their authored metadata as described above;
rendering's relocated prototype geometry is a separate representation.
See :doc:`instancing` for scene-graph instancing examples and queries.

Consequently, a runtime ``usd-prim-type`` value can differ from the source USD
type. Population predicates inspect the source USD prim; runtime queries
inspect the populated stage.

Read the populated type of ``/World/Body`` and resolve its token:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-reserved

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-reserved-c

Read ``usd-schemas`` to find the applied APIs. This scene applies
``PhysicsRigidBodyAPI`` to ``/World/Body``:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-schemas

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-schemas-c

.. _population-runtime-owned-prims:

Runtime-Owned Prims
-------------------

Rendering population keeps bookkeeping prims alongside the scene. They are not
USD content and remain after :func:`reset_usd() <ovstage.population.reset_usd>` or removal of a reference. Unfiltered
queries count them too. Other population domains do not create these prims.

To enumerate scene content, filter by its root path or expected types; see
:doc:`queries`. Their paths, types, and count are implementation details. Do not
write to them.

Path Conditions
---------------

:meth:`has_path() <ovstage.population.PrimPredicate.has_path>` and :meth:`is_under_path() <ovstage.population.PrimPredicate.is_under_path>` accept absolute prim paths, including
``/``. Property, target, and variant-selection paths fail at submission. Neither
condition matches prims inside a native instance.

For an exact match, use :meth:`has_path() <ovstage.population.PrimPredicate.has_path>` as the selector’s prim condition:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-paths

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-paths-c

To select Cube prims below ``/World``, combine a subtree condition with a type
condition. This example also includes the ``PHYSICS`` domain: the selector adds
``size`` to the domain’s selection. It runs at ordinal 7, after the time-update
examples below have used ordinals 3 through 6:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-subtree

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-subtree-c

.. _population-ancestors-transforms:

Ancestors and Transforms
------------------------

The ``PHYSICS`` domain and explicit selectors also populate ancestors of selected
prims so consumers can follow the hierarchy. An ancestor that no selector matches
gets the reserved attributes above and, if USD-xformable, transform data.
It does not receive other attributes, relationships, or metadata, except
``usd-metadata:typeName`` on ancestors populated by ``PHYSICS`` and on instance
roots populated by ``PHYSICS`` or a selector.

For each USD-xformable prim selected this way, including ancestors, population
writes:

* ``omni:xform`` — composed local USD transform, stored as a row-major 4×4
  ``float64`` matrix with 16 lanes and semantic ``MATRIX``.
* ``omni:resetXformStack`` — a boolean indicating whether the prim ignores its
  parent's transform.

When ``RENDERING`` is not selected, population also creates
``omni:fabric:worldMatrix`` if absent, initially using the local matrix. Later
world-transform values come from :meth:`ovstage.Stage.compute_hierarchy()`;
population does not overwrite them. A prim that stops being xformable loses this
attribute. When ``RENDERING`` is selected, that domain manages it instead.

Treat the provisionally named world-matrix attribute as read-only output.
Hierarchy computation updates existing rows but does not create them.
Non-xformable prims receive none of these derived attributes, and being
xformable does not by itself select a prim for population. See :doc:`transforms`.

These names belong to population. A USD property named ``omni:xform``,
``omni:resetXformStack``, or ``omni:fabric:worldMatrix`` is never
published, even when a selector names or matches it, on any prim.

Selecting ``/World/Body`` also populates the transform of its ancestor
``/World``. Its authored translation is 10 along X:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-ancestor

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-ancestor-c

Rebuilding from the Current USD Source
--------------------------------------

:func:`population.repopulate_usd_with_desc() <ovstage.population.repopulate_usd_with_desc>`
rewrites selected attributes from the current USD source at the supplied
:term:`ordinal`. Use it to replace runtime edits with USD values. Pass ``time``
in seconds, or :data:`ovstage.population.DEFAULT_TIME` for default opinions.
Use :func:`population.repopulate_usd_with_desc_async() <ovstage.population.repopulate_usd_with_desc_async>`
for asynchronous execution.

Repopulation keeps unselected prims and attributes, including runtime-created
data. Delete them explicitly if they must be removed. The supplied descriptions
also set the population scope for later updates.

The USD stage, root and session layers, edits, composition state, edit target,
and reference handles stay valid.

For incremental updates, use
:func:`population.apply_usd_changes() <ovstage.population.apply_usd_changes>` or
:func:`population.update_from_usd_time() <ovstage.population.update_from_usd_time>`. These
rewrite changed or time-sampled USD values, so they can leave runtime edits in
place on other attributes.

Editing the USD Source
----------------------

Reference edits change the USD source first. Apply them to the runtime stage
at a new ordinal:

* :func:`population.add_usd_reference() <ovstage.population.add_usd_reference>` or :func:`add_usd_reference_from_string() <ovstage.population.add_usd_reference_from_string>` adds
  a reference. An absolute-root target adds a sublayer; a prim target adds a
  reference at that prim.
* :func:`population.remove_usd() <ovstage.population.remove_usd>` removes a reference by its returned handle.
* :func:`population.reset_usd() <ovstage.population.reset_usd>` clears the USD source.
* :func:`population.apply_usd_changes() <ovstage.population.apply_usd_changes>` transfers pending edits to the runtime stage.

The reference target decides what the next apply republishes. A
not-yet-existing prim path republishes only the new prim and the content
beneath it. An existing prim path republishes that prim and every prim beneath
it, including prims the reference does not add or change; the absolute root
republishes the whole stage. Every selected attribute of a republished prim is
written at the apply ordinal even when its value is unchanged, so a read of
that ordinal range reports it as changed, and the written value replaces any
value written directly into the runtime stage since. To add content without
republishing existing prims, target a not-yet-existing prim path.

.. note::

   Clearing all USD values of a populated application attribute, with no fallback
   remaining, removes its runtime column on the next ``apply_usd_changes``.
   With ``RENDERING`` enabled, renderer-retained columns are exceptions: schema
   attributes, primvars, properties of shader and node-graph prims, and authored
   properties of ``RenderSettings``, ``RenderProduct``, and ``RenderVar`` prims.

These examples update the USD source, apply its changes, and read the result:

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

Advancing Time
--------------

Use :func:`ovstage.population.update_from_usd_time()` to sample animated USD
values at a new time and write them at a new ordinal. It uses the domains and
selectors from the opening call, including selectors with ``domains=NONE``.
Edits to time samples are picked up without a separate :func:`apply_usd_changes() <ovstage.population.apply_usd_changes>`.

The ``time`` argument is in **seconds**, converted using the stage's
``timeCodesPerSecond``. Pass ``population.DEFAULT_TIME`` for USD's default time
code; it is also the default for the basic open functions. The ``_with_desc``
functions require an explicit time.

``DEFAULT_TIME`` is NaN, so test it with :func:`population.is_default_time() <ovstage.population.is_default_time>` rather
than ``==``. In C, use ``OVSTAGE_POPULATION_DEFAULT_TIME`` and
:c:func:`ovstage_population_is_default_time() <ovstage_population_is_default_time>`.

Reapplying the same time rewrites in-scope time-sampled properties in every
domain, including at default time. It is not a no-op.

Use :func:`apply_usd_changes() <ovstage.population.apply_usd_changes>` for structural edits and changes to properties that
are not time samples. The ``RENDERING`` domain currently propagates these during
time updates too; other domains do not. Do not rely on that difference.

The example temperature has samples of 20 at USD time code 0 and 30 at
time code 24. Sample one second at ordinal 3, then read 30:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-time

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-time-c

Python ↔ C Name Mapping
-----------------------

Several Python wrappers use shorter names than their C entry points:

.. list-table::
   :header-rows: 1

   * - Python (``ovstage.population``)
     - C entry point
   * - :func:`open_usd <ovstage.population.open_usd>` / :func:`open_usd_async <ovstage.population.open_usd_async>`
     - :c:func:`ovstage_population_open_usd_from_file <ovstage_population_open_usd_from_file>`
   * - :func:`add_usd_reference <ovstage.population.add_usd_reference>`
     - :c:func:`ovstage_population_add_usd_reference_from_file <ovstage_population_add_usd_reference_from_file>`
   * - :func:`remove_usd <ovstage.population.remove_usd>`
     - :c:func:`ovstage_population_remove_usd_reference <ovstage_population_remove_usd_reference>`
   * - :func:`update_from_usd_time <ovstage.population.update_from_usd_time>`
     - :c:func:`ovstage_population_apply_usd_time <ovstage_population_apply_usd_time>`
   * - :func:`open_usd_with_desc <ovstage.population.open_usd_with_desc>`
     - :c:func:`ovstage_population_open_usd_from_file_with_desc <ovstage_population_open_usd_from_file_with_desc>`

.. _population-ordinal-ownership:

Choosing Ordinals
-----------------

The application assigns ordinals to population operations and seals completed
updates with ``advance_write_floor``. Population never chooses, increments, or
seals an ordinal for you. A tick may use several ordinals; seal through the final
one once its updates complete. An operation at or below an applicable floor is
a write-floor violation under every admission policy.

Python requires an explicit ``ordinal`` for ``open_usd`` (file/URL),
``open_usd_from_string``, ``apply_usd_changes``, and their ``_async`` variants.
Pass it positionally or as ``ordinal=...``. The former
implicit value of 1 has been removed; calls that omit it raise ``TypeError``.
The ``_with_desc`` and ``update_from_usd_time`` helpers already require an
explicit ordinal.

A newly created instance already has a write floor of 0, so the first population
must use an ordinal of at least 1 — 0 is never a usable *write* ordinal, even
though it is a perfectly valid ordinal to read at. An application whose frame
counter starts at 0 should offset it rather than pass 0 through.

Use increasing ordinals for successive updates to the same data. For example,
open at ordinal 1, propagate later USD edits with ``apply_usd_changes`` at 2,
and update time-sampled properties with ``update_from_usd_time`` at 3. Wait for
each operation before starting the next, then seal through ordinal 3. Use higher
values when other writers or floors have already advanced the stage.

If you apply direct overrides after population, the same sequence applies:
populate at 1, wait, write the overrides at 2, wait, then seal through 2.
See :ref:`ordinal-admission-policies` for the policy options and their
configuration.

After using ordinals 1 through 3, start the next updates at 4 even though the
frame counter starts at 0:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-ordinals

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-ordinals-c

Repopulating a Stage
--------------------

Opening a new USD source adds or updates its data without removing values left
by the previous source. Prims missing from the new scene remain, as do old
attributes that the new scene no longer publishes.

To replace previously populated content:

1. Call :func:`population.reset_usd(stage) <ovstage.population.reset_usd>` to clear the USD source.
2. Call :func:`population.apply_usd_changes(stage, ordinal=n) <ovstage.population.apply_usd_changes>` to remove its populated
   content. Prims created only by direct application writes remain.
3. Call :func:`population.open_usd(stage, path, ordinal=n + 1) <ovstage.population.open_usd>` to load the replacement.

Wait for each step before starting the next. The blocking Python functions do
this; with asynchronous calls or C, observe each operation's completion.
The following examples clear and reload a small Cube scene. ``CUBE_USDA`` in
Python and ``kCubeUsda`` in C hold the scene text. Their test helpers count prims
under ``/World`` to check removal:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population.py
         :language: python
         :start-after: # [snippet:reset-usd]
         :end-before: # [/snippet:reset-usd]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population.cpp
         :language: cpp
         :start-after: // [snippet:reset-usd-c]
         :end-before: // [/snippet:reset-usd-c]
         :dedent:

To clear the whole stage, including application-created prims, use an unfiltered
:meth:`stage.query() <ovstage.Stage.query>`, wait, then call :meth:`stage.delete_attributes(query, [], n) <ovstage.Stage.delete_attributes>`.
An empty attribute list deletes the selected prims. Wait for deletion and release
the query before opening the new source. In C, the same operations are
:c:func:`ovstage_query() <ovstage_query>` with no filter and :c:func:`ovstage_delete_attributes() <ovstage_delete_attributes>` with no
attribute names, followed by :c:func:`ovstage_release_query() <ovstage_release_query>`.

Use a fresh ordinal for deletion and another for population. Wait for deletion
to complete before starting population.

Failure and the Ordinal
-----------------------

A submission failure means the operation never ran. It leaves previously
populated content unchanged, so the same ordinal can be retried if it still
satisfies current watermarks, admission policy, and stage state. In C, this is
the enqueue result's ``status``.

A failure while waiting may follow partial writes or removal of previous
content. Retry population at a higher ordinal. Python raises ``OvstageError``;
in C, inspect :c:func:`ovstage_population_get_last_error() <ovstage_population_get_last_error>` after the failed
:c:func:`ovstage_population_wait_op() <ovstage_population_wait_op>`. The status code alone does not show how much
work completed.

This example deliberately opens a missing file, distinguishes rejection from
failure during execution, and retries with the valid scene:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/population/main.py
         :language: python
         :snippets: population-failure

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/population/main.cpp
         :language: cpp
         :snippets: population-failure-c

Further Reading
---------------

- :doc:`/guides/runtime_loop` — the full populate → read → update → read loop.
- :doc:`exporting_to_usd` — author selected runtime state back into a USD layer.
- :doc:`reading_attributes` — reading ``usd-prim-type`` and other populated metadata back.
- :doc:`instancing` — discovering instance and prototype roots in populated content.
