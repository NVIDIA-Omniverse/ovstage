.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Writing Attributes
==================

:meth:`ovstage.Stage.write_attribute()` writes :term:`attribute <Attribute>`
values for the :term:`prims <Prim>` selected by a :doc:`query <queries>`.
Supply an :term:`ordinal <Ordinal>` above the attribute's
:term:`watermark <Watermark>`. The call returns an :class:`ovstage.Operation`;
wait for successful completion and advance the watermark before reading.

Under ``STRICTLY_INCREASING``, every affected (prim path, canonical attribute) cell must use an ordinal newer
than its retained committed changes, as well as clearing the applicable floors.
This applies across writes, map commits, clones and deletes. Disjoint cells may
share an ordinal. A clone followed by customization of a copied cell therefore
needs a newer ordinal for the customization, even if neither operation is sealed.

WRITE and MAP enqueue check floors without deciding retained value conflicts.
Observe execution through ``wait_op`` / ``wait()``: WRITE admits the whole batch
before its first mutation; MAP checks before exposing writable groups and again
when publishing them.

Writes are not transactional. Completed entries survive a later failure.
Repair a batch that may have partially committed at a newer ordinal. A
same-ordinal retry remains subject to current watermarks, admission policy,
and stage state. See :doc:`/concepts/error_handling` for partial changes,
reserved metadata, and INSERT/UPSERT recovery.

.. _ordinal-admission-policies:

Ordinal Admission Policies
--------------------------

Process configuration selects the policy for all new instances. For a mutation
at ordinal ``n`` intersecting a retained change at ordinal ``h``:

.. list-table::
   :header-rows: 1

   * - Policy
     - Retained-history rejection
   * - ``FLOOR_ONLY`` (backward compatibility)
     - None
   * - ``NONDECREASING`` (default)
     - ``n < h``
   * - ``STRICTLY_INCREASING`` (experimental)
     - ``n <= h``

.. note::

   ``NONDECREASING`` remains the default. ``STRICTLY_INCREASING`` is experimental
   and represents the intended direction for a future default; ``FLOOR_ONLY``
   is retained for backward compatibility.

All modes require the ordinal to be above the operation's write floors and
preserve change tracking, type checks and INSERT requirements. Both history
policies also check applicable prim-lifecycle history and the ``usd-path``
lifecycle floor; ``FLOOR_ONLY`` skips that additional lifecycle check.
Canonical aliases share history, and disjoint cells can share an ordinal.
Allowing equal ordinals does not make writes transactional or add historical
payload versions: a read observes the latest representable committed payload.

With ``FLOOR_ONLY``, an older-ordinal write or delete that executes later can
replace or remove newer state. For example, if a write to a cell completes at
ordinal 10 and then another completes at ordinal 9 while both exceed its floors,
its current payload comes from the ordinal-9 write. Reported latest ordinals may
decrease. Retained change history still uses numeric ordinals, so range selection and
``OVSTAGE_ERROR_OUT_OF_RANGE`` checks need not match payload completion order.
Ordinal ranges do not reconstruct historical payload snapshots. Coordinate
overlapping mutations in ordinal order when that ordering is required.

Choose the policy through :doc:`C process configuration
</guides/project_setup_c>` or :doc:`Python StageConfig
</guides/project_setup_python>` before creating instances. The selection remains
fixed until every initialization and instance reference has been released.
For example, ``NONDECREASING`` permits customization of cloned cells at the clone's
ordinal while it remains unsealed; ``STRICTLY_INCREASING`` requires a newer ordinal.

Use ``placement=`` to select the destination device independently of source
tensor residency; it applies to the entire write call. Availability events can
extend the source-buffer lifetime beyond ``wait()``. See
:ref:`ovstage-device-placement` for this experimental extension and
:ref:`ovstage-placement-failures` for its post-commit failure exception.

Write and Advance the Watermark
-------------------------------

This example writes one float per prim at ordinal 1, advances the watermark,
and reads the values back:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:minimal-write-read]
         :end-before: # [/snippet:minimal-write-read]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:minimal-write-read]
         :end-before: // [/snippet:minimal-write-read]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Key Parameters
--------------

The examples below run in order with a query containing ``/World/Writes/A``,
``/World/Writes/B``, and ``/World/Writes/C``, in that order. They use an existing
``stage`` and its :class:`PathDictionary <ovstage.PathDictionary>` as ``paths`` (``dict`` in C).
The first Python snippet includes the imports used by the later examples.
The C helper ``waitOp()`` waits for completion, checks errors, and releases
operation tracking; ``checkOvx()`` checks path-dictionary calls.


* ``attribute`` — a string or :term:`token <Token>` naming the attribute.
* ``ordinal`` — must clear the applicable watermarks and satisfy the configured
  :ref:`ordinal-admission-policies` for each affected cell and prim lifecycle;
  otherwise admission fails with ``ErrorCode.WRITE_FLOOR_VIOLATION``.
* ``is_array`` — ``False`` for one fixed-size value per prim; ``True`` for an
  array per prim. Shape and tensor count do not infer this choice.
* ``tensors`` — CPU or CUDA data, supplied as NumPy arrays, DLPack producers,
  :class:`ovstage.DLTensor` objects, or lists of tensors for array attributes.
  Keep the inputs alive and unchanged until the write completes, or through
  device availability when a placement event is supplied. See
  :doc:`/concepts/dlpack_tensors` for ownership and synchronization.


These snippets write ``heat`` values of 10, 20, and 30 to A, B, and C.
The source array stays alive until the write completes:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: setup, write-parameters

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: write-parameters-c

Fixed-Size Write Shapes
-----------------------

With ``is_array=False``, the first dimension counts source rows, normally one
per selected prim. A compact ``(N, 3)`` NumPy array supplies N vectors;
``(N, 4, 4)`` supplies N matrices. With ``index_map``, several prims can use the
same source row. Without it, N must equal the number of addressed prims.

Raw tensors can instead use ``shape=[N]`` with the full component count in
``dtype.lanes``. Writes fold trailing dimensions into storage lanes. Reads and maps return
scalar-lane tensors: ``(N, 3)`` for vectors and ``(N, 4, 4)`` for square matrices
with MATRIX semantic. Output dimensions follow the stored type and semantic,
not the original input shape. A flat ``(N * L,)`` one-lane tensor does not mean N values of
L components. Supply ``(N, L)`` or explicit lanes.


These snippets write one vector per prim from a compact ``(3, 3)`` buffer:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: write-fixed-shape

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: write-fixed-shape-c

Array Write Element Widths
--------------------------

With ``is_array=True``, ``dtype.lanes`` specifies the number of components in
**each array element**. Shape does not infer that width. The element count is
``total_bytes / (bits * lanes / 8)``.

For example, an ``(N, 3)`` one-lane tensor contains 3N scalar elements, not N
vectors. It fails against an existing ``float3[]`` attribute, but creates a
``float[]`` attribute if the name is new. The same rule applies to matrices:
use 16 lanes for each 4×4 matrix.

Use :func:`ovstage.make_dltensor()` with an explicit :class:`ovstage.DLDataType`
to describe a compatible compact buffer without copying it. For three-component
float32 elements, use :class:`DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3) <ovstage.DLDataType>`.
See :doc:`/concepts/dlpack_tensors` for the conversion rules.

Supply one tensor per prim when array lengths differ. A single tensor packs
all addressed arrays back to back; each must have the same byte length. The
payload must divide evenly into complete elements for every source row.


These snippets write arrays of two, one, and three vectors to A, B, and C.
The explicit three-lane dtype makes each array element a vector:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: write-array-width

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: write-array-width-c

Targeting a Subset of the Query
-------------------------------

By default, the first row of the input tensor supplies the value for the first
prim in the query, the second row supplies the second prim, and so on. Use
``count`` to write to only the first ``count`` prims; it cannot exceed the number
of prims in the query. Use ``index_map`` to choose which input row each prim
receives, or ``mask`` to skip selected prims and leave their values unchanged.
A write can use either ``index_map`` or ``mask``, but not both.

For fixed-size attributes (``is_array=False``), the required number of input
rows depends on these options. For a query containing three prims:

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Options
     - Required input
   * - No ``count``, ``index_map``, or ``mask``
     - Exactly three rows, one per prim.
   * - ``count=2``
     - Exactly two rows; only the first two prims are written.
   * - ``index_map=[0, 0, 0]``
     - One row is sufficient; all three prims receive it.
   * - ``index_map=[0, 0], count=2``
     - One row is sufficient; only the first two prims receive it.
   * - ``mask=[0b010], count=3``
     - Exactly three rows, although only the second prim is written.

With ``index_map``, every referenced input row must exist, but extra,
unreferenced rows are allowed. Python defaults ``count`` to the length of
``index_map``; C requires an explicit count.

Source Rows: ``index_map``
^^^^^^^^^^^^^^^^^^^^^^^^^^

``index_map[i]`` selects the input row for prim i. For example, ``[0, 0]``
writes row 0 to the first two prims. The input tensor can contain just that
one row; it does not need a separate row for each prim. A scalar value uses
shape ``(1,)``; one three-component vector uses shape ``(1, 3)``.
Unreferenced rows are unused.

For fixed-size values, each index must be less than ``shape[0]``. For one tensor
per array, it must be less than the tensor count. An invalid index fails with
``ErrorCode.INVALID_ARGUMENT``.

A single packed array tensor has no explicit source-row count. With an index
map, it is divided into ``max(index_map) + 1`` equal rows. Without one, it is
divided by the number of addressed prims. Extra packed rows beyond the highest
index cannot be represented; use one tensor per row when that distinction matters.


These snippets reuse one input value for A and B, and a second value for C:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: write-index-map

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: write-index-map-c

Target Prims: ``mask``
^^^^^^^^^^^^^^^^^^^^^^

Each mask bit selects a target prim. Unset bits leave those prims unchanged.
The payload must still contain a row for every addressed prim, including those
not selected. For example, ``mask=[0b10], count=2`` writes only the second prim.
You can also use a query that selects just that prim.

A mask is a list of 64-bit words in Python and a ``uint64_t`` array in C.
Prim i uses bit ``i % 64`` of word ``i // 64``, starting at the least-significant
bit. Supply at least ``(count + 63) // 64`` words. Python checks this length;
C callers must provide it or the runtime can read past the buffer.


These snippets update only B. The payload still contains three rows:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: write-mask

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: write-mask-c

Choosing ``count``
^^^^^^^^^^^^^^^^^^

* With ``index_map``, Python defaults to ``len(index_map)``. If supplied, count
  cannot exceed that length.
* With ``mask``, Python requires an explicit count.
* With neither, Python defaults to the full query. An explicit count must be
  positive; Python rejects 0 so an empty selection cannot become a full write.
* In C, 0 means the full query when neither option is present. Either option
  requires an explicit nonzero count.



These snippets use ``count=2`` to update only A and B; C keeps its value:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: write-count

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: write-count-c

Attribute Semantics
-------------------

The ``semantic`` parameter describes what the values mean. It uses
:class:`ovstage.AttributeSemantic` and defaults to ``NONE``. For example,
``MATRIX`` identifies a matrix independently of its component count.

Identifier semantics require values interned through the :doc:`path_dictionary`,
not text. Supply the dtype and element count listed below; mismatches fail with
``ErrorCode.INVALID_ARGUMENT``.

.. list-table::
   :header-rows: 1
   :widths: 34 26 40

   * - Semantic
     - Required dtype
     - Notes
   * - ``TOKEN_ID``
     - ``uint64``, 1 lane
     - One token id per value.
   * - ``RELATIONSHIP_PATH_ID``
     - ``uint64``, 1 lane
     - Path ids: ``is_array=True`` for a target list, or ``False`` for
       exactly one path per prim.
   * - ``CONNECTION_PATH_ID``
     - ``uint64``, 2 lanes
     - One ``(path id, token id)`` pair per target prim/property. Scalar and
       per-prim tensor forms are accepted, but each prim must still carry
       exactly one pair; extra pairs are rejected.
   * - ``ASSET_PATH_ID``
     - ``uint64``, 2 lanes
     - One ``(authored, resolved)`` token-id pair. ``is_array=False`` stores one
       ``asset``; ``True`` stores ``asset[]`` with one pair per element.
   * - ``PATH_EXPRESSION_STRING``
     - ``uint64``, 1 lane
     - One id for the authored expression text. ``is_array=False`` is a scalar
       ``pathExpression``, ``True`` is ``pathExpression[]`` with one id per
       element. ovstage stores the id and does not evaluate the expression.

An id of 0 means "authored, but empty" — no token, no path, no expression — so a
value that is legitimately empty is written as 0 rather than omitted.

The first four semantics select specific runtime types. Without
``ASSET_PATH_ID``, for example, a token pair is just two ``uint64`` values.
``PATH_EXPRESSION_STRING`` labels the meaning without selecting a distinct
storage type.

For transform authoring, see :doc:`transforms`. For a worked local-transform
write over successive ordinals, see :doc:`/guides/runtime_loop`.


These snippets write token identifiers for the materials steel, rubber, and
wood. ``TOKEN_ID`` makes the values tokens rather than ordinary integers:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: write-semantic

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: write-semantic-c

Advancing the Watermark
-----------------------

Call :meth:`ovstage.Stage.advance_write_floor()` and wait for completion before
reading the written values. The watermark never moves backwards; an advance
to a lower ordinal has no effect. It can cover all attributes or a selected
set. See :doc:`/concepts/overview` for the full write and read rules.


These snippets first advance the watermark for ``heat`` alone, then for all
attributes, including the ``material`` values written above:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/write-flavors/main.py
         :language: python
         :snippets: write-watermark

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/write-flavors/main.cpp
         :language: cpp
         :snippets: write-watermark-c

Passing the Attribute as String or Token
----------------------------------------

The following snippets pass an attribute by name or by its interned token:

.. tab-set::

   .. tab-item:: Python

      .. filtered-literalinclude:: ../../examples/python/minimal/main.py
         :language: python
         :start-after: # [snippet:string-or-token-arg]
         :end-before: # [/snippet:string-or-token-arg]
         :exclude-pattern: ^\s*#\s*\[/?snippet:
         :dedent:

   .. tab-item:: C

      .. filtered-literalinclude:: ../../examples/c/minimal/main.cpp
         :language: cpp
         :start-after: // [snippet:string-or-token-arg]
         :end-before: // [/snippet:string-or-token-arg]
         :exclude-pattern: ^\s*//\s*\[/?snippet:
         :dedent:

Further Reading
---------------

- :doc:`reading_attributes` — read values after advancing the watermark.
- :doc:`transforms` — author local transforms or effective world transforms.
- :doc:`/concepts/dlpack_tensors` — tensor layout, residency, and writing through mapped staging buffers.
- :doc:`/concepts/async_model` — ordinals, watermarks, and observing the write.
- :doc:`/concepts/error_handling` — asynchronous failures, partial publication, and repair.

See :doc:`/scene/device_placement` for the experimental GPU-to-GPU workflow,
extension access, event ordering, and buffer lifetimes.
