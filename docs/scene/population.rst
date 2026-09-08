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

The population API (``ovstage_population.h``) is the bridge that ingests composed
USD content into the runtime stage. It is a self-contained C API with its own
asynchronous model — ``ovstage_population_enqueue_result_t`` and
``ovstage_population_wait_op`` — that runs parallel to the data-plane
:doc:`submit/observe model </concepts/async_model>`.

.. note::

   These entry points are **pre-release and provisional** ("Draft — API in
   flux").

Loading a Scene
---------------

``ovstage_population_open_usd_from_file`` (and ``_from_string``) loads a USD
scene into the runtime table in one op at a chosen ordinal. As with any write,
seal the ordinal with ``advance_write_floor`` before reading the populated prims
back:

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

Loads take a population **domain** bitmask (``PopulationDomain`` /
``ovstage_population_domain_t``: ``NONE``, ``RENDERING``, ``PHYSICS``, ``ALL``)
selecting which consumer's attribute set to populate.

Descriptions and Selectors
--------------------------

A **description** (``ovstage_population_desc_t`` / ``Desc``) says what to populate. It
carries a ``domains`` bitmask and a list of **selectors**; the ``_with_desc`` entry
points take an array of them, whose ``domains`` are OR-ed together and whose selectors
are combined in array order.

A **selector** pairs a **prim predicate** — which prims it puts in scope — with a
**property predicate** — which of their properties are published — and may also name
prim and property metadata paths. A prim in scope of more than one selector publishes
what any of them selects.

Predicates are trees. Each has a *kind* and, depending on the kind, either a list of
string values or a list of subpredicates. A kind that takes values matches when any one
of them matches — the values of a single predicate are a disjunction, never a
conjunction — so a ``HAS_TYPE`` over ``Cube`` and ``Sphere`` matches a prim that is
either. Requiring more than one condition, or a negated one, is what ``AND`` / ``OR`` /
``NOT`` are for.

A prim predicate reaches a property predicate through ``HAS_PROPERTY``, so a prim can
be gated on the properties it carries.

A predicate kind the build cannot honour, a graph that nests too deeply, one that refers
back to itself, and one that walks past the node limit are all rejected at enqueue with a
diagnostic — never populated as a silent no-op. Nested predicates are referenced, not
copied, so every node must outlive the call.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_population.py
         :language: python
         :start-after: # [snippet:populate-narrowed]
         :end-before: # [/snippet:populate-narrowed]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_population.cpp
         :language: cpp
         :start-after: // [snippet:populate-narrowed-c]
         :end-before: // [/snippet:populate-narrowed-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

See the ``ovstage_population_prim_predicate_kind_t`` and
``ovstage_population_property_predicate_kind_t`` enumerators in
``ovstage_population_predicate.h`` for what each kind matches.

USD Value Types
---------------

What a value becomes in ovstage is fixed by its USD value type. With a description
selecting arbitrary properties this is the contract you size and interpret a read
against, so the whole mapping is below.

Three rules cover almost all of it:

#. **Numerics keep their bytes.** The dtype's element type and width come from the USD
   scalar type, its lane count from the number of components.
#. **A USD array** populates with its element's dtype and ``is_array = true``. Everything
   else populates flat, one fixed-size value per prim.
#. **String-like types do not populate as text.** They are interned through the path
   dictionary and populate as ``uint64`` ids, resolved back through the dictionary.

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

A **role-carrying** type populates exactly as the type it is built on and differs only in
its semantic: ``point3f`` is ``{float, 32, 3}`` with ``POINT``, and ``normal3f``,
``vector3f``, ``color3f`` and ``texCoord2f`` / ``texCoord3f`` likewise carry ``NORMAL``,
``VECTOR``, ``COLOR`` and ``TEXTURE_COORDINATE``. ``frame4d`` is a ``matrix4d`` carrying
``FRAME`` rather than ``MATRIX`` — the role wins where a type has both.

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

An ``asset`` is the one that is two ids wide: it carries the ``(authored, resolved)``
pair. ovstage does not resolve assets — the resolved half is whatever USD had resolved
when the value was read.

A ``string`` populates with semantic ``NONE``, so semantic alone does not distinguish it
from a plain ``uint64``. Read the USD type if you need to tell them apart.

Arrays
~~~~~~

A USD array populates with ``is_array = true`` and its element's dtype and semantic:
``float3[]`` is ``{float, 32, 3}``, and ``matrix4d[]`` is ``{float, 64, 16}`` with
``MATRIX``.

String-like arrays carry one id per element, so the element count comes back directly
rather than having to be recovered from the payload: ``token[]``, ``string[]`` and
``pathExpression[]`` are ``{uint, 64, 1}``, and ``asset[]`` is ``{uint, 64, 2}`` with one
pair per element.

A relationship's targets populate as interned path ids, ``{uint, 64, 1}`` with
``RELATIONSHIP_PATH_ID`` and ``is_array = true``. A relationship authored with no targets
populates as an empty value rather than being absent, so presence is what tells you it
was authored.

Empty and Valueless
~~~~~~~~~~~~~~~~~~~

An empty ``token``, ``string``, ``pathExpression``, or ``asset`` half populates as **id
0**. These are values USD authors and reads back, so they are populated rather than
dropped, and absence means the property was not authored at all — not that it was
authored empty. In an array an empty element is id 0 and keeps its place.

``opaque`` carries no value by definition. Nothing is populated for it and nothing is
reported: there is no payload to store and nothing a consumer could read.

Properties That Do Not Follow Their USD Type
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A few properties populate in a type their USD type does not predict. The ``RENDERING``
domain publishes each of these in a fixed type and both domains have to agree on a shared
stage, so population matches it — in every domain, so that what you read never depends on
which domains you asked for.

.. list-table::
   :header-rows: 1
   :widths: 34 20 24 22

   * - Property (where it applies)
     - USD type
     - populated as
     - instead of
   * - ``extent`` (any ``UsdGeomImageable``)
     - ``float3[]``, two points
     - ``{float, 64, 6}``, ``is_array = false``, ``NONE``
     - ``{float, 32, 3}`` array
   * - ``normals`` (``UsdGeomCurves``, ``UsdGeomPoints``)
     - ``normal3f[]``
     - ``POINT``
     - ``NORMAL``
   * - ``inputs:color`` (``UsdLuxLightAPI``), ``inputs:shadow:color`` (``UsdLuxShadowAPI``)
     - ``color3f``
     - ``VECTOR``
     - ``COLOR``
   * - ``omni:rtx:domeLight:size`` (``OmniFiniteDomeLightAPI``); ``nerf:crop:minBounds``, ``nerf:crop:maxBounds``, ``nerf:offset`` (``OmniNerfVolumeAPI``); ``omni:nurec:crop:minBounds``, ``omni:nurec:crop:maxBounds``, ``omni:nurec:offset`` (``OmniNuRecVolumeAPI``)
     - ``float3``
     - ``VECTOR``
     - ``NONE``

``extent`` is the only one whose dtype differs; the rest differ only in semantic and their
bytes are unchanged. Each is gated on the schema shown, so a property that merely shares
one of these names elsewhere — on your own schema, say — is untouched and follows its USD
type.

Stage Metadata
--------------

Stage metadata is populated onto the root prim, ``/``, when something asks for it: a
selected domain may bring stage metadata of its own, and a description names whatever
it wants through ``stage_metadata_paths``. Each field lands under the
``usd-metadata:<path>`` prefix. With nothing asking, none is populated.

A path is a stage metadata field name, extended with a ``:``-joined key path to reach
inside a dictionary field.

- ``timeCodesPerSecond`` — populates that field.
- ``customLayerData:myTool:version`` — populates one value from inside a dictionary.
- ``customLayerData`` — names a dictionary rather than a single value, so every value
  beneath it is populated, one attribute each, under its flattened path.

A path that does not resolve populates nothing. A path with an empty component — a
leading, trailing or doubled ``:`` — names neither a field nor a key, and is rejected
wherever one is taken.

Prim and Property Metadata
--------------------------

A selector also names ``prim_metadata_paths`` and ``property_metadata_paths``: USD
metadata to populate for each prim it puts in scope, and for each property it publishes.
Paths take the same form as the stage metadata paths above, and land under the same
``usd-metadata:`` prefix:

- Prim metadata — ``usd-metadata:<path>``
- Property metadata — ``<property>:usd-metadata:<path>``

``HAS_METADATA`` selects on those same paths, and there is one of each kind: the prim kind
puts a prim in scope when the prim resolves one of the paths named, and the property kind
publishes a property when the property resolves one. Presence, not value — a path resolving
to ``false`` matches.

Name a key, not a whole dictionary field. A field matches whenever anything resolves
beneath it, including what a schema registers rather than the prim authoring it;
``customData:myTool:export`` selects the prims carrying that key.

Reserved Prim Attributes
------------------------

Every populated prim carries ``usd-prim-type``, whatever put it in scope and whatever
its properties are. A prim USD gives no type name gets the reserved untyped name
instead (``ovstage_population_untyped_type_name()``), so it is never absent.

``usd-schemas`` carries the prim's applied API schema names, and is populated only when
it has any — a prim with none does not carry it at all, rather than carrying an empty
one.

Neither depends on the selector's property predicate. A selector that publishes no
properties at all still yields these.

Path Predicates
---------------

``HAS_PATH`` and ``IS_UNDER_PATH`` take an absolute prim path, or ``/``. A property,
target, or variant-selection path is rejected at enqueue.

Neither matches a prim inside a native instance.

Populated Ancestors
-------------------

A prim that ``PHYSICS`` domain or a desc's selectors put in scope is published
together with every one of its ancestor prims, so a consumer can walk the prim tree
down to it however sparse the selection is.

An ancestor that no selector matches on its own merit carries none of the data a
selected prim does — no attributes, relationships, or metadata — only the reserved
ones above. It does carry the derived transform attributes below when it is
xformable, which is what lets world-transform composition run the whole chain.

Derived Transform Attributes
----------------------------

For every prim that ``PHYSICS`` domain or a desc's selectors put in scope, and that is
USD-xformable, population also publishes the prim's local transform state:

- ``omni:xform`` — the prim's USD transform ops composed into a 4x4 row-major
  matrix (``float64``, 16 lanes, ``MATRIX`` semantic). This is the input
  ``ovstage_compute_hierarchy`` reads to derive world transforms.
- ``omni:resetXformStack`` — ``bool``, true when the prim resets the inherited
  parent transform stack.
- ``omni:fabric:worldMatrix`` — created, not maintained, and only when ``RENDERING``
  is not among the selected domains. ``ovstage_compute_hierarchy`` updates this value but
  does not create it, so population creates it for a qualifying prim that has none and
  then leaves it alone: the value is written once, at creation, as the local transform,
  and later derived values come from hierarchy computation. Treat this provisionally
  named attribute as read-only output as described in :doc:`transforms`. A prim that
  stops being xformable loses it. With ``RENDERING`` selected that domain owns it outright
  and population neither creates nor writes it.

A prim that is not xformable gets none of these. Being xformable does not by itself
put a prim in scope: the derivation only ever applies to prims population already
publishes. See :doc:`transforms` for transform authoring and
``ovstage_population.h`` for the full population contract.

Editing the USD Source
----------------------

After a scene is live you can edit the USD source and propagate the change into
the runtime table at a fresh ordinal:

- ``ovstage_population_add_usd_reference_from_file`` / ``_from_string`` — add a
  reference onto an existing prim.
- ``ovstage_population_remove_usd_reference`` / ``ovstage_population_reset_usd`` —
  remove a reference or reset.
- ``ovstage_population_apply_usd_changes`` — propagate pending USD edits into the
  table (at a new ordinal).

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

``ovstage_population_apply_usd_time`` re-samples time-sampled attributes at a new
time and writes them at a fresh ordinal. Call it once per tick when playing back
time-sampled USD content.

Values are re-sampled from the current USD state, so edits to an attribute's time
samples are picked up even without an intervening
``ovstage_population_apply_usd_changes``. What is re-sampled is whatever the
opening ``open_usd_*`` put in scope — for a description that is its selectors as
well as its domains, so one carrying selectors and ``domains == 0`` is advanced
like any other.

Propagating structural edits — prims added or removed, and attribute or
relationship changes that are not time samples — is
``ovstage_population_apply_usd_changes``'s job. ``RENDERING`` is the exception: for
that domain this call propagates them as well. Other domains do not, and the
difference is expected to go away.

Python ↔ C Name Mapping
------------------------

Several Python wrappers use shorter names than their C entry points:

.. list-table::
   :header-rows: 1

   * - Python (``ovstage.population``)
     - C entry point
   * - ``open_usd`` / ``open_usd_async``
     - ``ovstage_population_open_usd_from_file``
   * - ``add_usd_reference``
     - ``ovstage_population_add_usd_reference_from_file``
   * - ``remove_usd``
     - ``ovstage_population_remove_usd_reference``
   * - ``update_from_usd_time``
     - ``ovstage_population_apply_usd_time``
   * - ``open_usd_with_desc``
     - ``ovstage_population_open_usd_from_file_with_desc``

The ``time_code`` parameter on ``open_usd*`` / ``update_from_usd_time`` is in
**seconds** — it maps to the C ``time`` parameter, converted internally via the
stage's ``timeCodesPerSecond``. ``math.nan`` (the ``open_usd*`` default)
evaluates at USD's Default time code.

Ordinal Ownership
-----------------

Population never opens or seals an ordinal for you — the application owns the
ordinal lifecycle. Pass the current ordinal to each call and seal each tick with
``advance_write_floor``. Keep ordinals monotonic (populate at 1, then writes and
USD edits at higher ordinals); an operation at or below the floor is a write-floor
violation.

A newly created instance already has a write floor of 0, so the first population
must use an ordinal of at least 1 — 0 is never a usable *write* ordinal, even
though it is a perfectly valid ordinal to read at. An application whose frame
counter starts at 0 should offset it rather than pass 0 through.

Where to Go Next
----------------

- :doc:`/guides/runtime_loop` — the full populate → read → update → read loop.
- :doc:`exporting_to_usd` — author selected runtime state back into a USD layer.
- :doc:`reading_attributes` — reading ``usd-prim-type`` and other populated metadata back.
- :doc:`instancing` — discovering instance and prototype roots in populated content.
