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

Version and Constants
^^^^^^^^^^^^^^^^^^^^^^

.. autodata:: ovstage.__version__

.. autofunction:: ovstage.library_version

.. autodata:: ovstage.TIMEOUT_INFINITE

.. autodata:: ovstage.OVSTAGE_TIMEOUT_INFINITE

.. autodata:: ovstage.OVX_API_SUCCESS

.. autodata:: ovstage.OVX_API_ERROR

Stage and Path Dictionary
^^^^^^^^^^^^^^^^^^^^^^^^^^

.. autoclass:: ovstage.Stage
   :members:
   :undoc-members:

.. autoclass:: ovstage.PathDictionary
   :members:
   :undoc-members:

Queries, Reads, and Maps
^^^^^^^^^^^^^^^^^^^^^^^^^

.. autoclass:: ovstage.Query
   :members:
   :undoc-members:

.. autoclass:: ovstage.OrdinalQuery
   :members:
   :undoc-members:

.. autoclass:: ovstage.Read
   :members:
   :undoc-members:

.. autoclass:: ovstage.Map
   :members:
   :undoc-members:

.. autoclass:: ovstage.Operation
   :members:
   :undoc-members:

.. autoclass:: ovstage.Hierarchy
   :members:
   :undoc-members:

Instancing Queries
^^^^^^^^^^^^^^^^^^

.. autofunction:: ovstage.instancing.available

.. autofunction:: ovstage.instancing.get_prototype_roots

.. autofunction:: ovstage.instancing.get_prototype_root

.. autofunction:: ovstage.instancing.get_instance_roots

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

.. autoclass:: ovstage.MapGroup
   :members:
   :undoc-members:

.. autoclass:: ovstage.AttributeMeta
   :members:
   :undoc-members:

.. autoclass:: ovstage.QueryResult
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

.. autofunction:: ovstage.make_dltensor

.. autofunction:: ovstage.dltensor_to_numpy

.. autofunction:: ovstage.numpy_to_dldatatype

ovstage.population
------------------

USD population: composes USD content into the runtime stage (see
:doc:`/scene/population`).

.. automodule:: ovstage.population
   :members:
   :undoc-members:
