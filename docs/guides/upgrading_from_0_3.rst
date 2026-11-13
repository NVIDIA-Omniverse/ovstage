.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Upgrading from ovstage 0.3
==========================

This page lists what a 0.3 application must change to run on 0.4, what behaves differently without a source change, and what is new. The complete record is the ``[0.4.0]`` section of :download:`CHANGELOG.md for this documentation version <../../CHANGELOG.md>`. There are no deprecation aliases: a removed or renamed name fails immediately.

Use headers, native libraries and Python bindings from one 0.4 package, and rebuild every native consumer. Several public types keep their size but change meaning, so a 0.3 binary is not compatible with 0.4 even where structure sizes match.

An agent can apply the mechanical changes with the ``update-0-3-to-0-4`` skill (``skills/update-0-3-to-0-4/SKILL.md``).

Breaking changes
----------------

The breaking changes fall into four groups, from changes the compiler reports to changes that only alter populated data.

C/C++ names and layouts
~~~~~~~~~~~~~~~~~~~~~~~

Most of these fail to compile or link against the 0.4 headers or fail at symbol lookup. The new selector fields compile without a change, so search for selectors explicitly. Python is unaffected unless it uses the ctypes structures directly.

.. list-table::
   :header-rows: 1
   :widths: 18 18 28 36

   * - Change
     - Who is affected
     - What to do
     - Before / after
   * - Renamed C types
     - Code that names ``ovstage_vtable_t``, ``ovstage_context_t`` or ``ovx_string_or_token_t``.
     - Rename to ``ovstage_data_plane_vtable_t``, ``ovstage_data_plane_context_t`` and ``ovx_token_or_string_t``. Python ctypes users rename ``ovstage_vtable_t``, ``ovstage_context_p`` and ``ovx_string_or_token_t`` to ``ovstage_data_plane_vtable_t``, ``ovstage_data_plane_context_p`` and ``ovx_token_or_string_t``.
     - .. code-block:: c

          /* 0.3 */ ovx_string_or_token_t name;
          /* 0.4 */ ovx_token_or_string_t name;
   * - ``ovstage_instance_t`` layout
     - Code that reads ``instance->vtable`` or ``instance->context``; resolves hierarchy, usd, population, instancing or clone functions by symbol name; or, in C++, passes a literal null instance to a wrapper or takes a wrapper's address.
     - Use ``instance->data_plane`` or call the ``ovstage_*`` functions through the headers. The functions of the hierarchy, usd, population and instancing families, ``ovstage_clone`` and ``ovstage_fetch_clone_result`` became ``static inline`` wrappers with no exported symbol. 0.3 exported, and 0.4 does not export, ``ovstage_get_hierarchy``, ``ovstage_compute_hierarchy``, ``ovstage_fetch_hierarchy_result``, ``ovstage_release_hierarchy``, ``ovstage_release_hierarchy_result``, ``ovstage_get_hierarchy_computation_models``, ``ovstage_get_usd_stage_id``, ``ovstage_clone``, ``ovstage_fetch_clone_result``, ``ovstage_instancing_get_instance_roots``, ``ovstage_instancing_get_prototype_root``, ``ovstage_instancing_get_prototype_roots``, and every ``ovstage_population_*`` function except the five below. Exported in 0.4: ``ovstage_population_get_last_error``, ``ovstage_population_get_last_op_error``, ``ovstage_population_register_usd_schemas``, ``ovstage_population_export_desc_init`` and ``ovstage_population_export_desc_init_snapshot``, plus the new ``ovstage_instancing_get_instance_roots_batch`` and ``ovstage_instancing_resolve_paths``, which 0.3 did not have. C++ overloads each wrapper for the instance and for its pair type, so a call with a literal ``NULL`` or ``nullptr`` instance and an address such as ``auto f = &ovstage_wait_op`` are ambiguous: pass a typed null, give the address an explicit function-pointer type or cast, wrap the call in a lambda, or use the ``_ex`` form.
     - .. code-block:: c

          /* 0.3 */
          instance->vtable->read_attributes(instance->context, ...);
          /* 0.4 */
          instance->data_plane.vtable->read_attributes(
              instance->data_plane.context, ...);
   * - Population selectors and prim predicates grow
     - C/C++ code that builds ``ovstage_population_selector_t`` (directly or in the ``selectors`` array of ``ovstage_population_desc_t``), ``ovstage_population_prim_predicate_t`` or ``ovstage_population_export_desc_t``, and foreign-function layouts of these structs.
     - ``ovstage_population_selector_t`` gains ``derived_properties`` and ``derived_property_count``, which population validates. Code that sets each 0.3 member of an uninitialized selector still compiles but leaves them indeterminate, so the description can be rejected or an unintended pointer followed: zero-initialize every selector, or set ``derived_properties`` to NULL and ``derived_property_count`` to 0. The prim predicate's ``values`` union gains ``targeted_by``, so the struct is larger and ``value_count`` moves. ``ovstage_population_selector_t`` and ``ovstage_population_export_desc_t`` embed a prim predicate by value and move with it. Rebuild, do not hard-code these sizes or offsets, and initialize export descriptors with ``ovstage_population_export_desc_init`` or ``ovstage_population_export_desc_init_snapshot``. Python is unaffected.
     - .. code-block:: c

          /* 0.3 */
          ovstage_population_selector_t sel;
          sel.prim_predicate = ...; /* every 0.3 member */
          /* 0.4, C */
          ovstage_population_selector_t sel = {0};
          /* 0.4, C++ ({0} does not convert to the
             predicate's enum) */
          ovstage_population_selector_t sel{};
   * - Implementations and wrappers of the vtable
     - Code that defines, forwards or interposes ``ovstage_data_plane_vtable_t``, or builds an ``ovstage_instance_t`` (mocks, interposers, alternate implementations).
     - Rename the ``release_group`` slot to ``release_groups`` and adopt the new ``fetch_read_next`` slot signature (output array, capacity, count). Append the new ``clone`` and ``fetch_clone_result`` slots after ``query_extension``. Fill ``ext_instances`` with one vtable and context per family, including ``lifecycle``, as ``ovstage_ext_vtables.h`` describes. Rebuild providers and consumers together.
     - .. code-block:: c

          /* 0.3 */
          ovstage_instance_t inst = { &vtable, ctx };
          /* 0.4 */
          ovstage_instance_t inst = {
              { &data_plane_vtable, ctx },
              ext_instances };

Typed identities
~~~~~~~~~~~~~~~~

Ids, handles, tokens and paths are distinct types. Passing a plain integer or a handle of the wrong kind fails to compile in C and C++ and raises ``TypeError`` in Python. Comparing does not raise in Python: a handle compared with an integer is never equal, so ``op_id == 0`` or ``token == 0`` silently becomes ``False``. Compare with the ``ovstage.INVALID_*`` constants (``INVALID_OP_ID``, ``INVALID_QUERY_HANDLE``, …) or with typed values, or test ``bool(handle)``, which is true when the handle is not its kind's invalid value.

.. list-table::
   :header-rows: 1
   :widths: 18 18 28 36

   * - Change
     - Who is affected
     - What to do
     - Before / after
   * - Typed operation ids and handles
     - C, C++ and Python code that stores, compares or passes op ids, query, read, map, hierarchy, population or result handles as integers.
     - Keep the typed values (``ovstage_op_id_t``, ``ovstage_read_handle_t``, … ; Python ``OpId``, ``ReadHandle``, …). Use ``.raw_id`` only where an integer is required (diagnostics, numeric storage or interchange, and C comparisons), and construct the type explicitly when importing a raw id. ``Handle{}`` in C++ is the invalid value.
     - .. code-block:: c

          /* 0.3 */
          uint64_t op = result.op_index;
          /* 0.4 */
          ovstage_op_id_t op = result.op_index;
   * - Typed tokens and paths
     - Code that treats ``ovx_token_t``, ``ovx_primpath_t`` or ``ovx_primpath_list_t`` as integers, and Python code that passes ``int`` to ``PathDictionary``.
     - Keep the typed values. In C compare ``.raw_id``; C++ supports same-kind ``==``, ``<`` and ``std::hash``. Python uses ``Token``, ``PrimPath`` and ``PrimPathList``; write ``handle.raw_id`` into ``uint64`` tensors and wrap readback.
     - .. code-block:: python

          # 0.3
          tensor[i] = pd.intern_path("/World")
          # 0.4
          tensor[i] = pd.intern_path("/World").raw_id
          path = ovstage.PrimPath(int(tensor[i]))
   * - ``OVX_INVALID_TOKEN`` is ``UINT64_MAX``
     - Code that uses token ``0`` as "no token" (``token == 0``, ``!token``, Python ``not token``), or sets both ``token`` and ``string.ptr`` in ``ovx_token_or_string_t``.
     - Test against ``OVX_INVALID_TOKEN``. Token ``0`` is the empty string, and so is a zero-initialized token (``ovx_token_t{}`` in C++, ``{0}`` in C, Python ``Token()``). In Python, ``bool(token)`` is false only for the invalid token, so ``not token`` tests for it. A non-null ``string.ptr`` takes precedence over ``token``, even for an empty or stale but valid string, so the call can address a different attribute; set it to NULL for every token-only value. Remove token ``0`` and ``OVX_INVALID_TOKEN`` from query attribute lists.
     - .. code-block:: c

          /* 0.3 */
          if (token == 0) { /* no token */ }
          /* 0.4 */
          if (token.raw_id == OVX_INVALID_TOKEN.raw_id) { /* no token */ }

Ordinals and reads
~~~~~~~~~~~~~~~~~~

The ordinal domain and the shape of read results change. Some of these still compile and fail or return different results at run time.

.. list-table::
   :header-rows: 1
   :widths: 18 18 28 36

   * - Change
     - Who is affected
     - What to do
     - Before / after
   * - ``ovstage_ordinal_t`` is signed
     - Code that reads current values with ``UINT64_MAX``, stores ordinals as ``uint64_t``, or treats ordinal ``0`` as a sentinel.
     - Use ``OVSTAGE_ORDINAL_MAX`` (Python ``ovstage.ORDINAL_MAX``) for current-value reads and ``ovstage_ordinal_t`` for every ordinal. Only ``OVSTAGE_ORDINAL_MIN`` is sealed on a new stage; ``0`` and negative ordinals above it are writable.
     - .. code-block:: c

          /* 0.3 */
          range.end_ordinal = UINT64_MAX;
          /* 0.4 */
          range.end_ordinal = OVSTAGE_ORDINAL_MAX;
   * - Array fetch and ``ovstage_release_groups``
     - C code that calls ``ovstage_fetch_read_next`` or ``ovstage_release_group``.
     - Pass an output array, a positive capacity and a count to the fetch, and release with ``ovstage_release_groups``, which validates every id before releasing any group. ``OVSTAGE_OK`` delivers 1 to ``cap`` groups and only the first ``n`` slots are valid; a short batch does not mean the read is complete. Keep fetching while the status is ``OVSTAGE_OK``, as with the 0.3 one-group loop. ``OVSTAGE_ERROR_END_OF_ITERATION`` is normal completion; any other status (``OVSTAGE_ERROR_TIMEOUT``, or the read's error, which can follow an OK batch and then repeats on every fetch) is a failure.
     - .. code-block:: c

          /* 0.3 */
          while (ovstage_fetch_read_next(inst, read, t, &group)
                 == OVSTAGE_OK) {
              /* use group */
              ovstage_release_group(inst, &group);
          }
          /* 0.4 */
          while (ovstage_fetch_read_next(inst, read, t,
                                         groups, cap, &n)
                 == OVSTAGE_OK) {
              /* use groups[0..n) */
              ovstage_release_groups(inst, groups, n);
          }
          /* then check the status is
             OVSTAGE_ERROR_END_OF_ITERATION */
   * - Read groups hold columns
     - C and Python code that reads ``attribute``, ``ordinal``, ``is_delete``, ``is_array``, ``semantic``, ``meta`` or ``data`` from a read group, or in Python calls ``tensor``, ``array``, ``dlpack``, ``tensor_count``, ``data_count``, ``has_data_index_map`` or ``data_row_index`` on it.
     - Read them from ``group.columns[i]`` (Python ``ReadColumn``); the removed Python group accessors raise ``AttributeError``. Iterate columns and identify each by ``column.attribute``. Keep the containing group referenced and unreleased while using its columns or borrowed views; copy data that must outlive release. Python ``Read.fetch_next()`` still returns one group. See :ref:`ovstage-migrating-read-consumers` for layout changes and :ref:`ovstage-read-groups-columns` for row mappings, omitted columns, masks, and lifetime rules.
     - .. code-block:: python

          # 0.3 (one attribute per group)
          values = group.array(0)
          # 0.4, read of one attribute
          values = group.columns[0].array(0)
          # 0.4, read of several attributes
          points = pd.intern_token("points")
          for column in group.columns:
              if column.attribute == points:
                  values = column.array(0)

.. _ovstage-migrating-read-consumers:

Migrating read consumers from 0.3
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The read changes in the table above apply even to single-attribute reads.
Rebuild C/C++ consumers against the new headers and use matching Python
bindings and native libraries: ``ovstage_read_group_t`` now contains
``prims``, ``columns``, ``column_count``, and the release identity. Its former
``attribute``, ``ordinal``, ``is_delete``, ``is_array``, ``semantic``, ``data``,
and ``meta`` members belong to each ``ovstage_read_column_t``.

In Python, move payload accessors to ``ReadColumn`` as shown in the table.
Prim accessors (``prim_list``, ``prim_count``, and ``prim_index()``) remain on
``ReadGroup``. ``MapGroup`` keeps its writable single-attribute interface.
Then check each read loop against :ref:`ovstage-read-groups-columns`, especially
the independent prim/value mappings, omitted columns, masks, and borrowed-view
lifetimes. The :doc:`reading guide </scene/reading_attributes>` includes tested
multi-column examples and the C batching contract.

Populated data
~~~~~~~~~~~~~~

These compile and run unchanged but populate different data. Search for them explicitly. Refer to :doc:`/scene/population` and :doc:`/scene/instancing`.

.. list-table::
   :header-rows: 1
   :widths: 18 18 28 36

   * - Change
     - Who is affected
     - What to do
     - Before / after
   * - ``PHYSICS`` and ``ALL`` domains removed
     - Code that populates with ``OVSTAGE_POPULATION_DOMAIN_PHYSICS`` or ``_ALL`` (Python ``PopulationDomain.PHYSICS`` or ``.ALL``).
     - Pass the physics population description your physics library provides through a ``_with_desc`` entry point. In place of ``ALL``, also pass ``RENDERING`` in the ``domains`` of a description in the same call. The named constants no longer compile, but a raw former ``PHYSICS`` bit is ignored and populates nothing.
     - .. code-block:: text

          # 0.3: domains = PHYSICS | RENDERING
          # 0.4: descs = [physics_desc,
          #               desc with domains = RENDERING]
   * - Instance roots carry their authored type
     - Code that finds instance roots by ``XformInstance`` / ``ScopeInstance``, by ``GeomInstanceAPI`` / ``GeomInstanceRootAPI`` without ``RENDERING``, or by ``usd-ancestor-types`` being ``Xformable`` without ``RENDERING``, or reads ``usd-metadata:typeName`` on them.
     - Find instance roots with the instancing queries. Read their type from ``usd-prim-type``, in every domain; two instance roots sharing a prototype keep their own types. Without ``RENDERING``, ``usd-schemas`` no longer carries ``GeomInstanceAPI`` or ``GeomInstanceRootAPI`` and ``usd-ancestor-types`` lists the authored type's ancestors. ``usd-metadata:typeName`` is published only where a description selects it. Typed export also uses ``usd-prim-type`` for instance roots.
     - .. code-block:: text

          # 0.3: usd-prim-type = XformInstance
          # 0.4: usd-prim-type = <authored type>
   * - ``PointInstancer.prototypes`` holds authored targets
     - Code that reads ``prototypes:usd-targets``, or reads ``prototypes`` as the geometry ``RENDERING`` renders.
     - Read ``prototypes``: authored targets in authored order, indexed by ``protoIndices``, in every domain.
     - .. code-block:: text

          # 0.3: prototypes:usd-targets = authored targets
          # 0.4: prototypes             = authored targets
   * - Render camera targets
     - Code that reads ``RenderProduct.camera`` or ``RenderSettings.camera`` under ``RENDERING`` and expects only camera prims.
     - The targets are kept in authored order whatever their type, including targets that do not resolve; property paths stay excluded. Check each target's type and skip the ones you do not support.
     - —
   * - ``extent`` of particle fields
     - Code that reads ``extent`` on ``ParticleField`` or ``ParticleField3DGaussianSplat`` prims.
     - ``extent`` is one ``{kDLFloat, 64, 6}`` value, as on other ``UsdGeomImageable`` prims, also where the particle field schema is not registered. In 0.3 an authored ``extent`` could keep its ``float3[]`` type, and with ``RENDERING`` and selectors together the open or live edit failed with ``OVSTAGE_ERROR_INVALID_ARGUMENT``.
     - .. code-block:: text

          # 0.3: float3[] as authored
          # 0.4: {kDLFloat, 64, 6}

Behavior changes
----------------

These need no source change but can alter results.

* **Attribute names containing an asterisk.** Writes, maps, deletes, query filter predicates and attribute lists, ``advance_write_floor`` with ``SCOPE_INCLUDE`` or ``SCOPE_EXCLUDE``, and ``get_attribute_write_floor`` reject a name that contains ``*`` with ``OVSTAGE_ERROR_INVALID_ARGUMENT``. In reads, a single trailing ``*`` (``*`` or ``prefix*``) is a selector pattern; any other ``*`` fails with ``OVSTAGE_ERROR_INVALID_ARGUMENT``.
* **Range reads of usd-path.** A range read of ``usd-path`` returns one tombstone group for the query prims deleted whole in the range, and fails with ``OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION`` unless the ``usd-path`` write floor is at or above the range end. Advance it with ``SCOPE_ALL``, or ``SCOPE_INCLUDE`` listing ``usd-path``, before the read.
* **Retention frontier.** Prim creations and whole-prim deletes keep history for ``usd-path`` range reads, which can raise ``ovstage_get_oldest_preserved_ordinal`` for every caller. Query the frontier instead of caching it.
* **Changed-property export.** ``since_ordinal`` accepts zero and negative values; it must still precede the export ordinal.
* **Live USD edits.** Adding, removing or changing a property republishes that property and the values population computes from its prim (derived properties, ``RENDERING`` schema tags and, after a transform op edit, the transform), rewriting them on every property edit even when they do not change, so they appear in change ranges. The whole prim is republished only when a description's prim predicate reads the property, the property is a relationship and a description brings relationship targets into scope, or it is a new collection property. The prims below it are republished only when a predicate reads the property on an ancestor (``HAS_ANCESTOR``, ``HAS_PARENT``, ``HAS_PURPOSE``) or a derived property resolves a material binding from it. In 0.3, adding or removing a property republished the prim and every prim below it. Instances, prototype content and prims below a ``PointInstancer`` still republish with their subtree.
* **Scene hierarchy.** Clones, populated prims and deletes now reconnect prims in the scene hierarchy, which can change transforms and hierarchy answers and can make structural operations fail under a retained read. See :ref:`upgrading-0-3-scene-hierarchy` below.
* **Maps of the type, schema and ancestor-type tags.** ``ovstage_map_attribute`` (Python ``Stage.map_attribute``) on these names now takes effect: the committed token ids become the prim's type, applied-schema and ancestor-type tags, as after a copy-in write, and a map that creates a prim gives it that type. In 0.3 such a map could land in a value column no reader consulted, so it did not change the tags; this is a correction, not the removal of a working GPU path. The map must be host-resident and canonical: ``dtype`` omitted or ``{kDLUInt, 64, 1}``, semantic ``NONE``, ``element_sizes`` given for ``usd-schemas`` and ``usd-ancestor-types`` and omitted for ``usd-prim-type``, and a query that names each prim once. A CUDA placement returns ``OVSTAGE_ERROR_NOT_SUPPORTED``, even for an empty query that 0.3 completed as a no-op. Like a copy-in write of these names, a commit is refused while any attribute of a mapped prim has an outstanding read, so release those read groups before committing; a failed accepted commit ends the map session (see the map-commit recovery item under :ref:`upgrading-0-3-scene-hierarchy`). A staged row whose ids name nothing fails the commit before any tag changes.
* **Process environment.** ovstage sets ``HD_ENABLE_SCENE_INDEX_EMULATION=0`` before loading OpenUSD, and release loaders set ``TF_ENV_SETTING_ALERTS_ENABLED=0``. Both override inherited values and are inherited by child processes.
* **Resolver context.** ``ovstage_create_instance`` captures the ``ArResolverContext`` bound on the calling thread and uses it for every population layer open. Bind or configure a context-dependent resolver before creating the instance. Instance creation can be the process's first ``ArGetResolver()`` call, which fixes OpenUSD's primary resolver, so select one with ``ArSetPreferredResolver`` before creating the first instance. Destroy the instance before releasing the resolver runtime its captured context refers to.
* **Bundled Omniverse Client Library 2.76.0.** ``omniverse_connection`` is no longer shipped in ``bin/plugins/omni.client.lib``.

.. _upgrading-0-3-scene-hierarchy:

Scene hierarchy
~~~~~~~~~~~~~~~

Writes, maps, clones, deletes and USD population now keep one scene hierarchy. Prims are reconnected under their real parent when it appears, which changes derived transforms and hierarchy answers that 0.3 left stale. Refer to :doc:`/scene/cloning` and :doc:`/scene/transforms`.

* **Late parents adopt their children.** A clone whose destination parent does not exist attaches to the nearest existing ancestor, as before. When a later write, map, clone or population creates the parent, the clone root moves under it, and its derived transforms are recomputed on the next computation trigger. Populated prims whose parent is not a prim, such as a ``RENDERING`` instance proxy under a prototype intermediate, are reconnected the same way. Create destination parents before cloning where you can: the copies then land in their final place at once and nothing is reconnected later.
* **Deletes reattach surviving children.** Deleting a prim that has connected children moves them under the nearest remaining ancestor; creating the prim again adopts them. A clone whose target root still has such children reattaches them and counts them in ``prims_cloned``.
* **Parents gain the world matrix attribute.** A written or mapped parent of prims that carry ``omni:fabric:worldMatrix`` receives the attribute once it has both those children and a local transform, and so do its written ancestors up to the first that already has it. The ancestor a clone root attaches to while its destination parent is absent receives it the same way when the clone is published. Its value is the identity until the next computation.
* **Retained reads can block structural operations.** Receiving that attribute relocates the prim's storage. A write, map or clone that would give it to a prim that a fetched, unreleased read group covers fails with ``OVSTAGE_ERROR_OP_FAILED`` and the rejected operation creates nothing. A delete that would give it to the remaining ancestor fails the same way and deletes nothing. The prim need not be a target: it can be the destination parent or one of its ancestors, a prim with a transform input between a target and a descendant that carries the attribute, or an existing descendant a clone merges into.
* **Recovering from that failure.** ``OVSTAGE_ERROR_OP_FAILED`` also has other causes, so read the diagnostic first (see :doc:`/concepts/error_handling`). If a held read group is the cause, release it before trying again; releasing it does not undo or complete the failed operation.

  * *Writes.* Entries that completed before the failure stay applied, and the API does not report which ones did. Treat a failed batch as possibly partial and do not infer completion from query visibility. Prefer repairing it with a newer ordinal. A corrected retry at the same ordinal is safe when no entry completed; under ``STRICTLY_INCREASING``, replaying completed cells at their recorded ordinal is rejected, while the other admission policies may admit it. Finish the repair before advancing the watermark, as :ref:`ovstage-write-recovery` describes.
  * *Map commits.* A map commit is not transactional: a failed commit is not undone, so some of its changes can remain. After an accepted group commit fails, the map session is terminal and its groups cannot be committed again, even once the read group is released. Call ``unmap_attribute`` to end the session, then repair with a new map session or a write.
* **Population defers instead of failing.** A population step in the same situation succeeds: the affected prims keep the parent population gave them, a warning is logged once, and they are connected at the next population step or ``ovstage_compute_hierarchy()`` call after the groups are released, or at the next write, map or clone that targets that parent and adds attributes or maps its transform input. Releasing the groups alone connects nothing.
* **Hierarchy answers.** Cloned prims now appear under their parents in ``ovstage_get_hierarchy()`` ``PARENT``, ``CHILDREN`` and ``SIBLINGS`` answers; in 0.3 they reported no parent and no children. A prim written, mapped or cloned under a parent that exists only through USD population reports that parent from ``ovstage_get_hierarchy()`` ``PARENT``. A populated prim whose parent is not a prim reports no ``PARENT`` until the parent exists, where 0.3 reported the ancestor it hung under; that ancestor's ``CHILDREN`` and its other children's ``SIBLINGS`` list it meanwhile. ``usd-parent`` and ``usd-children`` are unchanged.

New in 0.4
----------

* ``OVSTAGE_ORDINAL_MIN`` and ``OVSTAGE_ORDINAL_MAX`` (Python ``ORDINAL_MIN`` and ``ORDINAL_MAX``).
* ``OVSTAGE_FILTER_OP_NOT_HAS`` (Python ``FilterOp.NOT_HAS``): leave out prims that have an attribute. Refer to :doc:`/scene/queries`.
* Attribute selector patterns ``*`` and ``prefix*`` in ``read_attributes``. Refer to :doc:`/scene/reading_attributes`.
* CUDA placed reads of numeric array-valued attributes, including ragged and empty arrays.
* Population predicates ``TARGETS``, ``HAS_DESCENDANT``, ``IS_TARGETED_BY`` and ``IS_INSTANCE``, and derived properties with expressions, including resolved material bindings and collection members. Refer to :doc:`/scene/population`.
* ``ovstage_instancing_resolve_paths`` (Python ``ovstage.instancing.resolve_paths``) and ``ovstage_instancing_get_instance_roots_batch`` (Python ``ovstage.instancing.get_instance_roots_batch``). Refer to :doc:`/scene/instancing`.
* ``OVSTAGE_CONFIG_POPULATION_THREAD_LIMIT`` (Python ``StageConfig.population_thread_limit``) and ``OVSTAGE_CONFIG_POPULATION_STRICT_RESOLUTION`` (Python ``StageConfig.population_strict_resolution``), with ``OVSTAGE_POPULATION_THREAD_LIMIT_AUTO`` (Python ``ovstage.POPULATION_THREAD_LIMIT_AUTO``).

Checklist
---------

Search the application for these patterns before building against 0.4. The rows follow the groups above.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Pattern
     - Action
   * - ``ovstage_vtable_t``, ``ovstage_context_t``, ``ovx_string_or_token_t``; Python ctypes ``ovstage_context_p``
     - Rename.
   * - ``instance->vtable``, ``instance->context``; ``dlsym`` / ``GetProcAddress`` of ``ovstage_get_hierarchy``, ``ovstage_compute_hierarchy``, ``ovstage_fetch_hierarchy_result``, ``ovstage_release_hierarchy``, ``ovstage_release_hierarchy_result``, ``ovstage_get_hierarchy_computation_models``, ``ovstage_get_usd_stage_id``, ``ovstage_population_*``, ``ovstage_instancing_*``, ``ovstage_clone``, ``ovstage_fetch_clone_result``; C++ wrapper calls with ``NULL`` / ``nullptr``; ``&ovstage_`` addresses in C++
     - Use ``instance->data_plane`` or the inline wrappers; keep lookups only for the functions exported in 0.4; use a typed null, an explicit function-pointer type, a lambda or the ``_ex`` form.
   * - definitions or wrappers of ``ovstage_data_plane_vtable_t``; ``release_group`` and ``query_extension`` slots; ``ovstage_instance_t`` initializers
     - Rename ``release_group`` to ``release_groups``, adopt the new ``fetch_read_next`` signature, append ``clone`` and ``fetch_clone_result``, fill ``ext_instances``.
   * - ``ovstage_population_selector_t`` declared without an initializer, including entries of ``ovstage_population_desc_t.selectors``
     - Zero-initialize it (``{0}`` in C, ``{}`` in C++), or set ``derived_properties`` to NULL and ``derived_property_count`` to 0.
   * - op ids, handles, tokens or paths held as integers, compared with integers (``== 0``, ``!= 0``), tested for truth (``if handle``, ``not handle``), or converted implicitly
     - Keep typed values; compare with ``ovstage.INVALID_*`` or typed values; use ``.raw_id`` explicitly.
   * - ``OVX_INVALID_TOKEN``, ``token == 0``, ``!token``, Python ``not token``, ``ovx_token_or_string_t``
     - Test ``OVX_INVALID_TOKEN`` (Python: ``not token``); in every token-only ``ovx_token_or_string_t``, set ``string.ptr`` to NULL, even where it pointed at a valid string.
   * - ``UINT64_MAX``, ``0xFFFFFFFFFFFFFFFF``, ``2**64``, ``uint64_t`` ordinals, ordinal ``0`` as a sentinel
     - Use ``OVSTAGE_ORDINAL_MAX`` and ``ovstage_ordinal_t``.
   * - ``ovstage_fetch_read_next``, ``ovstage_release_group``; on a read group ``attribute``, ``ordinal``, ``is_delete``, ``is_array``, ``semantic``, ``meta``, ``data``, ``tensor``, ``array``, ``dlpack``, ``tensor_count``, ``data_count``, ``has_data_index_map``, ``data_row_index``
     - Fetch into an array while the status is ``OVSTAGE_OK``, using only the ``n`` delivered groups, and treat any end status other than ``OVSTAGE_ERROR_END_OF_ITERATION`` as a failure; find columns by ``attribute``; release with ``ovstage_release_groups``; keep Python groups referenced and unreleased while their columns or views are used.
   * - ``DOMAIN_PHYSICS``, ``DOMAIN_ALL``, ``PopulationDomain.PHYSICS``, ``PopulationDomain.ALL``
     - Pass the physics description; use ``RENDERING`` beside it.
   * - ``XformInstance``, ``ScopeInstance``, ``GeomInstanceAPI``, ``GeomInstanceRootAPI``, ``usd-ancestor-types`` on instance roots, ``usd-metadata:typeName``
     - Find instance roots with the instancing queries.
   * - ``prototypes:usd-targets``
     - Read ``prototypes``.
   * - ``RenderProduct.camera`` / ``RenderSettings.camera`` targets; ``extent`` on particle fields
     - Filter camera targets by type; read ``extent`` as one ``{kDLFloat, 64, 6}`` value.
   * - attribute names containing ``*``; read selectors with ``*`` other than one trailing
     - Rename; use ``*`` or ``prefix*`` only.
   * - ``usd-path`` range reads
     - Advance the ``usd-path`` floor first; expect tombstones.
   * - read groups held across writes, maps, clones or deletes; assertions on ``PARENT``, ``CHILDREN``, ``SIBLINGS``, ``prims_cloned`` or world transforms of clones and populated prims
     - Release groups first; create destination parents before cloning; on ``OVSTAGE_ERROR_OP_FAILED`` read the diagnostic, repair a write (preferably with a newer ordinal), and replace a failed map session; re-check the assertions.
   * - ``map_attribute`` on ``usd-prim-type``, ``usd-schemas`` or ``usd-ancestor-types``
     - Use a canonical host-resident map without CUDA placement; release read groups on the mapped prims before committing.
