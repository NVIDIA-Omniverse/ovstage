.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Upgrading from ovstage 0.2
==========================

This page lists what a 0.2 application must change to run on 0.3, what behaves differently without a source change, and what is new. The complete record is the ``[0.3]`` section of :download:`CHANGELOG.md for this documentation version <../../CHANGELOG.md>`. There are no deprecation aliases: a removed or renamed name fails immediately.

An agent can apply the mechanical changes with the ``update-0-2-to-0-3`` skill (``skills/update-0-2-to-0-3/SKILL.md``).

Breaking changes
----------------

.. list-table::
   :header-rows: 1
   :widths: 18 18 28 36

   * - Change
     - Who is affected
     - What to do
     - Before / after
   * - ``PathDictionary`` requires a ``Stage``
     - Python code that calls ``PathDictionary()`` or ``PathDictionary(None)``.
     - Create or reuse a ``Stage`` and pass it. The removed forms raise ``TypeError``; no backing ``Stage`` is created for you, so manage its lifetime.
     - .. code-block:: python

          # 0.2
          pd = ovstage.PathDictionary()
          # 0.3
          pd = ovstage.PathDictionary(stage)
   * - Each instance owns its path dictionary
     - C and Python code that builds a path list in one instance and uses it with another, or that outlives the instance.
     - Recreate the selection in the destination instance's dictionary. Release list references before destroying the owning instance. The ``ovstage_get_path_dictionary(instance)`` signature is unchanged.
     - .. code-block:: c

          /* 0.3: list belongs to instance_a only */
          ovstage_get_path_dictionary(instance_a);
          /* build a new list from instance_b's dictionary for instance_b */
          ovstage_get_path_dictionary(instance_b);
   * - ``time_code`` renamed ``time``
     - Python callers that pass ``time_code=`` by keyword to ``open_usd``, ``open_usd_from_string``, ``open_usd_with_desc``, ``open_usd_from_string_with_desc``, ``update_from_usd_time``, or their ``_async`` forms.
     - Rename the keyword. Positional calls are unaffected. The old keyword is not accepted as an alias and raises ``TypeError``.
     - .. code-block:: python

          # 0.2
          population.open_usd(stage, path, time_code=1.0)
          # 0.3
          population.open_usd(stage, path, time=1.0)
   * - Connection writes no longer broadcast
     - Code that writes a connection attribute to several prims from one ``(target path id, target attribute token id)`` pair.
     - Supply exactly one pair per target prim in a shared input tensor, or exactly one pair in each tensor of the per-prim form. Mismatched counts are rejected rather than broadcast. Submission validation reports ``INVALID_ARGUMENT``; Python ``wait()`` reports failures discovered during execution as ``OP_FAILED``. Always wait after a successful enqueue.
     - .. code-block:: text

          # 0.2: 1 pair, N prims  (broadcast)
          # 0.3: N pairs, N prims (one pair per prim)
   * - Untyped prim type removed
     - Code that calls ``ovstage_population_untyped_type_name()`` or tests for ``__ovstage_population_untyped__``.
     - Remove the call. A USD prim with no type is still populated and carries no ``usd-prim-type``. Test for the missing attribute instead (for example, a prim that does not ``HAS`` ``usd-prim-type``). Export still writes a typeless ``def``.
     - .. code-block:: c

          /* 0.2 */
          const char* t = ovstage_population_untyped_type_name();
          /* 0.3: no symbol; absence of usd-prim-type means untyped */
   * - ``ovalign.h`` removed
     - Code that includes ``<ovstage/ovalign.h>`` or names ``ovalign_align`` or ``ovalign_release``.
     - Remove the include and calls. No release exported these symbols, so such code could not link; the exported C ABI is unchanged.
     - .. code-block:: c

          /* 0.2 */
          #include <ovstage/ovalign.h>
          /* 0.3: delete the include */
   * - Selector-only ``UsdRenderVar.sourceName`` layout
     - Code that reads ``sourceName`` from a stage populated with generic selectors only (no ``RENDERING`` domain), as an interned id.
     - Read it as raw UTF-8 bytes (``uint8``, array) with ``OVSTAGE_SEMANTIC_STRING``, the representation ``RENDERING`` already used. Refer to :doc:`/scene/population`. This row is taken from the changelog.
     - .. code-block:: text

          # 0.2: uint64 interned id
          # 0.3: {uint, 8, 1} array + OVSTAGE_SEMANTIC_STRING

Behavior changes
----------------

These need no source change but can alter results.

* **Open merges instead of clearing.** Opening USD on an instance that already holds populated content writes over it and removes nothing, so opening different content yields a mixture. To replace, call ``ovstage_population_reset_usd`` (Python ``population.reset_usd``) then ``ovstage_population_apply_usd_changes`` at an ordinal below the open. To empty the whole instance, use an unfiltered query with a whole-prim ``ovstage_delete_attributes``. ``reset_usd`` on its own edits only the USD source. Refer to :doc:`/scene/population`.
* **``wait()`` returns the handle (Python).** ``Operation.wait()`` returns the ``Operation``, and the ``wait()`` methods of ``Query``, ``Read``, ``Map``, ``OrdinalQuery``, and ``Hierarchy`` return the handle they were called on, instead of ``None``. Code that uses ``wait()`` as a statement is unaffected; ``group = read.wait().fetch_next()`` now works. Errors still raise ``OvstageError``.
* **Stale or foreign operation ids.** ``ovstage_population_wait_op`` and ``ovstage_population_export_wait_op`` return ``OVSTAGE_ERROR_INVALID_HANDLE`` (previously ``OVSTAGE_ERROR_INVALID_ARGUMENT``) for an id that does not belong to the instance's current operation stream. The export wait also returns it for an id already consumed by an earlier terminal wait.
* **Unknown configuration keys.** ``ovstage_initialize`` rejects keys it does not recognize; earlier builds ignored them.
* **Query refresh failure.** A failed query refresh after structural changes no longer terminates the process. Recreate all ready query handles on the stage.
* **Every populated prim has ``usd-schemas``.** A prim with no applied schema carries an empty set, so ``HAS`` on ``usd-schemas`` matches every populated prim. Select by schema with ``CONTAINS``. ``RENDERING``-domain prims carry it only when they have applied schemas.
* **``apply_usd_time`` always rewrites** in-scope time-sampled properties, even when the time is unchanged. ``apply_usd_time`` and ``apply_usd_changes`` on one instance must not share an ordinal unless their attributes are disjoint.
* **Clone.** ``ovstage_clone`` rejects a repeated, empty, or unparseable target path at enqueue with ``OVSTAGE_ERROR_INVALID_ARGUMENT`` and lists all already-existing targets in its ``OVSTAGE_ERROR_PRIM_NOT_FOUND`` string. Clone and delete advance the attribute's latest ordinal. Refer to :doc:`/scene/cloning`.
* **Instance cleanup with an attached renderer.** Destroying the application's stage handle does not detach an attached ovrtx renderer. Use the renderer's public detach or destruction API to end the attachment. The destroyed stage handle is invalid; do not use it for calls or views. Follow the :ref:`stage cleanup sequence <threading-destroying-stage>`.
* **Failed writes keep completed entries.** When an entry in a batch fails after earlier entries completed, the completed entries remain applied. Repair the update and check its completion before advancing the watermark. Recoverable allocation failures report errors; a failure that prevents the runtime from keeping already-applied data consistent still terminates the process. See :ref:`ovstage-write-recovery` for recovery and the fatal-failure exception.
* **Bookkeeping prims.** A stage populated without the ``RENDERING`` domain no longer carries the two prims the runtime kept for its own bookkeeping, so an unfiltered query counts two fewer prims than in 0.2.
* **Maps (Python).** A fetched ``MapGroup`` keeps its owning ``Map``, and using group tensors after an accepted ``unmap_group`` raises. ``unmap_group`` checks ``LAYOUT_CHANGED`` against the map's own prims only.
* **Returned tensors** carry explicit compact ``strides`` instead of ``NULL``.

New in 0.3
----------

* ``ovstage_get_build_info`` / ``ovstage.get_build_info()`` and ``BuildInfo``: build string, revision, and location, available before runtime initialization. Refer to :doc:`/python_api/index`.
* ``ovstage_get_config`` / ``Stage.config`` (``StageConfig``): read back the process configuration an instance captured. Refer to :doc:`/guides/project_setup_python`.
* ``OVSTAGE_POPULATION_DEFAULT_TIME`` and ``ovstage_population_is_default_time`` (Python ``population.DEFAULT_TIME`` and ``is_default_time``): name USD's default time code, a NaN, so test it with ``is_default_time``, never ``==``. Refer to :doc:`/scene/population`.
* Clone results: ``ovstage_fetch_clone_result`` and ``ovstage_clone_result_t``; Python ``Stage.clone_async()`` returns a ``CloneOperation`` whose ``result()`` returns a ``CloneResult``, and ``Stage.clone()`` returns that ``CloneResult`` instead of ``None``. Refer to :doc:`/scene/cloning`.
* Export to an in-memory layer or stage by identifier: ``ovstage_population_export_to_usd_layer_identifier``, ``ovstage_population_export_to_usd_stage_identifier`` and their ``enqueue`` forms; Python ``population.export_to_usd_layer``, ``export_to_usd_stage``, ``export_typed_hierarchy_to_usd_layer`` and ``_async`` forms. The host that owns the USD layer or stage controls persistence. Refer to :doc:`/scene/exporting_to_usd`.
* ``usd-ancestor-types``: population records the alias names of every type a populated prim derives from, excluding its own type. Population also records the bare name of each multiple-apply schema in ``usd-schemas``. Refer to :doc:`/scene/queries` and :doc:`/scene/reading_attributes`.
* Connection attributes can be read and deleted. CPU reads return ``(target path id, target attribute token id)`` pairs; GPU reads report ``OVSTAGE_ERROR_NOT_SUPPORTED``. Refer to :doc:`/scene/reading_attributes`.
* Python ``ReadGroup.semantic``: the authored attribute semantic of a read group, as ``AttributeSemantic`` or a raw integer for values the installed enum does not name. Refer to :doc:`/python_api/index`.

Checklist
---------

Search the application for these patterns before building against 0.3.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Pattern
     - Action
   * - ``PathDictionary()``, ``PathDictionary(None)``, ``PathDictionary(stage=None)``
     - Pass a ``Stage``.
   * - ``time_code``
     - Rename to ``time`` when passed by keyword.
   * - ``untyped_type_name``, ``__ovstage_population_untyped__``
     - Remove; test for a missing ``usd-prim-type``.
   * - ``ovalign``
     - Remove the include and calls.
   * - ``ovstage_get_path_dictionary``, ``path_list``, path lists passed between instances
     - Recreate lists per instance; release before destroy.
   * - ``open_usd`` followed by another ``open_usd`` on the same instance
     - Decide whether to merge; otherwise ``reset_usd`` and ``apply_usd_changes`` first.
   * - connection writes (a connection attribute written to more than one prim)
     - One pair per target prim.
   * - ``ovstage_population_wait_op``, ``ovstage_population_export_wait_op`` error handling
     - Handle ``OVSTAGE_ERROR_INVALID_HANDLE``.
   * - ``sourceName`` read from selector-only stages
     - Read UTF-8 bytes with ``OVSTAGE_SEMANTIC_STRING``.
   * - ``HAS`` on ``usd-schemas``
     - Use ``CONTAINS`` to select by schema.
   * - ``== math.nan``, ``== NAN`` on a time value
     - Use ``is_default_time``.
   * - unfiltered prim-count assertions on non-``RENDERING`` stages
     - Expect two fewer prims.
   * - ``ovstage_initialize`` with custom configuration keys
     - Remove unknown keys.
