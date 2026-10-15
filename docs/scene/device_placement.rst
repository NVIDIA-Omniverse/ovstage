.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Device Placement (Experimental)
===============================

Use the ``ovstage.placement`` extension when a GPU producer and consumer need
to exchange attribute data through ovstage on a chosen device. For example,
a simulation can write CUDA tensors, and a downstream GPU kernel can read
those values on the same GPU without bringing the attribute payload to the CPU.
See :doc:`/concepts/dlpack_tensors` for tensor layout and :doc:`reading_attributes`
for queries, read groups, and the write floor that seals data for reading.

.. warning::

   Device placement is **experimental**. The functionality is likely to remain,
   but the API is likely to change as it matures. Source compatibility, ABI,
   and semantics are not guaranteed between releases. Isolate its use behind
   an application adapter and check availability at runtime. An incompatible
   extension version is reported as unavailable; do not cast an unknown table
   to the version your application expects.

Access the Extension
--------------------

The C entry points live in an explicitly included experimental header, rather
than in the core ``ovstage_vtable_t``. The typed lookup below uses the instance's
``query_extension`` mechanism and checks the extension version and table size.
The convenience wrappers perform the same lookup before dispatching the call.

.. code-block:: c

   #include <ovstage/ovstage_api/experimental/ovstage_placement_extension.h>

   const ovstage_placement_extension_v1_t* placement_api = NULL;
   ovstage_api_status_t status =
       ovstage_query_placement_extension(stage, &placement_api);
   if (status == OVSTAGE_ERROR_NOT_FOUND) {
       /* Disable this optional workflow, or select a CPU fallback. */
       return;
   }
   /* Check status before using placement_api or the wrappers below. */

Use ``ovstage_write_attribute_on_device``, ``ovstage_write_attributes_on_device``,
``ovstage_read_attributes_on_device``, and ``ovstage_map_attribute_on_device``
with an ``ovstage_placement_t`` initialized by ``OVSTAGE_PLACEMENT_INIT``.
No additional library is needed beyond ovstage and the CUDA facilities used by
the application. Passing ``NULL`` placement preserves the original API behavior.

Python exposes the extension through ``ovstage.Placement`` and the
``placement=`` keyword on ``Stage.write_attribute``, ``write_attributes``,
``read_attributes``, and ``map_attribute``. The bindings perform extension
lookup; an unavailable extension raises ``OvstageError`` with
``ErrorCode.NOT_FOUND``. Older Python packages may not export ``Placement`` at
all, so applications supporting them should check that separately.

.. code-block:: python

   import ovstage

   # A CUDA device is DLPack device type 2. This also accepts an object
   # implementing __dlpack_device__(), such as a CUDA array.
   # gpu_values has already finished producing its data on device_id.
   placement = ovstage.Placement.from_dlpack_device((2, device_id))
   operation = stage.write_attribute(
       query, attribute, ordinal, gpu_values, is_array=False, placement=placement
   )
   operation.wait()

This simplest form selects residency and waits for the write's GPU work to
finish. Add an availability event for the asynchronous workflow below.

Three Independent Choices
-------------------------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Setting
     - Meaning
   * - Input ``cuda_sync`` (Python ``cuda_event`` / ``cuda_stream``)
     - The producer dependency: ovstage waits before reading the source tensor.
   * - Placement ``device`` and ``stream``
     - The target device and stream for supported device work. A read selects
       the device on which the returned payload is available.
   * - Placement ``availability_event``
     - The destination handoff: ovstage records this caller-owned CUDA event
       after submitting the work that makes the data available.

Creating an event, recording it, and waiting for it are separate actions.
The application creates and owns the streams and events. ovstage records the
availability event; the consumer then waits for that record. Use distinct
producer, write-availability, and read-availability events in a pipeline.

An operation's CPU completion and its GPU availability are different boundaries:

.. code-block:: text

   Producer stream: produce values -> record producer event
   OVStage write:  wait producer event -> scatter on placement stream -> record write event
   CPU:            observe write submission -> seal ordinal -> request read -> fetch group
   OVStage read:   order pending writes / required transfers -> record read event
   Consumer stream: wait read event -> consume returned GPU tensors

For a write, call ``ovstage_wait_op`` (Python ``operation.wait()``) and check
execution errors **before** waiting on its availability event or submitting
dependent work to the placement stream. For a read, successfully fetch a group
before waiting on its availability event. Waiting earlier can observe an old
or never-recorded event and establish no dependency on the requested operation.
An event alone is not an error check: an accepted operation can record it even
when execution fails. See :doc:`/concepts/error_handling`.

Write and Read on the Same GPU
------------------------------

These excerpts assume an existing stage, a query selecting three prims, a
float32 scalar attribute, and a fresh writable ``ordinal``. All streams and
events belong to ``device_id``. The source remains allocated until the write
has finished consuming it. No overlapping edits are made while the read is live.

.. tab-set::

   .. tab-item:: Python

      The Python example uses CuPy. Install a CuPy distribution compatible with your
      CUDA environment alongside ovstage. The consumer computes a sum on the GPU.

      .. code-block:: python

         import cupy as cp
         import ovstage

         with cp.cuda.Device(device_id):
             producer = cp.cuda.Stream(non_blocking=True)
             writer = cp.cuda.Stream(non_blocking=True)
             reader = cp.cuda.Stream(non_blocking=True)
             consumer = cp.cuda.Stream(non_blocking=True)
             produced = cp.cuda.Event(disable_timing=True)
             written = cp.cuda.Event(disable_timing=True)
             readable = cp.cuda.Event(disable_timing=True)

             with producer:
                 values = cp.arange(3, dtype=cp.float32)
                 produced.record(producer)

             try:
                 stage.write_attribute(
                     query, attribute, ordinal, values, is_array=False,
                     cuda_event=produced.ptr,
                     placement=ovstage.Placement.from_dlpack_device(
                         values, stream=writer.ptr, availability_event=written.ptr
                     ),
                 ).wait()  # Submission/status boundary; GPU work may still be pending.
                 stage.advance_write_floor(ordinal=ordinal).wait()

                 with stage.read_attributes(
                     query, [attribute], ovstage.OrdinalRange.latest(ordinal),
                     placement=ovstage.Placement.from_dlpack_device(
                         values, stream=reader.ptr, availability_event=readable.ptr
                     ),
                 ) as read:
                     read.wait()
                     for group in read.groups():
                         with group:
                             try:
                                 consumer.wait_event(readable)  # Fetch has issued the record.
                                 with consumer:
                                     view = cp.from_dlpack(group.dlpack(0))
                                     result = cp.sum(view)
                             finally:
                                 # Complete all uses before releasing this borrowed group.
                                 reader.synchronize()
                                 consumer.synchronize()
             finally:
                 # Also protect values/events on error, including a failed event record.
                 producer.synchronize()
                 writer.synchronize()
                 reader.synchronize()
                 consumer.synchronize()

   .. tab-item:: C / C++

      The C API uses the same sequence. Here ``data`` is an
      ``ovstage_write_data_t`` containing one CUDA ``DLTensor`` with three float32
      rows. ``producer_ready`` has already been recorded after producing those rows.
      ``write_ready`` and ``read_ready`` are caller-created events; ``writer``,
      ``reader``, and ``consumer`` are explicit CUDA streams. ``waitOp`` checks the
      enqueue status, ``ovstage_wait_op`` status and reported operation errors, then
      releases the operation ID. ``checkStage`` and ``checkCuda`` stop on errors.
      ``enqueueConsumer`` queues application GPU work using the group's tensor layout
      and row metadata. These helpers represent application error handling and kernels.

      .. code-block:: cpp

         ovstage_placement_t writePlacement = OVSTAGE_PLACEMENT_INIT;
         writePlacement.device = { kDLCUDA, device_id };
         writePlacement.stream = reinterpret_cast<uintptr_t>(writer);
         writePlacement.availability_event = reinterpret_cast<uintptr_t>(write_ready);
         data.cuda_sync.wait_event = reinterpret_cast<uintptr_t>(producer_ready);
         data.cuda_sync.stream = 0;
         waitOp(stage, ovstage_write_attribute_on_device(
             stage, query, { attribute, {} }, ordinal, data,
             OVSTAGE_PRIM_MODE_UPSERT, &writePlacement));

         ovstage_write_floor_desc_t floor{};
         floor.ordinal = ordinal;
         floor.scope = OVSTAGE_SCOPE_ALL;
         waitOp(stage, ovstage_advance_write_floor(stage, &floor));

         ovstage_placement_t readPlacement = OVSTAGE_PLACEMENT_INIT;
         readPlacement.device = { kDLCUDA, device_id };
         readPlacement.stream = reinterpret_cast<uintptr_t>(reader);
         readPlacement.availability_event = reinterpret_cast<uintptr_t>(read_ready);
         ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
         waitOp(stage, ovstage_read_attributes_on_device(
             stage, query, &attribute, 1, { 0, ordinal, false }, &readPlacement, &read));
         for (;;) {
             ovstage_read_group_t group{};
             const auto status = ovstage_fetch_read_next(
                 stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
             if (status == OVSTAGE_ERROR_END_OF_ITERATION)
                 break;
             checkStage(stage, status);
             checkCuda(cudaStreamWaitEvent(consumer, read_ready, 0));
             enqueueConsumer(consumer, group);
             checkCuda(cudaStreamSynchronize(consumer));
             checkStage(stage, ovstage_release_group(stage, &group));
         }
         waitOp(stage, ovstage_release_read(stage, read));
         checkCuda(cudaEventSynchronize(write_ready));
         // The source allocation can now be reused or freed.

On success, ``cudaEventSynchronize(write_ready)`` after the write's submission
boundary is also how a C caller explicitly waits on the host for the write's
GPU work. Python can call ``written.synchronize()`` after ``operation.wait()``.
If recording the event fails, it cannot be used to prove completion: check the
operation error and drain the explicit placement stream before reclaiming input
storage. The write may already have applied and consumed its ordinal when event
recording fails; do not retry it at the same ordinal. Applications using
exceptions in the C++ excerpt must provide cleanup
that drains outstanding work before releasing groups, buffers, streams or events.

The examples wait on the host at resource-retirement boundaries. Between
submission and retirement, the CPU can do independent work and enqueue GPU work
that respects the events. A pipeline can defer retirement by retaining each
group and source buffer until a consumer-completion event has finished. Do not
release a group merely because its GPU consumer has been enqueued. A DLPack
view does not extend storage validity past explicit group release.

Maps and Current Limits
-----------------------

``ovstage_map_attribute_on_device`` / ``Stage.map_attribute(placement=...)``
select the device for writable staging buffers. After fetching a map group,
fill its buffers on that device, supply the producer handoff to the unmap/commit
call, and observe that commit's CPU completion before waiting on the placement
availability event. The event covers committed values, not initial map-buffer
readiness. Keep producer work ordered before commit and stop accessing the
mapped pointers when commit is accepted; mapping does not expose an unrestricted
pointer to persistent attribute storage.

- A read on a device where the attribute is already current needs no migration.
  A read requesting another device can copy. Placement is not a promise that
  all operations are copy-free; writes can still scatter into stage storage.
- Same-device fixed-size writes can leave GPU work pending when an availability
  event is supplied. Without it, operation completion drains the write's device
  work. Writes requiring staging between devices currently drain that work even
  with an event. Ragged writes ignore the placement stream and complete their
  default-stream work before reporting completion.
- Cross-device reads can block the host during materialization. First-use CUDA
  initialization and structural changes can also synchronize. Events express
  dependencies; they are not a guarantee that every API call returns without
  a host wait.
- Placement applies to the whole operation. Split work requiring different
  devices into separate operations and use streams/events for the correct device.
- Ordinal admission, write-floor sealing, error checking and overlapping-view
  rules still apply. Original calls, or calls without placement, retain their
  existing behavior.

Further Reading
---------------

- :doc:`/concepts/async_model` — submission, operation errors and ordering.
- :doc:`/concepts/dlpack_tensors` — tensor layout and borrowed views.
- :doc:`/examples/cuda_tensor_writes` — the simpler CUDA-input/CPU-readback workflow.
