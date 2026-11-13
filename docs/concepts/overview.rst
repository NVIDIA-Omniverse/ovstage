.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

.. _ovstage-overview:

ovstage Overview
========================

ovstage is a standalone C and Python library that gives simulation and visualization applications one shared, in-process store for scene and simulation data. Applications can author data directly or populate it from OpenUSD, then let physics, rendering, sensors, and other consumers query, read, and update that data through a common API. The application controls execution and when updates become visible, while ovstage provides bulk data access, shared scene identities, and change tracking so each consumer can read the state it needs.

ovstage represents data in arrays for bulk access and uses DLPack tensors for CPU/GPU interchange. In this release, CUDA tensors are accepted as write sources; payload reads and map/unmap buffers use CPU memory by default. The experimental :ref:`ovstage-device-placement` extension also supports CUDA buffers. The default ``history_latest`` implementation retains current values and tracks changes. Experimental :ref:`ovstage-selective-history` can retain earlier values for explicitly enabled (prim, attribute) pairs.

Applications coordinate changes using **ordinals**, which label updates, and **watermarks** (**write floors** in the API), which mark the ordinal through which updates are immutable and available to read. This page introduces that data model and explains what consumers can read.

USD Stage and Runtime Stage
---------------------------

A **USD stage** is the composed scene: USD loads layers from disk and resolves
references, overrides, and other composition rules to present one scene.
The **runtime stage** holds the values that application components read and
update while the simulation runs. It is separate from the USD stage.

**Population** transfers selected, composed USD data into the runtime stage.
It reads resolved values at a chosen USD time and writes them at an
application-supplied :term:`ordinal`. It does not copy the layers or their
composition rules. Runtime writes do not automatically edit the source USD
layers; exporting runtime values to USD is a separate operation.

The flow below shows how a USD scene becomes runtime data. After population,
producers can write further changes directly to the runtime stage.

.. figure:: /img/overview/population-flow.svg
   :alt: USD layers on disk are loaded and composed into a USD stage. Population transfers selected resolved values to the runtime stage. Producers write changes, advance watermarks, and consumers read runtime values.
   :align: center

   *Population supplies initial runtime values. Subsequent writes and reads use
   the same ordinal and watermark model described below.*

The runtime stage groups attribute values by attribute across prims and tracks which values changed by ordinal. By default, it retains the latest values and a limited record of which prims and attributes changed. This does not provide arbitrary historical snapshots; retaining earlier values requires opting into selective history. Ordinals have no built-in time unit.

See :doc:`/scene/population` for selecting and loading USD content, and
:doc:`/scene/exporting_to_usd` for exporting runtime values.

Why Not Share One USD Stage?
----------------------------

Consider an application where a physics engine writes body transforms into a USD stage every step and a renderer reads them back. That works for a small scene on one thread, but the application then has to solve four problems itself:

* **Cost per value.** USD reads and writes go through per-prim, per-attribute calls on composed layers, and edits raise change notifications. Moving thousands of transforms that way every step is costly.
* **Knowing when a step is finished.** A renderer that reads while physics is still writing can see some bodies at the new step and others at the old one. USD has no built-in way for a producer to say "step N is complete".
* **Finding what changed.** Each consumer must rescan the scene or keep its own change log to update only what moved.
* **Different rates and threads.** Physics might step four times per rendered frame while sensors run at yet another rate. USD does not support authoring the same layer from several threads at once, so the application needs its own locking and scheduling.

ovstage handles these with three mechanisms, described in the rest of this page:

* Values are stored **by attribute, in arrays**, so one call moves an attribute for many prims. See :ref:`ovstage-why-columns`.
* **Ordinals and watermarks** let producers declare when data is complete, and consumers read only completed data. See :ref:`ovstage-why-watermarks`.
* **Change records** keyed by ordinal let a consumer ask what changed since it last looked. See :ref:`ovstage-why-range-reads`.

USD stays the authoring and interchange format. ovstage holds the working copy that runtime libraries share while the application runs.

Prims, Attributes, and Value Columns
--------------------------------------------

A **prim** is an addressable entity identified by a path, such as
``/World/BodyA``. An **attribute** is a named, typed data slot on a prim, such as
its mass. A **value column** carries values for one attribute across selected prims.

For example, this USDA layer defines two prims with the same two attributes:

.. filtered-literalinclude:: /img/overview/bodies.usda
   :language: usda
   :exclude-pattern: ^# SPDX-

Assume population selects both attributes on both prims and writes their values
at ordinal 1. The diagram shows those values after the watermark advances to 1.
Each horizontal row is an attribute column; entries within it belong to different
prims. This is a logical layout, not a promise of one contiguous memory allocation.

.. figure:: /img/overview/runtime-layout.svg
   :alt: At ordinal 1, the mass column contains BodyA 2.0 and BodyB 3.0. The velocities column contains BodyA [(1, 0, 0)] and BodyB [(0, 1, 0)]. Both columns are red after the watermark reaches 1.
   :align: center

   *USD groups attributes under each prim. The runtime view groups values by
   attribute across prims. Red marks changes at or below the watermark.*

.. _ovstage-why-columns:

Why Data Is Organized by Attribute
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Simulation and rendering code usually processes one attribute across many prims at a time: every body's velocity, every mesh's transform. The ovstage API is organized the same way. Reads and writes address an attribute across a selection of prims and move its values as arrays, not one prim at a time. This gives applications three things:

* **One call per attribute, not per prim.** A read or write moves an attribute for every selected prim, so per-call overhead is paid once per attribute instead of once per prim.
* **Data ready for array code.** Values cross the API as DLPack tensors, so NumPy, PyTorch, Warp, or a CUDA kernel can use them without per-prim conversion. CUDA tensors are accepted as write sources. Reads use CPU memory by default, with optional :ref:`ovstage-device-placement` for CUDA consumers.
* **Only the attributes you need.** A consumer reads the columns it uses and never touches the rest of each prim.

The trade-off is that fetching many different attributes from one prim at a time is not the fast path. Select prims with a query and read attributes in bulk instead.

Prims need not have the same attributes; an absent value is different from zero.

An attribute value can be a scalar, a fixed-size vector or matrix, or an array.
Array lengths can differ between prims. The data type, array kind, and semantic
information describe how to interpret a value.

A **query** selects prims. Reads and writes combine that selection with attribute
names. Values cross the API as **tensors**: typed arrays described using
`DLPack <https://dmlc.github.io/dlpack/latest/>`_.
The instance's **path dictionary** maps names and prim paths to reusable identifiers.
Use that dictionary so your application and ovstage use compatible identifiers.

A **change** is a write or deletion applied to an attribute on a prim at a
particular ordinal. One operation can make changes to many prims or attributes.
A record of a change identifies what changed and when; it does not guarantee
that the value written by that change is still stored.

A **watermark** marks the ordinal through which writes are finished for the
attributes it covers. Once it reaches ordinal N, writes at N or any earlier
ordinal are rejected; further changes must use ordinals greater than N.
Values written at or below the watermark can be read if they are still available.
Values written beyond it cannot yet be read. Advancing the watermark does not
preserve earlier values or restore values replaced by later writes.

The diagram below shows changes to two attributes on one prim. Each row is an
attribute; each rectangle is a change at the ordinal above it. The vertical line
marks the watermark. Red marks changes at or below it; green marks changes
beyond it. The tinted row backgrounds show the same boundary, including ordinals
where no change is shown.

.. figure:: /img/overview/data-model.svg
   :alt: Mass and velocities are horizontal rows of changes at ordinals 8 through 14. With watermark 9, changes at 8 and 9 are red and changes at 10 are green.
   :align: center

   *Red means no further writes are accepted at those ordinals. Green means the
   watermark does not yet prohibit writes there; other write constraints still
   apply. This timeline shows changes, not a
   guarantee that their historical values remain available.*

Ordinals and Simulation Time
------------------------------------

An **ordinal** is an application-defined sequence number that labels changes.
ovstage represents it as a signed 64-bit integer with no built-in time unit:
it is up to the application to decide how time maps to ordinals (if at all).
See :ref:`ordinal-bounds-and-initial-state` for the named bounds and initial
sealed state.

An application can use several ordinals within one simulation step—for example,
one for input changes and another for the resulting physics updates. The
application defines what each ordinal represents and which ordinals each consuming
library, such as ovphysx or ovrtx, reads.

Library Clocks and Stage Updates
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

ovphysx and ovrtx each track simulation time internally:

* **ovphysx:** ``PhysX.step(dt)`` advances its clock by ``dt`` seconds. With an
  ovstage attached, ``update_from_ovstage(from_ordinal, to_ordinal)`` first applies
  input changes from that inclusive ordinal range. Complete the watermark advance
  before requesting the update, then step physics. See the
  `ovphysx API <https://nvidia-omniverse.github.io/PhysX/ovphysx/latest/python_api.html#ovphysx.api.PhysX.update_from_ovstage>`_.
* **ovrtx:** ``Renderer.step(..., delta_time=dt, ordinal=N)`` updates from the
  attached ovstage through at least N, then advances its sensor simulation clock
  by ``dt`` seconds. It may observe newer stage values; N does not select a
  historical snapshot. In C, call ``ovrtx_update_from_stage()`` followed by
  ``ovrtx_step_with_stage()``. See the
  `ovrtx integration guide <https://nvidia-omniverse.github.io/ovrtx/core/ovstage_integration.html>`_.

Calling ``physx.step(dt)`` advances physics time. Calling
``renderer.step(..., delta_time=dt, ordinal=N)`` advances renderer time and consumes
ovstage updates through at least ordinal N. These are separate actions: writing
data at ordinal N or advancing the watermark does not advance either clock.
The application decides which simulation time the values written at N represent.

Writes, Watermarks, and Reads
-------------------------------------

A **watermark** sets the boundary for further writes to the attributes it covers.
Once the watermark reaches ordinal **N**, no further writes at **N** or any earlier
ordinal are accepted. Advancing it to **N** declares that all writes through **N**
are finished. Later updates must use ordinals greater than **N**. This does not
guarantee that earlier values remain available to read.

.. note::

   **API naming:** as noted above, the API uses ``write_floor`` for a watermark. For example, :meth:`ovstage.Stage.advance_write_floor()` advances a watermark. This documentation preserves existing API identifiers.

The simplest update cycle is:

1. Submit writes at a new ordinal.
2. Wait for every required write and check its result. If a write fails, repair the update before continuing; see :ref:`ovstage-write-recovery`.
3. Advance the watermark to cover the completed update for the required attributes.
4. Wait for the watermark advance and check its result.
5. Read and consume the results.
6. Release the results before submitting overlapping updates.

A successful watermark advance does not report failures from earlier writes. For example, if updating body A succeeds and updating body B fails, advancing the watermark can make A's new value readable alongside B's old value. Check and repair the writes before declaring the step complete.

The snippets below write three ``temperature`` values at ordinal 1, advance the
watermark, and read the values back. ``query`` selects ``/World/A``, ``/World/B``,
and ``/World/C``; ``attr`` identifies ``temperature`` in the path dictionary.
The Python example imports NumPy as ``np`` and ``OrdinalRange`` from ``ovstage``.
See :doc:`application_flow` for creating the stage and query.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:minimal-write-read]
         :end-before: # [/snippet:minimal-write-read]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      This example calls the C API from C++. ``attrArg`` wraps the attribute
      token for the write call. The example's ``waitOp()`` helper checks enqueue
      status, waits for completion, and releases operation tracking; ``check()``
      handles status errors.

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:minimal-write-read]
         :end-before: // [/snippet:minimal-write-read]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

      The example also releases the read handle during cleanup:

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:overview-release-read]
         :end-before: // [/snippet:overview-release-read]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

This small, uniform column returns one group of results. General consumers must
fetch all groups, as described in :doc:`/scene/reading_attributes`.

The initial watermark is ``OVSTAGE_ORDINAL_MIN`` (Python:
``ovstage.ORDINAL_MIN``), so the first write must use a greater ordinal.
Watermarks never move backwards; a lower advance has no effect.

The two panels below show the same changes before and after a watermark advance.
The rectangles at ordinal 10 turn from green to red when the watermark reaches 10.
Assume these are the latest changes and no further writes occur between the panels.

.. figure:: /img/overview/write-read.svg
   :alt: Before the watermark advances, changes at ordinal 10 are green and cannot yet be read. After it advances from 9 to 10, the same changes are red and can be read.
   :align: center

   *Observe the watermark advance before issuing dependent reads. Release the results
   before updating the same values again.*

An accepted enqueue does not mean that an operation has completed. Work can execute
before you wait; waiting observes completion and reports execution errors.

Writes at the same ordinal execute in submission order. Different ordinals do not
establish a general execution order. Use explicit coordination when one update
depends on another. The default ``NONDECREASING`` admission policy allows equal
unsealed ordinals but rejects older retained changes on the same cells;
``STRICTLY_INCREASING`` also rejects equal ordinals. See
:ref:`ordinal-admission-policies`. Sharing an ordinal does
not make a batch transactional. See :doc:`/scene/writing_attributes`
for write admission and partial-failure behavior.

A watermark advance is ordered after preceding writes, deletes, and map commits
at or below its target ordinal in the affected scope. Work at higher ordinals
remains independent. Reads do not automatically wait for unrelated writes or
watermark advances: observe successful completion of the advance before issuing
dependent reads.

.. _ovstage-why-watermarks:

Why Ordinals and Watermarks
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Several libraries share one stage, each with its own clock and rate, and none of them calls the others. Ordinals and watermarks are how they agree on order and completeness without locks or callbacks between them.

* **Ordinals give every change a place in one shared order.** The application assigns them, so it can encode both time and phase. For example, inputs for step N could use one ordinal and physics results for step N the next one. ovstage does not own a clock. Each library keeps its own time, and the ordinal records where a change belongs.
* **A watermark is the producer's "done" signal.** After checking that the required writes succeeded, the producer advances it to N to declare the update complete. Reads reject selected values beyond the watermark (``WRITE_FLOOR_VIOLATION``), but the watermark does not make a batch transactional or prove that every intended write succeeded.
* **Producers and consumers stay decoupled.** A producer completes and checks its writes, then advances the watermark. A consumer reads whatever is sealed. The application coordinates when each runs and handles failures.
* **Per-attribute watermarks avoid waiting on unrelated work.** Physics can seal transforms while another producer is still writing a different attribute, and consumers of transforms can proceed. See `Per-Attribute Watermarks`_.
* **Independent writes can be submitted together.** Keep each operation and its input buffers valid, then check every write's completion before advancing the watermark. Waiting only for the advance does not check those write results. Sequence dependent writes explicitly; different ordinals do not guarantee execution order.

A watermark is a completion signal, not a snapshot. It does not keep older values for a consumer that falls behind; see `Consuming Results Safely`_.

Per-Attribute Watermarks
--------------------------------

Different producers can finish different attributes at different times. You can
advance a watermark for all attributes, only named attributes, or all known
attributes except named exclusions.

For an attribute with watermark **W**:

* New writes at ordinals **less than or equal to W** are rejected.
* Writes at ordinals **greater than W** remain subject to normal write validation.
* Reads of stored attribute values require the watermark to have reached the
  ordinals of the selected changes.

For example, consider a stage with two known attributes:

.. figure:: /img/overview/watermarks.svg
   :alt: Mass and velocities have changes at ordinals 8, 10, and 12. The mass watermark is 12, so all its changes are red. The velocities watermark is 10, so its change at 12 is still green.
   :align: center

   *The mass producer has finished writes through 12; the velocities producer has
   finished writes through 10. The minimum watermark across these attributes is 10.*

Query an attribute's watermark with :meth:`ovstage.Stage.get_attribute_write_floor()`. Passing ``None`` queries the global value, called the **global write floor** in the API: the minimum across the attributes currently on the stage, but never below the floor set by advancing all attributes. It does not report the most advanced producer.

An attribute need not change at every step. Its current value may have been
written at an ordinal below the watermark.

What Reads Return
-------------------------

With the default ``history_latest`` implementation, reads use the **latest values**
plus a limited record of which attributes changed on which prims. The table and
example below describe this default. For retained historical values, see the
experimental :ref:`ovstage-selective-history` contract.

There are two read forms:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Read form
     - Meaning with the default ``history_latest`` implementation
   * - Snapshot (``OrdinalRange.latest(end)``)
     - Checks every queried path for each requested attribute. A recorded change
       after ``end`` makes the older value unavailable. Pass ``OVSTAGE_ORDINAL_MAX``
       (``ovstage.ORDINAL_MAX`` in Python) for current values; selected changes must
       still be sealed. There is no implicit default end.
   * - Inclusive change range ``[start, end]``
     - Selects attributes on prims that changed within the interval. Returns their
       latest values or deletion markers only when that fixed interval can still
       be served.

The diagram below follows ``mass`` on ``/World/BodyA`` through two writes.
At each step, compare a current read (end ``OVSTAGE_ORDINAL_MAX``) with a read
restricted to ``[10, 10]``.
That range includes only ordinal 10. Each panel shows new reads after the stated
operation completes; previous results have been released. In step 1, no change
exists at or below watermark 9. The current read fails because the only change is
at 10, beyond the watermark, not because the earlier interval is empty. Both change records
remain retained throughout the example.

.. figure:: /img/overview/read-ranges.svg
   :alt: Four steps compare current reads (end OVSTAGE_ORDINAL_MAX) and range reads. Writing 2.0 at ordinal 10 requires watermark 10 before either read succeeds. A later write of 3.0 at ordinal 11 makes the earlier value unavailable to read, so the range read fails. Advancing the watermark to 11 allows a current read of 3.0 but does not restore 2.0 for the range read.
   :align: center

   *Advancing the watermark lets the current read return 3.0. It cannot restore
   2.0 for the fixed range. The crossed outline marks the earlier change whose
   value is no longer available to read.*

A **watermark violation** is reported as
``ErrorCode.WRITE_FLOOR_VIOLATION``. The check applies to the selected recorded
changes at or before the requested end, rather than comparing the end itself
with the watermark. Lowering a snapshot's end below a recorded change instead
makes it ``OUT_OF_RANGE``; it does not recover an earlier value. In steps 1 and
3, a snapshot ending at the watermark therefore has a different error from the
current read shown. See :doc:`error_handling` for how enqueue, wait, and fetch
expose these errors.

``ErrorCode.OUT_OF_RANGE`` means values needed for the requested snapshot or
change range are no longer available. This can happen even when history is enabled,
because retained values can be discarded. Snapshots select every queried path;
change ranges select only pairs changed within the interval. Advancing the watermark
cannot restore discarded values. Choose a newer interval or read current state,
according to the consumer's needs.

Records of older changes can also be discarded. See
:ref:`ovstage-read-retention` before relying on an old interval.

Queries resolve against current scene state. They do not reconstruct
which prims existed at an earlier ordinal. Latest reads of derived metadata
(``usd-path``, ``usd-parent``, and ``usd-children``) reflect current structure and
are not gated by the watermark. Range reads of ``usd-parent`` and ``usd-children``
return no groups. A range read of ``usd-path`` can return whole-prim deletion
tombstones and requires its effective write floor to cover the range end. See
:ref:`ovstage-read-derived-metadata` for the deletion and recreation rules.

A current read (end ``OVSTAGE_ORDINAL_MAX``) answers "what are the values now?". A change-range read answers "what changed since I last looked?", which is what a consumer needs when it keeps its own copy of the scene. See :ref:`ovstage-why-range-reads` for where it fits in an application.

.. _ovstage-why-groups:

Why Reads Return Groups
^^^^^^^^^^^^^^^^^^^^^^^

A read returns one or more **groups**. Each describes its values and the selected prims they belong to, and each must be released separately. This supports a simple processing sequence:

* **Interpret each result.** Use the group's tensor descriptors and prim-to-row mapping. Results use CPU memory by default; inspect the returned device when using :ref:`ovstage-device-placement`. Do not assume a particular number of groups or that every group's rows match the full query.
* **Finish using the data before releasing it.** Writes, maps, and deletes that overlap a held group fail with ``OP_FAILED``, as does any operation, including a clone, that would give ``omni:fabric:worldMatrix`` to a prim the group covers: a hierarchy parent or ancestor of the operation's prims, or a prim with a transform input between one of them and a descendant that carries it (see :doc:`/scene/transforms`). Copy values that must remain available after release.
* **Process and release each group.** Release groups promptly so subsequent operations can update their data. Release the read handle after processing the results.

See :doc:`/scene/reading_attributes` for the fetch and release pattern.

Consuming Results Safely
--------------------------------

Keep read results alive while using their values, and release them before
updating the same data. Copy values that must outlive the results. See
:doc:`/scene/reading_attributes` for fetching and releasing results.

For a coherent step across several attributes, coordinate the producers, advance
the watermark for the required scope, consume the results, and release them before
those values are overwritten. A watermark alone does not retain that step for a
consumer that falls behind.

Further Reading
-----------------------

* :doc:`application_flow` — a complete create, write, advance-watermark, and read example.
* :doc:`async_model` — operation ordering, waits, and timeouts.
* :doc:`/scene/queries`, :doc:`/scene/writing_attributes`, and
  :doc:`/scene/reading_attributes` — selection and data access.
* :doc:`dlpack_tensors` — layouts, CPU/CUDA support, and buffer lifetimes.
* :doc:`/scene/population` and :doc:`/scene/exporting_to_usd` — moving data between
  USD and the runtime stage.

Where to Go Next
-----------------------

* :doc:`/concepts/choosing_api` — which call to use for writing, reading, selecting prims, loading data, and tracking changes with several consumers.
* :doc:`/concepts/limitations` — what this release does not do, and which behaviors can change.
* :doc:`/concepts/threading_lifetime` — which calls block, which threads may call them, and how long handles and buffers stay valid.

.. _ovstage-overview-glossary:

Glossary
----------------

.. container:: overview-glossary

   .. glossary::

      USD stage
         The composed scene obtained by resolving USD layers and composition rules.

      Runtime stage
         The data store that holds attribute values and tracks changes by ordinal
         for application components to read and update.

      Population
         Transferring selected, composed USD data into the runtime stage at a
         supplied ordinal.

      Prim
         An addressable entity identified by a prim path, such as ``/World/BodyA``.

      Attribute
         A named, typed data slot on a prim, such as its mass.

      Change
         A write or deletion applied to an attribute on a prim at a particular
         ordinal. One operation can make many changes.

      Ordinal
         An application-defined sequence number that labels changes. It has no
         built-in time unit. "Values written at ordinal N" refers to the data;
         "ordinal N" refers to the number.

      Watermark
         A boundary for further writes to the attributes it covers. Once it reaches
         N, no further writes at N or any earlier ordinal are accepted. Earlier
         values are not guaranteed to remain available to read.

      Operation ID
         An identifier for an asynchronous operation, used to wait for its completion. In C it is the ``op_index`` field of the enqueue result (type ``ovstage_op_id_t``); in Python it is ``Operation.op_id``. Population operations use a separate identifier space (``ovstage_population_op_id_t``) and their own wait call. It is separate from the ordinal attached to a change.

      Token
         An identifier returned by the path dictionary for a string, such as an
         attribute name. Reuse it to refer to the same name without passing the
         string again.

      Tensor
         A typed array used to exchange attribute values. Its DLPack description
         gives the buffer's element type, shape, memory device, and strides
         (the distances between elements).

      Global watermark
         The watermark across all attributes (the global write floor in the API): the minimum of the attribute watermarks on the stage, never below the floor set by advancing all attributes. A consumer tracking everything uses it as the upper bound of what is complete.

      Latest committed state
         The most recent value stored for an attribute on a prim. A current read (end ``OVSTAGE_ORDINAL_MAX``) returns it once the watermark has reached the ordinals of the selected changes. The default ``history_latest`` implementation does not keep earlier values.

      Change range
         An inclusive ordinal interval ``[start, end]`` used in a read. The read selects prims and attributes that changed in the interval. With ``history_latest``, it returns their latest values or deletion markers and fails with ``OUT_OF_RANGE`` if a selected attribute changed again after ``end``. Experimental selective history can serve retained earlier values for enabled pairs. Also called an ordinal-range read.

      Retention frontier
         The oldest ordinal for which change records are exact, reported by ``get_oldest_preserved_ordinal``. Older records may have been combined or discarded.

      Query
         A handle to a fixed list of prims, produced by a filter or from a path list. Reads, writes, maps, and deletes combine a query with attribute names. Membership does not change after the query runs.

      Group
         One part of a read result (``ReadGroup``): a shared set of logical prim rows and one or more attribute columns. Each column holds its own tensors and maps logical rows to its values independently. One read can return many groups; fetch every group and release each one.

      Path list
         A list of interned prim paths created through one instance's path dictionary. Keep the list valid while using it with a query, then release it through that dictionary.

      Path dictionary
         The public interface for interning strings as tokens and prim paths as path identifiers, and for managing path lists. Obtain it from the stage and use it only while that stage is valid.

      Predicate
         In a query, one condition (attribute, operator, values) of a filter; predicates in a filter are combined with AND. In population, a prim or property predicate that selects which USD prims and properties to copy.

      Population domain
         A predefined selection of USD prims and properties for a consuming library: ``RENDERING``. ``NONE`` selects nothing and leaves selection to explicit selectors.

      Lanes
         The number of components per element in a tensor's ``dtype.lanes``. Canonical writes encode a 3-vector with 3 lanes and a 4x4 matrix with 16. Reads and maps use scalar lanes and trailing component dimensions, such as ``(N, 3)`` and ``(N, 4, 4)``.

      index_map
         A list that maps each logical element to a row of the data. On a write, ``index_map[i]`` picks the source row for prim i, so rows can be shared or reordered. On a read group, it maps selected prims to stored data rows.

      Semantic
         The authored meaning of a column's values, such as point, matrix, token, or path (``AttributeSemantic``). It is independent of the element type, and identifier semantics require values interned through the path dictionary.
