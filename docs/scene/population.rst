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

A population operation runs on one thread by default. The process configuration
can allow it more threads without changing the populated data; see
:ref:`configuring-population-threads-python` or
:ref:`configuring-population-threads-c`.

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

Context-Dependent Resolvers
---------------------------

Choose the resolver before first USD use
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

OpenUSD routes every identifier to one *primary* resolver, except identifiers in
a URI scheme that another resolver plugin claims. Which resolver is primary is
the host's choice (``ArSetPreferredResolver``; without it, OpenUSD takes the
first resolver plugin it discovers), and OpenUSD fixes the choice the first time
anything in the process calls ``ArGetResolver()``; ``ovstage_create_instance``
can be that first call, so select the primary resolver before creating the first
instance.

One caveat in this release: the bundled Omniverse Nucleus resolver
plug-in, which the population runtime loads at instance creation unless the host
has loaded it already, selects itself the same way, so a host that makes no
resolver access of its own before creating its first instance gets that
selection instead; any resolver access before instance creation (binding a
context is one) fixes the host's choice first. URI-scheme routing is the
alternative for a resolver that serves only its own schemes and leaves the
primary resolver in place; both models share the limits listed below.

Bind the context before creating the instance
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

ovstage is indifferent to which model the host uses: what it captures and re-binds is the
resolver *context*. A *context-dependent* resolver — one that resolves nothing
without a bound ``ArResolverContext``, whether it is the primary resolver or
claims a scheme — needs its context in place **before the instance is created**.
The host either binds the context on the thread that calls
``ovstage_create_instance`` (``ArResolverContextBinder``, the way a host binds
its resolver context around instance creation) or configures the resolver's
default context; the instance's backing stage captures what it finds at
creation: the context bound on the calling thread, merged with the one OpenUSD
provides by default (``ArResolver::CreateDefaultContext()``) with the bound
objects taking precedence per context type, or the default context alone when
nothing is bound. ovstage binds that captured context around every layer open it
performs on its population worker (``open_usd`` /
``ovstage_population_open_usd_from_file`` and ``add_usd_reference`` /
``ovstage_population_add_usd_reference_from_file``), where the Ar context stack
would otherwise be empty.

Relative paths keep their layer anchor
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A relative local path (no URI scheme) that exists
relative to the process working directory is opened by its absolute path, the
location OpenUSD's default resolver resolves it to; a resolver that keeps an
absolute input absolute then reports an absolute resolved path, and the layer's
own relative references (textures, sublayers) keep their anchor. The working
directory is the one at the time of the call. File-format arguments on the
identifier and a package-relative suffix are kept across the anchor (for
``pkg.usdz[inner.usda]`` the outer package is the path that must exist), whether
the path exists is checked when the worker opens the layer, and a failed open of
the anchored path is reported with the absolute path. Other relative paths reach
the resolver unchanged, as does, on Windows with OpenUSD releases before 26.08,
a path whose absolute form is 260 characters or more.

Context lifetime and Python hosts
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A context bound, or a
resolver configured, only afterwards is seen by instances created afterwards;
instances created under the previous configuration keep resolving through it for
their lifetime, so destroy an instance before releasing the resolver runtime its
context refers to.

Python hosts cannot bind an ``ArResolverContext`` through the
``ovstage`` package (it carries no ``pxr``): bind through the resolver's own
Python entry point (the ovstorage USD resolver's ``Runtime.bind()``) before
creating the ``Stage``, or install the resolver's process default (not in the
resolver's v0.1.0; available from its 0.2 release); the resolver plugin must be
loaded into the same OpenUSD the ``ovstage`` package uses. Local behavior is
unchanged either way.

Process-wide limits
~~~~~~~~~~~~~~~~~~~

Two limits of that isolation, both inherited from OpenUSD:

- Resolver selection happens once. The primary resolver is fixed at the
  process's **first** ``ArGetResolver()`` call by any library, and the
  URI-scheme dispatch table is built at that moment from the resolver plugins
  discoverable then; a URI resolver whose plugInfo becomes discoverable later
  never receives its schemes. Select the primary resolver and make every
  resolver plugin discoverable before the first USD access. The first instance
  creation in a process logs one warning on the ``omni.ovstage`` channel when
  a resolver plugin declares URI schemes the table does not route; a primary
  resolver declares no schemes, so the warning concerns URI-scheme resolvers
  only.
- Isolation is enforced per resolver access. OpenUSD keeps one process-wide
  layer registry, so a layer a live instance already has open is reused —
  without a resolver access — by an instance created later under a different
  configuration. Destroy the instances that opened a layer before expecting a
  reconfiguration to fetch it again.

Once no other live instance shares a layer it opened (previous bullet: a reused
layer keeps the context it was first opened under until its last holder is
destroyed), destroying an instance releases the resolver context it captured:
nothing ovstage keeps outlives the instance, and OpenUSD's schema registry no
longer does either. OpenUSD builds that registry once per process, on the first
schema read, and keeps every schema definition layer until process exit, each
storing a clone of the resolver context bound where it was created; ovstage
performs that first read itself, under an empty resolver context, at the point
where it closes schema registration (the first population or export in the
process, see :func:`ovstage.population.register_usd_schemas()`), so no schema
layer holds a clone of an instance's context, of a host context bound on the
calling thread, or of the store a context-dependent resolver carries in it.

Failure diagnostics are classified in
``ovstage_population_get_last_op_error``: an unregistered URI scheme (the
primary resolver handled the identifier and could not open it), or a registered
scheme whose resolver still could not open the layer. In either branch the most
common cause is a context-dependent resolver — the primary resolver or the
scheme's — whose context was not in place when the instance was created; the
other is an object that is not a readable USD layer. Unresolved sublayers,
references, and payloads discovered during composition are reported once per
wholesale population (open or repopulate) as an aggregated warning — or fail the
op when the ``OVSTAGE_CONFIG_POPULATION_STRICT_RESOLUTION`` process config key
is set (``StageConfig(population_strict_resolution=True)`` in Python), so
automated tests need no log scraping. A strict failure lists up to three of the
unresolved identifiers (plus a count of the rest) after the population has
completed, so the instance holds the same
consistently populated (partially resolved) content the default path produces.
``add_usd_reference`` reports only the classified open failure of the
referenced layer itself; it performs no aggregated dependency check.

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
     - Scene data for rendering, including meshes, lights, materials, and cameras,
       with their transforms and hierarchy. See :ref:`population-rendering-selection`.
   * - ``NONE``
     - No predefined selection. Use explicit selectors to choose the prims and
       properties to populate.

Selecting a domain loads data; it does not start a renderer. A bit that names no
domain is ignored. Other data, such as physics, is populated through explicit
selectors, which add to the domain's selection, as described below.

A domain selects prims and properties, not a category of prims. A prim can be
populated by several domains and descriptions, and each adds only the properties
it selects. ``RENDERING`` populates transformable prims even when they carry no
rendered data, so a rigid body on an ``Xform`` is present after a
``RENDERING``-only open. Its ``usd-schemas`` lists the physics APIs, but none of
their properties are populated.

Population itself has no notion of what a library needs. The library or application
that consumes the data owns that knowledge and states it as population descriptions:
selectors choose the prims and properties, and derived properties compute the values
USD does not author, from expressions. ``RENDERING`` is the last predefined domain and
will be removed in a future release; describe new data with descriptions rather than
relying on a domain.

The examples below use ``scene.usda`` from the
:doc:`population example </examples/population>`. It contains a rigid-body Cube
at ``/World/Body`` below an Xform at ``/World``. Load the prims that apply a physics
API, with their ``physics`` properties, first.
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

``HAS_PROPERTY`` selects a prim by the properties it carries. Inside it, ``TARGETS``
selects a relationship by the prims it points at, so together they select a prim by
what its relationships point at. A target that names a property path counts as the prim
carrying it, and a target population does not walk (inactive, undefined, abstract,
unloaded or inside an instance) matches nothing. An attribute never matches, even one
with connections: connections are not targets.
``TARGETS`` is accepted only inside ``HAS_PROPERTY``; as a selector's own property
predicate it is rejected at submission.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_descriptions.py
         :language: python
         :start-after: # [snippet:select-by-relationship-target]
         :end-before: # [/snippet:select-by-relationship-target]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_descriptions.cpp
         :language: cpp
         :start-after: // [snippet:select-by-relationship-target-c]
         :end-before: // [/snippet:select-by-relationship-target-c]
         :dedent:

After ``apply_usd_changes``, a prim is selected as a fresh population would select it,
including after changes to its targets or to the prims above them.

``HAS_DESCENDANT`` selects a prim by what lies below it: it matches when any
descendant matches its subpredicate. The descendants are the prims population walks,
and an instance's descendants are its prototype's content, so a match inside a native
instance counts. A prim is not its own descendant.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_descriptions.py
         :language: python
         :start-after: # [snippet:select-by-descendant]
         :end-before: # [/snippet:select-by-descendant]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_descriptions.cpp
         :language: cpp
         :start-after: // [snippet:select-by-descendant-c]
         :end-before: // [/snippet:select-by-descendant-c]
         :dedent:

After ``apply_usd_changes``, such a prim is selected as a fresh population would select
it, including after changes below it or inside the prototypes it reaches.

``IS_INSTANCE`` matches a native instance: a prim USD instances because it is
``instanceable`` and has composition arcs, including an instance nested inside a
prototype. Together with ``HAS_DESCENDANT`` it selects the instances of the prototypes
that hold something:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_descriptions.py
         :language: python
         :start-after: # [snippet:select-instances]
         :end-before: # [/snippet:select-instances]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_descriptions.cpp
         :language: cpp
         :start-after: // [snippet:select-instances-c]
         :end-before: // [/snippet:select-instances-c]
         :dedent:

After ``apply_usd_changes``, a prim that starts or stops being an instance is selected
as a fresh population would select it.

``IS_TARGETED_BY`` matches a prim that a relationship points at. The relationship must
match its property predicate, and the prim carrying it must match its prim predicate. A
target naming a property points at the prim that owns the property:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_descriptions.py
         :language: python
         :start-after: # [snippet:select-targeted-prims]
         :end-before: # [/snippet:select-targeted-prims]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_descriptions.cpp
         :language: cpp
         :start-after: // [snippet:select-targeted-prims-c]
         :end-before: // [/snippet:select-targeted-prims-c]
         :dedent:

Only relationships on prims population walks count: active, defined, loaded and not
abstract. A relationship inside a native instance counts once, as authored in the
prototype, not once per instance.

``IS_TARGETED_BY`` asks only about the prim being matched. It may be the whole prim
predicate of a selector or a derived property, or sit inside ``AND``, ``OR`` and ``NOT``,
at any depth. Anywhere else, including the prim predicate of an expression, submission
fails as not supported.

Its two predicates may not contain ``IS_TARGETED_BY``, ``TARGETS`` or
``HAS_DESCENDANT``. Submission fails as not supported if they do.

After ``apply_usd_changes``, a prim is selected as a fresh population would select it,
including when the prims pointing at it or their relationships changed.

Unsupported kinds, cycles, or graphs exceeding the depth or node limits fail
at submission with a diagnostic. In C, predicate nodes and string arrays are
borrowed and must remain valid through the call. Python builders retain them.

For the remaining examples, select ``/World/Body`` and name the properties to
populate. The description also requests metadata, explained below. Populate
these values at ordinal 2, after the physics load at ordinal 1:

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

For explicit selectors, population reads the selected properties through USD:

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
   * - ``extent`` (any ``UsdGeomImageable``; any ``ParticleField`` or
       ``ParticleField3DGaussianSplat`` prim, whether or not its schema is
       registered)
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

.. _population-derived-properties:

Derived Properties
------------------

Everything above is *read* off a prim. A selector can also *compute* a property: a
**derived property** (:class:`ovstage.population.DerivedProperty`,
``ovstage_population_derived_property_t`` in C) names a column and gives an **expression**
(:class:`ovstage.population.Expression`, ``ovstage_population_expression.h`` in C) for its
value. Population
evaluates it on each prim the selector selects, narrowed by the rule's own prim predicate,
and publishes the result under that name, in place of any value read under it.

The expression kinds yield a **number**, a **bool**, a **vector**, a **token** or
**paths**, computed from literals, stage metadata (``STAGE_METADATA``), the stage's up
axis (``UP_AXIS``), addition, subtraction, multiplication and division, and what USD
resolves for the prim: its bound material, the strength and purpose of the binding that
resolves it, and its collections. They read no property of the prim. The set of kinds is
open; the column a rule publishes is not limited to what they yield today.

**Names.** A derived column is not a USD property, so its name may hold what a property
name cannot, such as ``-``, which keeps it apart from any property a prim authors. It may
not be empty, start or end with ``:``, hold ``::``, or contain whitespace. It may not be a
transform column population derives itself, or another column population or the stage
publishes itself: a name starting with ``usd-`` or holding ``:usd-metadata:`` is rejected,
also where an instance name would make it one.

**When.** A rule's ``when`` decides, from the rule's own property on the prim, whether it
publishes there:

- ``ALWAYS``: on every prim the rule applies to.
- ``WHEN_UNAUTHORED``: where nothing is authored. The derived value stands in for the
  schema fallback, or is added where nothing declares the property; what is authored is
  read as usual.
- ``WHEN_ABSENT``: where the prim has no such property at all.

For an attribute, authored means an authored default or time samples, and a value block
counts as no value; for a relationship, authored targets.

**Type.** A derived column is an attribute or a relationship; which one follows from its
type, as for a column read off a prim. A derived property never changes a type population
defines. Where the prim's schemas declare the name, or population publishes it in a fixed
type (see `USD Value Types`_), the column has that type. Anywhere else the rule's
``dtype``, ``semantic`` and ``is_array`` give it; a rule that leaves them zero takes the
type its value yields, so paths publish as a relationship. A stated type must fit the value:
a number or a bool takes no semantic, a vector at most a USD role (``POINT``, ``VECTOR``,
``NORMAL``, ``COLOR`` or ``TEXTURE_COORDINATE``), a token a ``uint64`` with ``TOKEN_ID`` and
paths a ``uint64`` array with ``RELATIONSHIP_PATH_ID``. Any other fails at submission.
The value converts to the column's type as an authored value of another type does; a value
that does not convert publishes nothing on that prim. An ``opaque`` attribute holds no
value, so the rule sees no property of that name: the column takes the rule's type or the
value's, and ``when`` finds the property absent.

A **default** in stage units: where a ``Cube`` authors no ``size``, the rule publishes
``1 / metersPerUnit``, one meter in the stage's units, instead of the schema fallback. An
authored ``size`` is published as read.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_descriptions.py
         :language: python
         :start-after: # [snippet:derive-default-in-stage-units]
         :end-before: # [/snippet:derive-default-in-stage-units]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_descriptions.cpp
         :language: cpp
         :start-after: // [snippet:derive-default-in-stage-units-c]
         :end-before: // [/snippet:derive-default-in-stage-units-c]
         :dedent:

A **resolved relationship**: the material bound to a ``Mesh`` for the ``preview``
purpose, through direct, inherited and collection-based bindings, published as
``resolvedMaterial:preview``. An empty purpose resolves the all-purpose binding. Given a
prim predicate, the expression keeps only a material that matches it. A prim with no
bound material, or whose material does not match, publishes nothing.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_descriptions.py
         :language: python
         :start-after: # [snippet:derive-bound-material]
         :end-before: # [/snippet:derive-bound-material]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_descriptions.cpp
         :language: cpp
         :start-after: // [snippet:derive-bound-material-c]
         :end-before: // [/snippet:derive-bound-material-c]
         :dedent:

A **resolved collection**: the members of a ``lights`` collection on the prim, flattened
through USD's expansion rules, keeping only the prims that are lights and no property
member. Prim members are tested by the prim predicate and property members by the property
predicate; a ``NULL`` predicate (``None`` in Python) keeps every member of its kind. An
empty membership is
published as a relationship with no targets.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_descriptions.py
         :language: python
         :start-after: # [snippet:derive-collection-members]
         :end-before: # [/snippet:derive-collection-members]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_descriptions.cpp
         :language: cpp
         :start-after: // [snippet:derive-collection-members-c]
         :end-before: // [/snippet:derive-collection-members-c]
         :dedent:

The **binding** behind a bound material: ``RESOLVED_MATERIAL_BINDING_STRENGTH`` yields its
strength, ``strongerThanDescendants`` or ``weakerThanDescendants``, and
``RESOLVED_MATERIAL_BINDING_PURPOSE`` the purpose it was authored for, or ``allPurpose``
where resolution fell back to an all-purpose binding. Both take a material purpose and an
optional prim predicate the material must match, and yield nothing where no material is
bound or it does not match. The winning binding can be an ancestor's: on an instance, the
strength says whether the material resolved at the instance overrides the bindings inside
its prototype.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_descriptions.py
         :language: python
         :start-after: # [snippet:derive-binding-strength]
         :end-before: # [/snippet:derive-binding-strength]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_descriptions.cpp
         :language: cpp
         :start-after: // [snippet:derive-binding-strength-c]
         :end-before: // [/snippet:derive-binding-strength-c]
         :dedent:

A rule over a multiple-apply schema names the family in ``multi_apply_schema_family`` and
writes ``__INSTANCE_NAME__`` in its name; it is evaluated once per instance the prim
applies, under that instance's name.

A rule for one known instance is a plain rule: its name spells the instance out, and its
prim predicate keeps it to the prims that apply that instance. ``HAS_APPLIED_SCHEMA``
matches the applied name including its instance:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population_descriptions.py
         :language: python
         :start-after: # [snippet:derive-for-one-instance]
         :end-before: # [/snippet:derive-for-one-instance]
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population_descriptions.cpp
         :language: cpp
         :start-after: // [snippet:derive-for-one-instance-c]
         :end-before: // [/snippet:derive-for-one-instance-c]
         :dedent:

Two descriptions passed to one call cannot derive the same column. Rules in different
descriptions whose names are equal, or whose ``__INSTANCE_NAME__`` template can expand to
the other's name, are rejected when the call is made. Within one description, two rules
deriving one column on one prim resolve last-one-wins, in selector and rule order, so a
rule for one instance placed after a rule for the whole family overrides it for that
instance. Malformed rules are rejected at enqueue; an evaluation that fails on a prim
publishes nothing there and is logged once per rule.

With ``RENDERING`` populated, ``purpose`` on imageable prims and ``extent`` on
``PointInstancer`` prims keep the values ``RENDERING`` computes; a rule deriving either
publishes nothing on those prims.

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

For ordinary prims populated through selectors, these names
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

An instance root carries its authored type as ``usd-prim-type`` in every population
domain, like any other prim. The :doc:`instancing` queries identify it as an instance
root. Without ``RENDERING``, its ``usd-schemas`` lists its applied APIs and its
``usd-ancestor-types`` lists the ancestors of its authored type, as for any other prim.
With ``RENDERING``, its ``usd-schemas`` also carries ``GeomInstanceAPI``, plus
``GeomInstanceRootAPI`` when it is outside a prototype, and its ``usd-ancestor-types``
contains only ``Xformable``.

When a selector populates an instanceable prim authored beneath
a ``PointInstancer``, it keeps its authored applied schemas, including alongside
``RENDERING``. At that authored path it receives neither instance tag nor the
rendering-derived tags listed below. It still links its prototype and remains
discoverable through the :doc:`instancing` queries. With ``RENDERING`` selected,
population does not synthesize ``usd-ancestor-types`` at these authored paths;
use their type or APIs rather than relying on an ancestor set. Rendering uses
relocated prototype geometry, whose metadata is a separate representation.

An instance root's authored properties are populated as for any other prim.

A ``PointInstancer``'s ``prototypes`` holds its authored targets, in authored order, in
every domain: the list ``protoIndices`` indexes. Live edits update it, including an edit
that clears the targets. Each target is a path: with ``RENDERING`` alone, the stage might have
no prim at that path.

When the ``RENDERING`` domain is selected, the following hold for every prim it populates,
including prims a selector also populates:

- A ``PointInstancer``'s ``extent`` is the bounds of its prototypes, not the authored
  ``extent``.
- ``usd-schemas`` includes ``MaterialBindingAPI`` on supported geometry and ``DomeLight``, even
  without an authored material binding. This is a recognized USD API and typed
  export applies it. Instance roots are excluded; material-binding subsets are
  included only under a non-instance ``Mesh``.
- ``usd-schemas`` also carries rendering tags that are not applied USD APIs:

  - ``GeomInstanceAPI`` on every instance root, and ``GeomInstanceRootAPI`` on an
    instance root outside a prototype.
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
  - ``SpgAPI`` on a ``Shader`` outside a ``Material`` and outside a USD prototype
    whose ``info:id`` is an SPG factory id (``spg:``, ``rtx.spg.`` or ``omni.rtx.spg.``
    followed by a name), or whose ``info:spg:sourceAsset`` is a CUDA (``cu``, ``ptx``,
    ``cubin``, ``fatbin``) or Slang (``slang``, ``slang-module``, ``spv``) asset with
    no empty sub-identifier. A source-asset shader also carries ``SpgCudaRunnableAPI``
    or ``SpgSlangRunnableAPI``. None of these is added while SPG is disabled.

These tags are preserved when ``RENDERING`` is combined with selectors. They are
not added at authored paths beneath a ``PointInstancer``.

Rendering population supplies the ancestor set when selected, including alongside
selectors, except at the authored ``PointInstancer`` descendant paths described
above. Its other current exceptions are:

* Typeless prims carry ``Imageable``.
* Instance roots and instance proxies carry only ``Xformable``.
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

USD scene-graph instance roots carry their authored types as described above.
With ``RENDERING`` population, the populated contents of an
instance can use types such as ``MeshInstance`` and ``CubeInstance``: a ``Mesh``
beneath an instance root can become ``MeshInstance``. You author ordinary USD prims
with references and ``instanceable = true``; population generates these runtime
types. You do not author ``MeshInstance`` in USD.

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
condition. This example also includes the physics selector from the setup: the
subtree selector adds ``size`` to its selection. It runs at ordinal 7, after the time-update
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

Explicit selectors also populate ancestors of selected
prims so consumers can follow the hierarchy. An ancestor that no selector matches
gets the reserved attributes above and, if USD-xformable, transform data.
It does not receive other attributes, relationships, or metadata.

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

Populated prims take part in the scene hierarchy that
:meth:`ovstage.Stage.get_hierarchy()` answers and that writes, maps and clones
connect to. A populated prim whose parent is not a prim -- an instance proxy
whose intermediate ancestor the ``RENDERING`` domain leaves out, or a prim whose
parent USD does not define -- attaches to its nearest existing ancestor, is
reported as having no parent (its children and siblings are reported as for any
other populated prim), and its derived transforms compose under that ancestor;
while it hangs there, that ancestor's ``CHILDREN`` and the ``SIBLINGS`` of its
other children list it, as the derived ``usd-children`` attribute does, and
``PARENT`` reports none until the parent exists. When a later write, map, clone
or population creates the parent, the prim is reconnected under it and its
derived transforms are recomputed on the next computation trigger, exactly as
for a cloned prim (see :doc:`cloning`).

A populated prim whose parent is a prim that a write, map or clone created
before the population step (an intermediate written before the instance is
populated) is connected under that parent as it is created, and the parent
receives ``omni:fabric:worldMatrix`` by the rule described in
:doc:`transforms` when it has a local transform and no such output yet.

The population step is never rejected for a fetched read group: while a group
covers the parent or an ancestor that would receive the attribute, the prim
keeps the parent population gave it and is connected at the next population step
or :meth:`compute_hierarchy() <ovstage.Stage.compute_hierarchy>` call after the
group is released, or at the next write, map or clone that targets that parent
and adds attributes (to it or to another of its targets) or maps its transform
input; a write or map that only changes values the parent already stores does
neither, and releasing the group by itself connects nothing. A
:meth:`compute_hierarchy() <ovstage.Stage.compute_hierarchy>` call made while
the group is still held succeeds and composes the prim where it hangs; the
first one after the release connects the prim before it computes, so its
output rows already compose through the parent.

Deleting a parent that prims
were connected under moves them to the nearest remaining ancestor, where their
derived transforms are recomputed and whose ``CHILDREN`` lists them;
re-creating the parent adopts them again.

Conversely, a prim written, mapped or cloned under a parent that does not exist
yet is reconnected when population creates that parent, and a prim written,
mapped or cloned under a populated parent reports that parent.

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

A prim that ``apply_usd_changes`` removes while the ``RENDERING`` domain is
enabled gives no ``usd-path`` tombstone in a range read. To find such prims,
compare the prims of a new query with the prims that you have. See
:doc:`/concepts/limitations`.

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
Pass it positionally or as ``ordinal=...``. The ``_with_desc`` and
``update_from_usd_time`` helpers also require an explicit ordinal.

A new stage starts sealed through ``OVSTAGE_ORDINAL_MIN``
(Python: ``ovstage.ORDINAL_MIN``). Choose an ordinal strictly above the
applicable floors and satisfy the configured admission policy; negative
ordinals and zero are valid. See :ref:`ordinal-bounds-and-initial-state`.

Use a fresh ordinal for each update, including calls to
:func:`update_from_usd_time() <ovstage.population.update_from_usd_time>` and
:func:`apply_usd_changes() <ovstage.population.apply_usd_changes>`.
The current implementation does not reject a completed write followed by another
solely because they use the same ordinal. The watermark and other write constraints
still apply; see :doc:`writing_attributes`.

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
