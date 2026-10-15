.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Transforms
==========

Two :term:`attributes <Attribute>` describe a :term:`prim <Prim>`'s transform:

- ``omni:xform`` is always the prim's **local** transform.
- ``omni:resetXformStack`` controls whether the parent's world transform
  participates in computing the prim's world transform.

Set ``omni:resetXformStack=True`` when supplying object-to-world transforms
explicitly for each prim, such as those returned by a rigid-body physics simulation.
Each prim then uses
its supplied transform directly, without also applying its parent's transform.

Set ``omni:resetXformStack=False`` when a prim should move with its parent.
For example, place a car's wheels under its body and give each wheel a transform
relative to the body. Moving the body moves the whole car, while updating each
wheel's local transform lets it rotate without losing its position relative to
the body.

The examples below run in order on a USD-populated hierarchy. ``query`` selects
``/World/Car/Wheel``; its parent, ``/World/Car``, has a translation of 10 along X.
Both initially inherit their parent transforms. The runnable ``transforms``
example selects every ``Xform`` during population, which creates the transform
and derived-output rows used here. ``paths`` is the stage's :class:`PathDictionary <ovstage.PathDictionary>`
(``dict`` in C). The C helpers ``waitOp()``, ``check()``, and ``checkOvx()`` wait
for operations and check stage or dictionary errors.

Transform Data Layout
---------------------

``omni:xform`` is a fixed-size, non-array attribute with:

- ``float64`` values with 16 DLPack lanes;
- ``AttributeSemantic.MATRIX``;
- one row-major 4x4 matrix per queried prim.

ovstage follows the USD row-vector matrix convention. Translation occupies
matrix elements 12, 13, and 14 (the first three elements of the last row).

``omni:resetXformStack`` is a fixed-size, non-array scalar with DLPack dtype
:class:`DLDataType(code=DLDataTypeCode.kDLBool, bits=8, lanes=1) <ovstage.DLDataType>`. Use boolean values;
an integer column is not valid even if it has the same byte width.


These snippets prepare a matrix translating the wheel by 4 along X and a
boolean reset value. Python supplies a compact ``(1, 4, 4)`` array; C uses one
16-lane tensor row:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/transforms/main.py
         :language: python
         :snippets: transform-imports, transform-layout

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/transforms/main.cpp
         :language: cpp
         :snippets: transform-layout-c

Writing Local or World Transforms
---------------------------------

Use these combinations when writing a transform:

.. list-table::
   :header-rows: 1
   :widths: 28 26 46

   * - Intent
     - ``omni:resetXformStack``
     - Result
   * - Author a local transform
     - ``False``
     - ``omni:xform`` composes with the parent's world transform.
   * - Set a world transform
     - ``True``
     - Parent composition stops at this prim, so its effective world transform
       is exactly ``omni:xform``.

With reset enabled, the prim ignores its parent transform. Its children still
inherit its transform. ``omni:xform`` remains the local matrix.

The reset value persists until another write or an xform-related USD
synchronization replaces it. While it remains ``True``, later transform
updates need only write ``omni:xform``. When changing both attributes, write
them at the same :term:`ordinal <Ordinal>` and complete both before advancing the
:term:`watermark <Watermark>` or computing world transforms.
:meth:`ovstage.Stage.write_attributes()` groups the writes in one operation,
but does not make them transactional.


These snippets write the matrix and reset value prepared above. With reset
set to false, the wheel keeps its position relative to the car:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/transforms/main.py
         :language: python
         :snippets: transform-write

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/transforms/main.cpp
         :language: cpp
         :snippets: transform-write-c

Deriving World Transforms
-------------------------

Call :meth:`ovstage.Stage.compute_hierarchy()` to compute world transforms.
Choose a model returned by :meth:`ovstage.Stage.get_hierarchy_computation_models()`.

The :class:`ovstage.HierarchyComputationModel` values choose where and how
hierarchy data is updated:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Model
     - Behavior
   * - ``CPU_INCREMENTAL``
     - Updates affected hierarchy data on the CPU.
   * - ``GPU_INCREMENTAL``
     - Updates affected hierarchy data on the GPU.
   * - ``GPU_GLOBAL``
     - Recomputes hierarchy data globally on the GPU.

``DEFAULT_CPU`` currently aliases ``CPU_INCREMENTAL``; ``DEFAULT_GPU`` aliases
``GPU_GLOBAL``. ``RUNTIME_DEFAULT`` uses the process default captured when the
stage was created, which defaults to ``CPU_INCREMENTAL``. It does not select a
GPU model merely because the input buffers are on the GPU.

Check :meth:`get_hierarchy_computation_models() <ovstage.Stage.get_hierarchy_computation_models>` for availability, then pass the
chosen model as ``model=`` to :meth:`compute_hierarchy() <ovstage.Stage.compute_hierarchy>`. The CUDA examples below
select ``GPU_GLOBAL`` explicitly and wait for computation to complete.

In the current release, CPU incremental hierarchy computation supports
USD-populated transform hierarchies. A hierarchy created entirely through
client writes requires a supported GPU computation model and pre-existing
derived-output rows. Refer to the ``authoring-hierarchy`` pair in
:doc:`/examples/index` for that workflow.
Its clone snippet customizes environment roots at ordinal 3 but seals only
ordinal 2. Ordinal 3 remains open until the later output-row initialization and
hierarchy computation finish; the example then seals 3 before reading those
results.

To compute transforms for one update:

#. Write ``omni:xform`` and, when needed, ``omni:resetXformStack`` at ordinal
   ``N``.
#. Wait for the writes to complete.
#. Call :meth:`stage.compute_hierarchy(input_ordinal=N, output_ordinal=N, model=model) <ovstage.Stage.compute_hierarchy>`.
   This call blocks. If using :meth:`compute_hierarchy_async() <ovstage.Stage.compute_hierarchy_async>`, wait for its operation
   before advancing the watermark.
#. Advance the :term:`watermark` to N and wait before reading the results.

The input and output ordinals do not select historical transforms. Computation
uses current values. Do not overlap writes from a later update with the
computation. Reads and attached
consumers can also trigger runtime-default propagation, so do not use a read
to check whether transforms are stale. Call :meth:`compute_hierarchy() <ovstage.Stage.compute_hierarchy>` when your
application needs to control when they are updated.


These snippets select the supported CPU model, compute the wheel's world
transform, and then advance the watermark:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/transforms/main.py
         :language: python
         :snippets: transform-compute

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/transforms/main.cpp
         :language: cpp
         :snippets: transform-compute-c

How This Differs from Editing USD
---------------------------------

The inheritance semantics match USD's reset-xform-stack behavior, but the
representation and data ownership differ:

- USD encodes a reset in ``xformOpOrder`` as ``!resetXformStack!``.
- ovstage exposes the mirrored runtime state as an independently writable
  ``omni:resetXformStack`` runtime attribute.

When population includes a prim that supports USD transforms, it initializes
``omni:xform`` and ``omni:resetXformStack`` from USD. See
:ref:`Population — Ancestors and Transforms <population-ancestors-transforms>`
for how prims are selected. An application can then change either value in
ovstage without editing the USD source. For example, setting the runtime
reset solely to publish world-space simulation poses can make ovstage's reset
value differ from the reset authored in USD. Do not assume the runtime value
still describes the source layer after such a write.


These snippets change only the runtime reset to true. The source USD still
inherits the parent transform, while the runtime wheel now uses its supplied
matrix as its world transform:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/transforms/main.py
         :language: python
         :snippets: transform-override

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/transforms/main.cpp
         :language: cpp
         :snippets: transform-override-c

Reading World Transforms
------------------------

Read ``omni:fabric:worldMatrix`` to get a prim's computed object-to-world
matrix. It combines the prim's ``omni:xform`` with its ancestors' transforms,
stopping wherever ``omni:resetXformStack`` is true. Treat it as **read-only
output**. To change a pose directly in the runtime stage, write ``omni:xform``
and ``omni:resetXformStack``, then compute the hierarchy.

There are two separate requirements: **the output attribute must exist**, and
**hierarchy computation must have finished for the update you want to read**.
An existing attribute alone does not mean its value is up to date.

When Does the Attribute Exist?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Being transformable in USD does not, by itself, guarantee that a prim has
``omni:fabric:worldMatrix`` in the runtime stage. Population must include the
prim, and the population path must create this output:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - How the prim enters the runtime stage
     - World-matrix output
   * - Population without ``RENDERING``, including ``PHYSICS`` or explicit selectors
     - Population creates the attribute for each in-scope USD-transformable
       prim. Its initial value is the **local** matrix; compute the hierarchy
       before using it as a world matrix.
   * - Population with ``RENDERING``, including ``ALL``
     - The rendering domain owns ``omni:fabric:worldMatrix`` on ordinary ``Xform``
       and geometry prims. :ref:`Generated instance prims <population-instance-types>`,
       such as ``XformInstance`` and ``MeshInstance``, can store a compact world
       transform instead. Check for ``omni:fabric:worldMatrix`` on these instance
       prims before reading it.
   * - Prims created entirely through runtime writes
     - Writing ``omni:xform`` does not automatically create the world-matrix
       attribute. Initialize placeholder output rows once during hierarchy
       setup, then let computation fill them. See the ``authoring-hierarchy``
       example in :doc:`/examples/index`.

:meth:`compute_hierarchy() <ovstage.Stage.compute_hierarchy>` updates existing output rows; it does not create
missing ones. Repeating the computation will not fix a missing attribute.
Check the population selection and hierarchy setup instead. See
:ref:`population-ancestors-transforms` for selection rules and
:doc:`queries` for checking whether an attribute exists on a prim.

When Is the Value Ready to Read?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**USD changes applied through rendering population:** when ``RENDERING`` is
enabled (including ``ALL``), population updates world transforms as it processes
USD changes. After editing USD, call :func:`population.apply_usd_changes() <ovstage.population.apply_usd_changes>` and
wait for completion if using its asynchronous form. Then advance the
:term:`watermark` to the update's :term:`ordinal` and wait before reading.
You do not need a separate :meth:`compute_hierarchy() <ovstage.Stage.compute_hierarchy>` call for those USD changes.
Editing USD alone does not update the runtime stage; population must process
the changes first. It updates affected transforms, rather than necessarily
recomputing every matrix for every USD edit.

**Direct runtime transform writes, or population without rendering:** use
explicit hierarchy computation before reading. For direct runtime writes,
this applies even if the stage was originally populated with ``RENDERING``.
Follow this order:

#. Finish population or writes to the transform inputs.
#. Call :meth:`compute_hierarchy() <ovstage.Stage.compute_hierarchy>` with that ordinal for both ``input_ordinal``
   and ``output_ordinal``. If using :meth:`compute_hierarchy_async() <ovstage.Stage.compute_hierarchy_async>`, wait for it.
#. Advance the :term:`watermark` to that update's :term:`ordinal` and wait.
#. Read ``omni:fabric:worldMatrix`` with :meth:`OrdinalRange.latest(ordinal) <ovstage.OrdinalRange.latest>`.
   Finish reading before writing the next update.

Use a supported computation model as described in `Deriving World Transforms`_.

.. note::

   The current implementation uses the latest committed inputs;
   ``input_ordinal`` does not select a historical snapshot. Neither ordinal
   preserves old matrices. Updated world transforms participate in change reads
   at ``output_ordinal``, even if that ordinal is already sealed. Consumers that
   already read that ordinal will miss these updates, so compute first and then
   advance the watermark.

   Advancing the watermark for ``omni:fabric:worldMatrix`` also recomputes world
   transforms when their inputs have changed, using the runtime default model,
   and makes those changes available at the advance ordinal. Compute explicitly
   first to choose the model.

For the car and wheel above, the car is at X=10 and the wheel's local matrix
has X=4:

.. list-table::
   :header-rows: 1

   * - Wheel's ``omni:resetXformStack``
     - Computed wheel world X
   * - ``False``
     - 14: the wheel inherits the car's translation.
   * - ``True``
     - 4: the wheel ignores its parent's translation.

The following snippets run after the reset override and computation at ordinal
3, so they print ``Wheel world x: 4.0``. They fetch every result group and map
each prim to its matrix row. An absent output row is not an identity matrix
and must not be treated as one.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/transforms/main.py
         :language: python
         :snippets: transform-imports, transform-read

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/transforms/main.cpp
         :language: cpp
         :snippets: transform-read-c

The attribute stores one row-major 4x4 ``float64`` matrix per prim, with the
same matrix convention as ``omni:xform``. The Python snippet reshapes the
returned values to ``(row_count, 4, 4)``; the C tensor has the same shape
with scalar lanes and compact row-major strides.
These unplaced reads return CPU buffers, even if hierarchy computation ran on
the GPU. Use :ref:`ovstage-device-placement` for a CUDA read.
See :doc:`reading_attributes` for result-group ownership and row mapping.

Writing Transforms From CUDA Buffers
------------------------------------

Supply CUDA buffers directly to :meth:`write_attribute() <ovstage.Stage.write_attribute>` to update transforms
without copying their values back to the CPU. Use one contiguous, row-major
``float64`` 4×4 matrix per prim in query order, with ``is_array=False`` and
``AttributeSemantic.MATRIX``. Python accepts a CUDA DLPack producer
with shape ``(prim_count, 4, 4)`` or ``(prim_count, 16)``; C describes
the device pointer with a ``DLTensor`` whose device is ``kDLCUDA``.

For object-to-world matrices, set ``omni:resetXformStack`` to true for each prim.
The examples below also supply these boolean flags from a CUDA buffer. If reset
is already set correctly, subsequent updates need only write ``omni:xform``.

Record a CUDA event after producing both buffers and pass it with the writes.
Keep the buffers and event alive, and do not overwrite the buffers until the
writes complete. Waiting on an operation synchronizes completion; it does not
copy the transform values to the CPU.

To keep the write payloads on the GPU, use dense inputs in query order, without
``index_map`` or ``mask``. The current sparse fixed-size write path can copy
inputs through CPU memory. Do not call :meth:`read_attributes() <ovstage.Stage.read_attributes>` or map the values
without placement in this workflow: unplaced reads and maps return CPU buffers.
The experimental :ref:`ovstage-device-placement` can instead select CUDA buffers.
If world transforms must also be computed on the GPU, select a supported GPU
hierarchy computation model rather than ``CPU_INCREMENTAL``. GPU writes alone
do not determine where a downstream consumer performs its work.

These helpers accept buffers already produced on the GPU and compute the
hierarchy using ``GPU_GLOBAL``. Use a populated hierarchy with existing
derived-output rows, as in the earlier example. ``prim_count`` must
match the query's prim count, and ``ordinal`` must be above the watermark.
The Python ``ready_event`` is the integer handle of the recorded CUDA event;
the C helper accepts a ``cudaEvent_t``. Neither helper reads the values back:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/transforms/gpu_write.py
         :language: python
         :snippets: gpu-transform-write

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/transforms/gpu_write.cpp
         :language: cpp
         :snippets: gpu-transform-write-c

Further Reading
---------------

- :doc:`writing_attributes` — tensor shapes, semantics, ordinals, and watermarks.
- :doc:`population` — when population publishes transforms and reset state from USD.
- :doc:`/guides/runtime_loop` — a complete populate and update loop.
