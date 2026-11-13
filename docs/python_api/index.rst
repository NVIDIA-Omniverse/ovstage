.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
.. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
..
.. NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
.. property and proprietary rights in and to this material, related
.. documentation and any modifications thereto. Any use, reproduction,
.. disclosure or distribution of this material and related documentation
.. without an express license agreement from NVIDIA CORPORATION or
.. its affiliates is strictly prohibited.

Python API Reference
=====================

The Python API provides a ctypes-based interface to the ovstage runtime data plane.

.. contents:: Contents
   :local:
   :depth: 2

ovstage
-------

.. _python-handle-types:

Handle Types
^^^^^^^^^^^^

.. automodule:: ovstage.handles
   :no-members:
   :no-special-members:

.. autoclass:: ovstage.OpId

.. autoclass:: ovstage.QueryHandle

.. autoclass:: ovstage.ReadHandle

.. autoclass:: ovstage.MapHandle

.. autoclass:: ovstage.OrdinalQueryHandle

.. autoclass:: ovstage.ReadGroupId

.. autoclass:: ovstage.QueryResultId

.. autoclass:: ovstage.HierarchyHandle

.. autoclass:: ovstage.HierarchyResultId

.. autoclass:: ovstage.PopulationOpId

.. autoclass:: ovstage.PopulationUsdReferenceHandle

.. autoclass:: ovstage.PopulationExportDestinationHandle

.. autodata:: ovstage.INVALID_OP_ID

.. autodata:: ovstage.INVALID_QUERY_HANDLE

.. autodata:: ovstage.INVALID_READ_HANDLE

.. autodata:: ovstage.INVALID_MAP_HANDLE

.. autodata:: ovstage.INVALID_ORDINAL_QUERY_HANDLE

.. autodata:: ovstage.INVALID_READ_GROUP_ID

.. autodata:: ovstage.INVALID_QUERY_RESULT_ID

.. autodata:: ovstage.INVALID_HIERARCHY_HANDLE

.. autodata:: ovstage.INVALID_HIERARCHY_RESULT_ID

.. autodata:: ovstage.POPULATION_INVALID_OP_ID

.. autodata:: ovstage.POPULATION_INVALID_USD_REFERENCE_HANDLE

.. autodata:: ovstage.POPULATION_EXPORT_INVALID_DESTINATION_HANDLE

Version and Constants
^^^^^^^^^^^^^^^^^^^^^^

.. autodata:: ovstage.__version__

.. autofunction:: ovstage.get_build_info

.. autoclass:: ovstage.BuildInfo
   :members:

.. autofunction:: ovstage.library_version

.. autodata:: ovstage.TIMEOUT_INFINITE

.. autodata:: ovstage.OVSTAGE_TIMEOUT_INFINITE

.. The ordinal bounds are re-exported data: the docstrings in ``_src/types.py`` do not
   reach the ``ovstage`` namespace, so each entry carries its one-line description.
.. autodata:: ovstage.ORDINAL_MIN

   Initial sealed empty floor; writes must be strictly above this minimum.

.. autodata:: ovstage.ORDINAL_MAX

   Largest representable ordinal; also the current-value read end.

.. autodata:: ovstage.OVSTAGE_ORDINAL_MIN

   C-named alias of :data:`ovstage.ORDINAL_MIN`.

.. autodata:: ovstage.OVSTAGE_ORDINAL_MAX

   C-named alias of :data:`ovstage.ORDINAL_MAX`.

.. autodata:: ovstage.POPULATION_THREAD_LIMIT_AUTO

.. autodata:: ovstage.OVX_API_SUCCESS

.. autodata:: ovstage.OVX_API_ERROR

Stage and Path Dictionary
^^^^^^^^^^^^^^^^^^^^^^^^^^

.. autoclass:: ovstage.Stage
   :members:
   :undoc-members:

.. autoclass:: ovstage.StageConfig
   :members:
   :undoc-members:

.. autoclass:: ovstage.PathDictionary
   :members:
   :undoc-members:

.. autoclass:: ovstage.Token
   :members:

.. autoclass:: ovstage.PrimPath
   :members:

.. autoclass:: ovstage.PrimPathList
   :members:

.. ``PathList`` is a ``PrimPathList`` subclass that only ``PathDictionary.create_path_list*``
   mints. Its constructor is excluded because the separated class signature would
   otherwise publish the internal ``__init__``/``__new__`` arguments as API.
.. autoclass:: ovstage.PathList
   :members:
   :undoc-members:
   :exclude-members: __new__, __init__

Queries, Reads, and Maps
^^^^^^^^^^^^^^^^^^^^^^^^^

.. autoclass:: ovstage.Query
   :members:
   :undoc-members:
   :inherited-members:

.. autoclass:: ovstage.OrdinalQuery
   :members:
   :undoc-members:
   :inherited-members:

.. autoclass:: ovstage.Read
   :members:
   :undoc-members:
   :inherited-members:

.. autoclass:: ovstage.Map
   :members:
   :undoc-members:
   :inherited-members:

.. autoclass:: ovstage.Operation
   :members:
   :undoc-members:

.. autoclass:: ovstage.CloneOperation
   :members:
   :undoc-members:

.. autoclass:: ovstage.Hierarchy
   :members:
   :undoc-members:
   :inherited-members:

Instancing Queries
^^^^^^^^^^^^^^^^^^

.. The module entry makes ``ovstage.instancing`` cross-referenceable and renders its
   docstring. ``:no-members:`` because the functions are documented individually
   below; documenting them twice fails the strict build.
.. automodule:: ovstage.instancing
   :no-members:
   :no-special-members:

.. autofunction:: ovstage.instancing.available

.. autofunction:: ovstage.instancing.get_prototype_roots

.. autofunction:: ovstage.instancing.resolve_paths

.. autoclass:: ovstage.instancing.PathKind
   :members:

.. autoclass:: ovstage.instancing.PathResolution
   :members:

.. autofunction:: ovstage.instancing.get_prototype_root

.. autofunction:: ovstage.instancing.get_instance_roots

.. autofunction:: ovstage.instancing.get_instance_roots_batch

Data and Result Types
^^^^^^^^^^^^^^^^^^^^^^

.. autoclass:: ovstage.WriteDesc
   :members:
   :undoc-members:

.. autoclass:: ovstage.OrdinalRange
   :members:
   :undoc-members:

.. autoclass:: ovstage.Predicate
   :members:
   :undoc-members:

.. autoclass:: ovstage.Filter
   :members:
   :undoc-members:

.. autoclass:: ovstage.ReadGroup
   :members:
   :undoc-members:
   :inherited-members:

.. autoclass:: ovstage.ReadColumn
   :members:
   :undoc-members:
   :inherited-members:

.. autoclass:: ovstage.MapGroup
   :members:
   :undoc-members:
   :inherited-members:

.. autoclass:: ovstage.AttributeMeta
   :members:
   :undoc-members:

.. autoclass:: ovstage.QueryResult
   :members:
   :undoc-members:

.. autoclass:: ovstage.CloneResult
   :members:
   :undoc-members:

.. autoclass:: ovstage.HierarchyItem
   :members:
   :undoc-members:

.. autoclass:: ovstage.HierarchyResult
   :members:
   :undoc-members:

.. autoclass:: ovstage.HierarchyComputationModelDesc
   :members:
   :undoc-members:

Enums
^^^^^

.. autoclass:: ovstage.ErrorCode
   :members:
   :undoc-members:

.. autoclass:: ovstage.FilterOp
   :members:
   :undoc-members:

.. autoclass:: ovstage.PrimMode
   :members:
   :undoc-members:

.. autoclass:: ovstage.AttributeMode
   :members:
   :undoc-members:

.. autoclass:: ovstage.AttributeSemantic
   :members:
   :undoc-members:

.. autoclass:: ovstage.Scope
   :members:
   :undoc-members:

.. autoclass:: ovstage.PopulationDomain
   :members:
   :undoc-members:

.. autoclass:: ovstage.HierarchyRelation
   :members:
   :undoc-members:

.. autoclass:: ovstage.OrdinalAdmissionPolicy
   :members:
   :undoc-members:

.. autoclass:: ovstage.HierarchyComputationModel
   :members:
   :undoc-members:

Errors
^^^^^^

.. autoclass:: ovstage.OvstageError
   :members:
   :undoc-members:

.. autoclass:: ovstage.OvxError
   :members:
   :undoc-members:

Logging
^^^^^^^

.. autofunction:: ovstage.set_log_callback

.. autofunction:: ovstage.flush_log

.. autoclass:: ovstage.LogSeverity
   :members:
   :undoc-members:

USD Schemas
^^^^^^^^^^^

.. autofunction:: ovstage.population.register_usd_schemas

DLPack Tensor Interchange
^^^^^^^^^^^^^^^^^^^^^^^^^^

.. autoclass:: ovstage.DLTensor
   :members:
   :undoc-members:

.. autoclass:: ovstage.DLDataType
   :members:
   :undoc-members:

.. autoclass:: ovstage.DLDevice
   :members:
   :undoc-members:

.. autoclass:: ovstage.DLDeviceType
   :members:
   :undoc-members:

.. autoclass:: ovstage.DLDataTypeCode
   :members:
   :undoc-members:

.. autoclass:: ovstage.ManagedDLTensor
   :members:
   :undoc-members:

.. autofunction:: ovstage.typed_tensor_to_write

.. autofunction:: ovstage.make_dltensor

.. autofunction:: ovstage.dltensor_to_numpy

.. autofunction:: ovstage.numpy_to_dldatatype

Experimental Device Placement
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Pass ``placement=`` to stage read, write, or map methods. Availability depends
on the runtime's experimental extension; see :ref:`ovstage-device-placement`.

.. autoclass:: ovstage.Placement
   :members:
   :undoc-members:

ovstage.population
------------------

USD population composes USD content into the runtime stage; population export
authors selected runtime state back into a USD destination that ovstage opens
and owns. See :doc:`/scene/population` and :doc:`/scene/exporting_to_usd`.

.. Symbols documented explicitly elsewhere on this page (the classes below, and
   ``register_usd_schemas`` under "USD Schemas") must be excluded here, or
   ``population.__all__`` documents them a second time and the strict ``-W``
   docs build fails with "duplicate object description". Keep this list in sync.
.. automodule:: ovstage.population
   :members:
   :undoc-members:
   :exclude-members: Desc, Selector, PrimPredicate, PropertyPredicate, PrimPredicateKind, PropertyPredicateKind, register_usd_schemas

Descriptions and Selectors
^^^^^^^^^^^^^^^^^^^^^^^^^^

.. autoclass:: ovstage.population.Desc
   :members:
   :undoc-members:

.. autoclass:: ovstage.population.Selector
   :members:
   :undoc-members:

.. autoclass:: ovstage.population.PrimPredicate
   :members:
   :undoc-members:

.. autoclass:: ovstage.population.PropertyPredicate
   :members:
   :undoc-members:

.. autoclass:: ovstage.population.PrimPredicateKind
   :members:
   :undoc-members:

.. autoclass:: ovstage.population.PropertyPredicateKind
   :members:
   :undoc-members:

Export Constants
^^^^^^^^^^^^^^^^

Constants used by the population export API (refer to
:doc:`/scene/exporting_to_usd`): the integer selection, layer-mode,
destination-mode, path- and name-match, transform, unknown-metadata,
source-API-schema, projection, rule, property-kind and metadata-kind values,
plus the ``customData`` key
(``OVSTAGE_POPULATION_EXPORT_UNKNOWN_APPLIED_API_SCHEMAS_CUSTOM_DATA_KEY``) under
which unresolved applied API schema names are preserved when the unknown-metadata
policy is ``PRESERVE``. :class:`~ovstage.population.ExportDestinationMode`,
:class:`~ovstage.population.SourceApiSchemaPolicy` and
:class:`~ovstage.population.ExportProjection` wrap the destination,
source-API-schema and projection families as enums.

.. These are re-exported module data without ``#:`` doc comments, which the
   ``automodule`` above skips silently; list each one explicitly.
.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_LAYER_MODE_OVER

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_DESTINATION_EMPTY

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_DESTINATION_OPEN_EXISTING

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_PATH_MATCH_PREFIX

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_NAME_MATCH_EXACT

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_TRANSFORM_NONE

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_MATRIX_OP

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_PRESERVE

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_FAIL

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_EXPLICIT_ONLY

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_SOURCE_API_SCHEMAS_APPLY_RECORDED

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_PROJECTION_EXPLICIT_RULES

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_PROJECTION_SCHEMA_DECLARED

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_UNKNOWN_APPLIED_API_SCHEMAS_CUSTOM_DATA_KEY

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_RULE_NONE

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_RULE_EXPORT_DEFAULT_VALUE

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_RULE_DERIVED_ALLOWED

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_PROPERTY_RELATIONSHIP

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_PROPERTY_CONNECTION

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_PROPERTY_MATERIAL_BINDING

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_METADATA_PRIM

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_CUSTOM_DATA

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_METADATA_REFERENCES

.. autodata:: ovstage.population.OVSTAGE_POPULATION_EXPORT_METADATA_LAYER_STANDARD
