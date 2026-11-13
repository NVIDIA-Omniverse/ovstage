.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Threading, Blocking, and Lifetime
=================================

This page explains how to share a stage across threads, wait for results, and clean up application resources. See :doc:`/concepts/async_model` for operation ordering and :doc:`/concepts/limitations` for release-specific limits.

Handle lifetime
---------------

Copying a handle does not retain its resource or extend its lifetime. Its type
and a nonzero value do not prove that the resource is alive or belongs to a
particular stage. Coordinate resource use and release with the owning stage.
See :ref:`c-api-handle-types` and :ref:`python-handle-types` for representation,
comparison, and invalid-value rules.

Thread Safety
-------------

The C API permits concurrent calls on the same live instance or on different live instances in the same process, including GPU hierarchy computation. Coordinate each object's lifetime: stop other threads from using a handle before releasing or destroying it. Pass a valid instance to every call that requires one.

Concurrent GPU hierarchy computations are safe but are not guaranteed to execute in parallel. Callers remain responsible for GPU synchronization between producers and consumers.

Copy error information immediately on the thread that made the call. The text from :c:func:`ovstage_get_last_error()`, the strings from ``ovstage_get_last_op_error()``, and the ``error_op_ids`` list from :c:func:`ovstage_wait_op()` are valid only for the documented lifetime. A later call that waits on that thread can invalidate them.

Thread safety does not imply ordering between operations submitted by different threads. Sequence dependent calls explicitly; see :doc:`/concepts/async_model`.

Waiting and Responsiveness
--------------------------

Successful submission means an operation was accepted, not that it completed. Check the submission status, then use the corresponding wait or fetch call before consuming results or starting dependent work. Do not assume every submission returns immediately: writes, deletes, and maps can wait for their input query to become ready.

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Call or operation
     - Caller guidance
   * - :c:func:`ovstage_wait_op()` and query/read/map/ordinal fetches
     - A timeout of 0 polls. A finite timeout limits the wait. ``OVSTAGE_TIMEOUT_INFINITE`` waits until completion. A timeout does not cancel the operation or permit early reuse of its input buffers.
   * - :c:func:`ovstage_fetch_hierarchy_result()`
     - Returns ``TIMEOUT`` until the result is ready.
   * - Query, read, hierarchy, and ordinal-query release
     - Complete required work before releasing its handles. Release calls may wait; do not use them as a substitute for observing successful operation completion.
   * - :c:func:`ovstage_release_groups()` and :c:func:`ovstage_release_op()`
     - Finish using the result before releasing it. Release operation tracking after reading completion and error information.
   * - :c:func:`ovstage_population_wait_op()`
     - Use this population-specific wait for population operations; a data-plane wait does not replace it.
   * - :c:func:`ovstage_flush_log()`
     - Waits, subject to its timeout, for earlier messages to reach the callback. Use timeout 0 to poll.
   * - :c:func:`ovstage_destroy_instance()`
     - Complete required work and release its resources first. Destruction is not a substitute for a successful wait.

Use finite waits where the calling thread must stay responsive. Avoid waiting while holding an application lock needed by another operation or a callback. Do not rely on a callback running on the thread that submitted the operation.

When using an attached ovrtx renderer, complete the stage updates and write-floor advance required by a frame before rendering. See :doc:`/concepts/async_model` for the supported sequence.

Python
------

A :class:`ovstage.Stage` can be shared across threads to issue calls, but give each query, read, map, operation, and path-list object a single owner thread. Coordinate with that owner before using or releasing the object.

* Use finite wait timeouts in a loop when the application must handle interruption. An infinite native wait can delay Python's handling of Ctrl-C until the call returns.
* Keep log callbacks brief. Do not call :func:`ovstage.set_log_callback` or :func:`ovstage.flush_log`, or create or destroy a stage, from a log callback. Avoid holding a lock across a wait if the callback needs that lock.
* If all stages have been destroyed and every successful explicit initialization has been matched by shutdown, register the log callback again after creating a new stage. To preserve logging between stage lifetimes, initialize explicitly and call the matching shutdown only when finished.
* Read :meth:`~ovstage.Operation.error_message` immediately after failure, on the same thread.
* Use ``with`` blocks for handles and results where available, and finish using array or DLPack views before leaving the owning object's scope.

Process Initialization
----------------------

Call :c:func:`ovstage_initialize()` before creating stages when supplying process configuration. Pair every successful initialize call with one :c:func:`ovstage_shutdown()`. Destroying a stage does not replace that matching shutdown, and shutdown does not destroy the application's stages. A shutdown without a matching initialize returns ``INVALID_ARGUMENT``.

When linking the shared library directly, explicit initialization is optional if the application needs no process configuration. Static-loader applications must supply the required package location; see :doc:`/guides/project_setup_c`.

.. _threading-destroying-stage:

Destroying a Stage
------------------

Wait for required operations to complete and inspect their results. Release every application-owned operation, query, read, read group, map, and path list, and finish using tensor views before final stage cleanup. Do not depend on destruction to complete queued writes successfully.

Stop other threads from using the stage before calling :c:func:`ovstage_destroy_instance()` or :meth:`ovstage.Stage.destroy`. The destroyed handle is invalid. Do not call through it, read through its views, or use it to wait for further work.

Destroying the application's stage handle does not detach an already attached ovrtx renderer. End that attachment through the renderer's public detach or destruction API. For a straightforward application cleanup sequence, finish the required rendering and stage work, detach or destroy the renderer, release stage resources, then destroy the stage.

Input and Result Buffers
------------------------

* Keep input payloads valid and unchanged until the operation using them completes. Follow the public descriptor rules for the lifetime of shape, stride, index-map, and mask arrays.
* Use read-group tensors only while their group is valid. :c:func:`ovstage_release_read()` does not release groups already fetched from that read; release each group with :c:func:`ovstage_release_groups()`. Copy data that must remain available afterward.
* A path list returned in a result is valid for that result's documented lifetime. To use the list longer, call ``path_dictionary_add_path_list_reference`` before releasing the result and later pair it with ``path_dictionary_release_path_list_reference``. Release all such lists before destroying their stage. Keep an explicitly supplied path list valid while using a query created from it.

Read and Write Overlap
----------------------

While a read group is held, a write, map, or delete that overlaps its attributes and prims fails with ``OP_FAILED``, as does any operation, including a clone, that would give ``omni:fabric:worldMatrix`` to a prim the group covers: a hierarchy parent or ancestor of the operation's prims, or a prim with a transform input between one of them and a descendant that carries it (see :doc:`/scene/transforms`). A read overlapping an unfinished write also fails. Release read groups before modifying their data, and wait for writes before reading. Copy results and release their groups promptly when processing will take longer.
