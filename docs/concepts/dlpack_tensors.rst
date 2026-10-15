.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

DLPack Tensor Exchange
==============================

ovstage exchanges :term:`attribute <Attribute>` values as DLPack :term:`tensors <Tensor>`.
Each tensor describes a buffer's element type, shape, memory device, and strides
(the distances between elements).

The snippets come from separate examples and tests; do not run them as consecutive
steps in one program. ``stage`` / ``stage_`` and ``query`` / ``query_`` are
already created. Python snippets use NumPy as ``np``. The C tabs call the C
API from C++. Test snippets use assertions to check calls, ``waitOp()`` to wait
and check asynchronous errors, and ``seal(N)`` to advance the watermark and wait.

Passing Attribute Values
--------------------------------

Pass tensors to :meth:`ovstage.Stage.write_attribute()` and read them through
:class:`ovstage.ReadGroup` or :class:`ovstage.MapGroup`. They associate tensor
values with :term:`prims <Prim>`. On writes, set ``is_array`` explicitly. Shape and tensor
count do not determine whether an attribute is array-valued.

.. list-table::
   :header-rows: 1

   * - Value per prim
     - ``is_array``
     - Write tensors
   * - Scalar, vector, or matrix
     - ``False``
     - One tensor containing the values for all selected prims.
   * - Variable-length array
     - ``True``
     - One tensor per data row, or one packed tensor. See
       :doc:`/scene/writing_attributes` for array layout rules.

The :class:`ovstage.DLTensor` reference describes ``data``, ``device``, ``dtype``,
``shape``, ``strides``, and ``byte_offset``. Within ``dtype``, ``code`` identifies
the scalar type, ``bits`` its width, and ``lanes`` the component count.

For a complete write-and-read example in Python and C, see :doc:`application_flow`.

The snippets below select ``/World/A``, ``/World/B``, and ``/World/C``, then write
temperatures 1.0, 2.0, and 3.0 in that order. ``is_array=False`` means each prim
receives one fixed-size value. The stage and its path dictionary already exist.

.. tab-set::

   .. tab-item:: Python

      Select the three prims and resolve the temperature attribute:

      .. filtered-literalinclude:: ../../tests/python/test_minimal.py
         :language: python
         :start-after: # [snippet:dlpack-three-prim-query]
         :end-before: # [/snippet:dlpack-three-prim-query]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

      Write one value for each prim in query order:

      .. filtered-literalinclude:: ../../tests/python/test_minimal.py
         :language: python
         :start-after: # [snippet:dlpack-pass-values]
         :end-before: # [/snippet:dlpack-pass-values]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      Select the three prims and resolve the temperature attribute:

      .. filtered-literalinclude:: ../../tests/c/test_minimal.cpp
         :language: cpp
         :start-after: // [snippet:dlpack-three-prim-query-c]
         :end-before: // [/snippet:dlpack-three-prim-query-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

      Write one value for each prim in query order:

      .. filtered-literalinclude:: ../../tests/c/test_minimal.cpp
         :language: cpp
         :start-after: // [snippet:dlpack-pass-values-c]
         :end-before: // [/snippet:dlpack-pass-values-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Scalar, Vector, and Matrix Layouts
------------------------------------------

Reads and maps return compact scalar-lane tensors (``dtype.lanes == 1``).
For fixed-size attributes, the leading axis counts transported data rows. Each
per-row array tensor instead counts array elements on its leading axis. In both
cases, trailing axes follow the stored component width and semantic.
The original input shape is not stored. Returned tensors have explicit compact
row-major strides in scalar elements, for example ``[16, 4, 1]`` for
``[N, 4, 4]``. Python ``array()`` and ``dlpack()`` preserve these typed shapes.
Write inputs can still use null strides for compact data.

Logical prims select those data rows one-to-one when ``index_map`` is NULL. If
``index_map`` is present, logical prim ``i`` selects row ``index_map[i]`` and
``N`` may differ from ``count``: it can be smaller when rows are shared, or
larger when a query touches only part of a transported bucket.

Copy-in writes may use either the canonical form or compact convenience
dimensions. Convenience dimensions are folded into ``dtype.lanes`` and are not
preserved as schema.

Without ``index_map``, the leading dimension must equal the logical element
count. A flat ``shape = [N * L]``, ``lanes = 1`` tensor is not a convenience
encoding of ``N`` rows of width ``L``; use ``shape = [N, L]``, ``lanes = 1`` or
the canonical ``shape = [N]``, ``lanes = L`` form.

.. list-table::
   :header-rows: 1

   * - Value per data row
     - Canonical write
     - Accepted convenience write
     - Raw read/map / Python DLPack export
   * - Scalar
     - ``shape = [N]``, ``lanes = 1``
     - Same
     - ``[N]``, ``lanes = 1`` / ``(N,)``
   * - ``point3f``
     - ``shape = [N]``, ``lanes = 3``
     - ``shape = [N, 3]``, ``lanes = 1``
     - ``[N, 3]``, ``lanes = 1`` / ``(N, 3)``
   * - ``matrix4d``
     - ``shape = [N]``, ``lanes = 16``
     - ``shape = [N, 4, 4]``, ``lanes = 1`` (or ``[N, 4]`` with 4 lanes)
     - ``[N, 4, 4]``, ``lanes = 1`` / ``(N, 4, 4)``

.. important::

   Map creation dtypes and canonical write inputs encode the complete component
   width in ``dtype.lanes``. Square MATRIX values return two component axes for
   sides 2, 3 or 4; generic tuples retain one axis, including 12-component tuples.
   Use ``array().reshape(-1)`` when flat scalar indexing is needed.

Forwarding typed tensors to writes
----------------------------------

Writes keep their canonical input contract. To forward a descriptor known to
come from an OVStage read or map, use ``ovstage_tensor_to_write()`` from
``ovstage/ovstage_api/ovstage_tensor_utils.h`` or Python
``typed_tensor_to_write()``. These adapters fold compact component dimensions
into lanes, preserve outer row strides, device, pointer and byte offset, and
copy metadata only. They do not infer types for arbitrary scalar arrays.
Preserve the attribute semantic, array kind and prim/data index mapping.

A returned descriptor still borrows native payload. For a rewrite to pinned
read storage, copy the payload before releasing the read group, then adapt
the owned descriptor. Keep all input payload and metadata alive until the
write completes, or through the availability-event synchronization when supplied
(see :ref:`ovstage-placement-synchronization`). A map's borrowed payload stops being valid at accepted
unmap submission. Synchronously rejected unmap leaves the map usable.


.. warning::

   Array attributes use different rules. An ``(N, 3)`` tensor with one lane
   supplies 3N scalar array elements, not N three-component elements. To write
   vector array elements, set ``dtype.lanes = 3`` explicitly. Python's
   :func:`ovstage.make_dltensor()` can describe a compatible compact producer
   buffer this way without copying it. See :doc:`/scene/writing_attributes`.

For non-NumPy DLPack producers, :func:`ovstage.make_dltensor()` combines only
complete trailing dimensions of a compact, byte-aligned buffer. The base type
and bit width must remain unchanged; supported lane counts are 1–255. If all
source dimensions are combined, the result is ``shape=(1,)``, ``ndim=1``.
Explicit shape and stride overrides must match that result: ``shape=[1]``,
``ndim=1``, ``strides=[1]`` in this case. The function's API reference describes
these restrictions and the separate NumPy path.

These snippets write both scalar and three-component values for three prims.
``paths`` is the stage's path dictionary. C uses the ``temperature`` and
``velocity`` tokens. Both representations put the complete
component count in ``dtype.lanes``.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_attribute_shapes.py
         :language: python
         :start-after: # [snippet:attribute-shapes-fixed]
         :end-before: # [/snippet:attribute-shapes-fixed]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_attribute_shapes.cpp
         :language: cpp
         :start-after: // [snippet:attribute-shapes-fixed-c]
         :end-before: // [/snippet:attribute-shapes-fixed-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Buffer Ownership and Lifetime
-------------------------------------

* For C writes, provide exactly one of ``tensors`` or ``managed_tensors``.
  Caller-owned tensor data must remain valid until the operation completes. With
  a placement availability event, retain it through device availability instead;
  see :ref:`ovstage-placement-synchronization`.
  Managed tensors transfer ownership to storage, which calls their deleter when
  no longer needed. See :c:type:`ovstage_write_data_t` for the ownership contract.
* Read results borrow storage. Keep each returned group alive while using its
  tensors or any array views made from them. Releasing the read handle does not
  release those groups.
* Copy values that must outlive their result group. A DLPack consumer array does
  not keep the owning group alive for you. Outstanding groups can block
  overlapping writes.

The :term:`watermark <Watermark>` controls which changes may be read; it does not extend the
lifetime of returned buffers. See :doc:`/scene/reading_attributes` for cleanup.

Copy values before releasing their group if you need them afterwards. Here
``group`` is an already-fetched result containing three float32 scalars; the C
code has checked its layout and allocated ``float out[3]``. Both copies survive
release of the group. Release the read handle separately when finished fetching.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_minimal.py
         :language: python
         :start-after: # [snippet:dlpack-copy-result]
         :end-before: # [/snippet:dlpack-copy-result]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_minimal.cpp
         :language: cpp
         :start-after: // [snippet:dlpack-copy-result-c]
         :end-before: // [/snippet:dlpack-copy-result-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

.. _ovstage-device-placement:

Experimental Device Placement
-----------------------------

Pass a :class:`ovstage.Placement` as ``placement=`` to
:meth:`Stage.read_attributes() <ovstage.Stage.read_attributes>`,
:meth:`Stage.write_attribute() <ovstage.Stage.write_attribute>`,
:meth:`Stage.write_attributes() <ovstage.Stage.write_attributes>`, or
:meth:`Stage.map_attribute() <ovstage.Stage.map_attribute>`.
In C, include ``<ovstage/ovstage_api/experimental/ovstage_placement_extension.h>`` and
use the corresponding ``ovstage_*_on_device`` wrapper. The extension is
experimental: its API, ABI, and semantics may change. An unavailable or
incompatible extension reports ``NOT_FOUND``; ordinary calls remain available.

``device_type`` selects CPU (``kDLCPU``) or CUDA (``kDLCUDA``), and ``device_id``
selects the CUDA device. One placement applies to the whole operation.
Omitting placement preserves existing behavior: writes follow the source
tensor's device, and reads/maps return CPU buffers. A zero device type within
a placement leaves device selection unspecified with those same defaults.

Placement selects a device's copy of an attribute; it does not pin that attribute
to one device permanently. Writes and map commits produce values on the selected
device. Reads use the copy already current there, or materialize one if needed.
A read of an already-current copy avoids migration. Inspect each returned
``DLTensor.device`` and use a consumer for that device. ``group.array()`` is
CPU-only; CUDA buffers require a CUDA-capable DLPack consumer. Keep each group
alive through its consumer's device work before releasing it or committing a map.

For a complete producer-to-consumer sequence in C and Python, see
:doc:`/scene/device_placement`.

.. _ovstage-placement-synchronization:

Completion, Availability, and Buffer Lifetime
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Placement's ``stream`` carries destination work; a write's ``cuda_stream`` and
``cuda_event`` describe the independent source handoff. ``availability_event``
is a caller-owned CUDA event that ovstage records for destination availability.
Without it, device work is drained before the operation reports completion.
With it, an operation can complete while its device work is still running:

* **Write or map commit:** enqueue, wait for the operation, and check its status;
  then wait on the availability event or submit downstream work on the placement
  stream. Keep write source buffers alive and unchanged until device work finishes.
* **Read:** fetch a group successfully; only then wait on the availability event
  or submit its consumer on the placement stream before dereferencing the payload.
  The event covers prior writes as well as any transfer needed by the read.
* **Map:** the event covers each commit, not the staging buffers returned by
  setup. Finish producing mapped values and provide the source handoff needed by
  a CUDA commit; stop accessing buffers when unmap submission is accepted.

Waiting on the event or stream before these boundaries can observe an old or
unrecorded event. Stream ``2`` denotes the worker thread's per-thread default
stream, which may differ from the caller's; use the event to order the caller's
work in that case. Stream/event handles remain caller-owned, must be valid for
the recording device, and must survive their use. The placement descriptor
itself is copied during enqueue.

Python :meth:`Operation.wait() <ovstage.Operation.wait>` releases its references
to write sources. When using an availability event, keep your own source
references until that event has completed. Also wait on outstanding write
events before an attached renderer performs its own hierarchy propagation.

An event can be recorded for a failing operation: its completion does not prove
success. If CUDA refuses to record it, the event remains untouched and a write
or map commit may report ``OP_FAILED`` after committing its effects. Do not wait
on that event as proof of availability. After observing the operation failure,
synchronize the specific placement stream, or the recording device when using
a default/per-thread stream, before releasing write sources. See
:ref:`ovstage-placement-failures` and :class:`ovstage.Placement` for the full rules.

Current Placement Limits
^^^^^^^^^^^^^^^^^^^^^^^^

Placement does not guarantee asynchronous transfers in every case. A write
staged between devices drains its staging buffer before returning completion.
Ragged writes use the default stream rather than the requested placement stream;
their availability event is already signalled when the operation completes.
CUDA maps support fixed-size attributes only; array-valued CUDA maps return
``NOT_SUPPORTED``. Map commit scatter uses the mapped device's default stream
and is drained before operation completion. An availability event does not make
that scatter asynchronous with respect to completion, although submission still
enqueues the operation.
Staged host rows with negative or zero/broadcast strides are not supported.
Hierarchy outputs follow the selected hierarchy computation model; placement
does not change that model.

CPU/CUDA Data and Synchronization
-----------------------------------------

Writes accept CPU or CUDA source tensors. Reads and maps return CPU buffers
unless a device is selected through :ref:`ovstage-device-placement`. A DLPack
view avoids a copy of the returned buffer; transfers between devices can still
be necessary.

For CUDA input, :meth:`ovstage.Stage.write_attribute()` accepts ``cuda_stream``
and ``cuda_event`` to synchronize before consuming the data. You can use both:

.. list-table::
   :header-rows: 1

   * - Parameter
     - Meaning
   * - ``cuda_stream``
     - ``None`` or ``0``: no stream synchronization. ``1``: CUDA default stream.
       Otherwise pass the integer CUDA stream handle. Synchronization waits for
       queued work on that stream to finish before consuming the input.
   * - ``cuda_event``
     - ``None`` or ``0``: no event. Otherwise pass the integer CUDA event handle
       to wait for.

Leave both unset for CPU data or CUDA data whose producer has already completed.
For writes with several tensors, do not mix CPU and CUDA tensors or different
CUDA devices. In C, these parameters correspond to ``cuda_sync.stream`` and
``cuda_sync.wait_event`` in :c:type:`ovstage_cuda_sync_t`; use ``{0, 0}`` for no
additional synchronization.

These examples finish producing three CUDA values before submitting the write,
so no additional synchronization is needed. The GPU buffer remains alive until the
write completes. Python uses Warp as ``wp``; C uses the CUDA runtime and a
``checkCuda()`` helper that reports CUDA errors. The query selects three prims,
and ``gpu_samples`` / ``attribute`` names the destination attribute. See the
:doc:`CUDA tensor-write example </examples/cuda_tensor_writes>` for setup.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :start-after: # [snippet:dlpack-cuda-ingest]
         :end-before: # [/snippet:dlpack-cuda-ingest]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/cuda_ingest.cpp
         :language: cpp
         :start-after: // [snippet:dlpack-cuda-ingest-c]
         :end-before: // [/snippet:dlpack-cuda-ingest-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Selecting Values and Prims
----------------------------------

By default, a write assigns the first input value to the first
:term:`prim <Prim>` in the query, the second value to the second prim, and so on.

Use ``index_map`` to change which input value each prim receives. For example,
``[1, 0]`` gives the second input value to the first prim and the first value
to the second prim.

Use ``mask`` to update only some prims in the query. It contains one bit per
prim addressed by the write: a set bit writes that prim; an unset bit leaves
its existing value unchanged. The input still needs a value for every prim
addressed by the write, including those skipped by the mask.

In C, supply an array of ``uint64_t``; in Python, supply a list of integers.
Each integer masks 64 prims. The least significant bit of the first integer
controls the first prim, the next bit controls the second, and so on. The second
integer starts at the 65th prim. For four prims ``[A, B, C, D]``,
``mask=[0b1001]`` writes only A and D.

A write can use ``index_map`` or ``mask``, but not both. ``count`` specifies how many prims the
write addresses, starting at the beginning of the query. In C, set it explicitly
when using either option. Python can infer it from the length of ``index_map``;
with ``mask``, set it explicitly. Supply at least ``(count + 63) // 64`` mask
integers. Bits beyond ``count`` are ignored.

See :doc:`/scene/writing_attributes` for index bounds and packed array rules.
Packed data must divide evenly across the inferred row count,
with a whole number of ``dtype`` elements per row; otherwise the write is
rejected. See :meth:`ovstage.Stage.write_attribute()` for the Python parameters
and :c:type:`ovstage_write_data_t` for the C descriptor.

The query below selects A, B, C, and D, initially holding 10, 20, 30, and 40.
The first write uses ``index_map`` to assign 222, 333, and 111 to A, B, and C.
The second uses ``mask`` to change only D to 44. In C, ``tensor(values, shape)``
builds a compact CPU float32 tensor; ``three`` and ``four`` are shapes ``{3}``
and ``{4}``. ``readFour()`` copies the four current values into the verification buffer
``float values[4]``.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_sparse_writes.py
         :language: python
         :start-after: # [snippet:sparse-index-map-and-mask]
         :end-before: # [/snippet:sparse-index-map-and-mask]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_sparse_writes.cpp
         :language: cpp
         :start-after: // [snippet:sparse-index-map-and-mask-c]
         :end-before: // [/snippet:sparse-index-map-and-mask-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Writing through Mapped Buffers
--------------------------------------

Mapping without placement gives you writable CPU **staging buffers**.
The experimental :ref:`ovstage-device-placement` also supports CUDA map buffers
for fixed-size attributes. They need not share memory
with the final attribute storage; unmap may copy or scatter the values.
The call order is:

1. Call :meth:`ovstage.Stage.map_attribute()` with an :term:`ordinal <Ordinal>` and
   attribute. Creating an attribute requires its ``dtype``.
2. Iterate :meth:`ovstage.Map.groups()` and write each buffer's values.
3. Call :meth:`ovstage.Map.unmap_group()` for each group, then
   :meth:`ovstage.Map.unmap()` to finish remaining writes and release the map.
   A ``with`` block calls the final unmap for you. Leave ``cuda_event`` and
   ``cuda_stream`` unset for these CPU buffers.
4. Call :meth:`ovstage.Stage.advance_write_floor()` and wait before reading
   the changes.

Ready means that staging buffers are available. It does not create visible
prims or attributes, change their schemas or parents, or record changes or
ordinal ownership. Each successful unmap commits its group's values and
structural changes. Final unmap commits only groups that remain, then closes
the session; earlier successful commits remain if a later group fails.

Unmap rechecks the current schema, prim-creation policy, ordinal admission, and
read overlap. A conflicting dtype or authored semantic requires deleting the
attribute first and mapping at a later ordinal, or setting
``attribute_mode=AttributeMode.FORCE`` with an explicit dtype to replace the
schema when each group commits. An omitted semantic adopts an unambiguous compatible existing
one; specify dtype and semantic when targets have multiple same-name identities.
Changes to unrelated prims do not by themselves invalidate the map.
Finish unmapping before advancing the watermark to or beyond the map's ordinal.

In Python, groups fetched through a ``Map`` retain that map. Keep the group or
its DLPack producer alive while using a view. Stop accessing the group and all
previously obtained views as soon as its group commit or final unmap submission
is accepted. A synchronously rejected submission leaves it usable. Views already
returned cannot check whether they are still valid.

.. note::

   Prefer :meth:`mapping.fetch_next() <ovstage.Map.fetch_next>`, :meth:`mapping.groups() <ovstage.Map.groups>`, and :meth:`mapping.unmap() <ovstage.Map.unmap>`
   on the Python ``Map`` object. The lower-level Stage methods also accept the
   map's integer ID: :meth:`stage.fetch_map_next(mapping.handle) <ovstage.Stage.fetch_map_next>` returns a group
   that does not retain the ``Map``, and
   :meth:`stage.unmap_attribute(mapping.handle) <ovstage.Stage.unmap_attribute>` does not mark the Python ``Map``
   or its groups as unusable. Passing the ``Map`` object instead of its integer
   ID preserves that lifetime tracking.

A successful transform unmap makes derived transforms need updating; use the
computation sequence in :doc:`/scene/transforms`. A pending or failed map does
not supply a committed parent. Children reconnect when that parent is later
committed successfully. For failed-map cleanup, see :doc:`error_handling`.

The snippets below map an existing scalar attribute on two prims, write 10 and
20, and finish the map before advancing the watermark to 2. Mapped buffers are
write-only and uninitialized: fill every selected element. The index calculations
associate each buffer row with its prim. Python's ``with`` block performs the
final unmap; C calls it explicitly.

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../tests/python/test_map_attribute.py
         :language: python
         :start-after: # [snippet:dlpack-map-existing]
         :end-before: # [/snippet:dlpack-map-existing]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../tests/c/test_map_attribute.cpp
         :language: cpp
         :start-after: // [snippet:dlpack-map-existing-c]
         :end-before: // [/snippet:dlpack-map-existing-c]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:
