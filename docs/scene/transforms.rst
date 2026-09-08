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

ovstage represents an authored transform with two attributes:

- ``omni:xform`` is always the prim's **local** transform.
- ``omni:resetXformStack`` controls whether the parent's world transform
  participates in computing the prim's world transform.

Local and Effective-World Authoring
-----------------------------------

Use these combinations when writing a transform:

.. list-table::
   :header-rows: 1
   :widths: 28 26 46

   * - Intent
     - ``omni:resetXformStack``
     - Result
   * - Author a local transform
     - ``false``
     - ``omni:xform`` composes with the parent's world transform.
   * - Author an effective world transform
     - ``true``
     - Parent composition stops at this prim, so its effective world transform
       is exactly ``omni:xform``.

A reset prim becomes a new inheritance root; its descendants continue to
inherit from it normally. The matrix itself remains local data.

The reset value persists until another write or an xform-related USD
synchronization replaces it. While it remains ``true``, later transform
updates need only write ``omni:xform``. When changing both attributes, write
them at the same ordinal and make both writes complete before sealing that
ordinal or computing hierarchy-derived data. The C API's
``ovstage_write_attributes`` can group the writes in one operation; the
grouping is not an atomic transaction.

Transform Data Layout
---------------------

``omni:xform`` is a fixed-size, non-array attribute with:

- DLPack dtype ``{ kDLFloat, 64, 16 }``;
- ``OVSTAGE_SEMANTIC_MATRIX``;
- one row-major 4x4 matrix per queried prim.

ovstage follows the USD row-vector matrix convention. Translation occupies
matrix elements 12, 13, and 14 (the first three elements of the last row).

``omni:resetXformStack`` is a fixed-size, non-array scalar with DLPack dtype
``{ kDLBool, 8, 1 }``. Write a real boolean column; an integer column is not the
public data contract even when it has the same byte width.

Deriving World Transforms
-------------------------

A consumer that needs hierarchy-derived world transforms must run
``ovstage_compute_hierarchy`` using a model advertised by
``ovstage_get_hierarchy_computation_models``.

In the current release, CPU incremental hierarchy computation supports
USD-populated transform hierarchies. A hierarchy created entirely through
client writes requires a supported GPU computation model and pre-existing
derived-output rows. Refer to the ``authoring-hierarchy`` pair in
:doc:`/examples/index` for that workflow.

Use explicit computation as a batching and freshness boundary:

#. Write ``omni:xform`` and, when needed, ``omni:resetXformStack`` at ordinal
   ``N``.
#. Wait for the write operations with ``ovstage_wait_op``, then advance the
   write floor to ``N`` and wait for that operation.
#. Enqueue ``ovstage_compute_hierarchy`` with ``input_ordinal = N`` and
   ``output_ordinal = N``, then wait for that operation before consuming
   derived results.

This backend is latest-snapshot only: the hierarchy call's input and output
ordinals do not provide historical transform snapshots. Do not overlap writes
that belong to a later update with the computation. Reads and attached
consumers can also trigger runtime-default propagation, so do not use a read
itself as a staleness probe; call ``ovstage_compute_hierarchy`` when the
application must own the freshness boundary.

How This Differs from Editing USD
---------------------------------

The inheritance semantics match USD's reset-xform-stack behavior, but the
representation and data ownership differ:

- USD encodes a reset in ``xformOpOrder`` as ``!resetXformStack!``.
- ovstage exposes the mirrored runtime state as an independently writable
  ``omni:resetXformStack`` runtime attribute.

When the ``PHYSICS`` domain or a population description's selectors put a
USD-xformable prim in scope, population initializes ``omni:xform`` and
``omni:resetXformStack`` from USD. An application can then change either value
in ovstage without editing the USD source. For example, setting the runtime
reset solely to publish world-space simulation poses can make ovstage's reset
value differ from the reset authored in USD. Do not assume the runtime value
still describes the source layer after such a write.

Derived World-Matrix Output
---------------------------

The current population and hierarchy path may materialize a derived world
matrix under ``omni:fabric:worldMatrix``. Treat this as **read-only output**:
do not use it as the transform-authoring protocol. Its name is provisional and
may change in a future release.

Hierarchy computation updates existing world-matrix rows; it does not create
the column. Population without the ``RENDERING`` domain can seed it with the
local transform before the first compute; with ``RENDERING`` selected, that
domain owns the column instead. A client-created stage can have no such rows at
all. Therefore, absence is valid and a pre-compute value is not proof that the
world transform is current.

Although the generic attribute API can address this column in current builds,
a direct write can be interpreted as world-authoring input and back-converted
into local state. That behavior is outside the stable transform-authoring
contract. It can also alter descendant results, so never use the column as a
world-space shortcut. USD export recognizes ``omni:xform`` rather than this
derived output; see :doc:`exporting_to_usd`.

Where to Go Next
----------------

- :doc:`writing_attributes` — tensor shapes, semantics, ordinals, and sealing.
- :doc:`population` — when population publishes transforms and reset state from USD.
- :doc:`/guides/runtime_loop` — a complete populate and update loop.
