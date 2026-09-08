# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""ovstage — Python bindings for the OVStage data-plane C API.

Asynchronous, ordinal-keyed, zero-copy simulation data access over CPU/GPU
memory. Bound to the ovstage shared loader via ctypes. Set ``OVSTAGE_LIBRARY_PATH_HINT``
(or add the build output to ``LD_LIBRARY_PATH``) so the loader can find the
shared library.

Example::

    import numpy as np
    import ovstage
    from ovstage import OrdinalRange

    with ovstage.PathDictionary() as paths, ovstage.Stage("demo") as stage:
        plist = paths.create_path_list_from_strings(["/World/Cube"])
        attr = paths.intern_token("density")
        query = stage.query_from_path_list(plist)

        stage.write_attribute(
            query, attr, ordinal=2, tensors=np.array([1.5], np.float32), is_array=False
        ).wait()
        stage.advance_write_floor(ordinal=2).wait()

        read = stage.read_attributes(query, [attr], OrdinalRange.latest(2))
        read.wait()
        group = read.fetch_next()
        print(group.array(0))           # -> [1.5]
        stage.release_group(group)
        read.release().wait()
        paths.destroy_path_list(plist)
"""

import sys as _sys

from . import _src

# ``gates`` stays reachable as ``ovstage.gates`` for the concurrency tests, but it is
# deliberately absent from ``__all__`` and from the submodule registration below: it
# wraps the ``ovstage.test.hooks`` extension that only test-instrumented libovstage
# builds export, so it is instrumentation, not public API.
from ._src import gates, instancing, population  # noqa: F401  (gates: re-export only)
from ._src.bindings import (
    OVSTAGE_TIMEOUT_INFINITE,
    OVX_API_ERROR,
    OVX_API_SUCCESS,
    flush_log,
    library_version,
    set_log_callback,
)
from ._src.dlpack import (
    DLDataType,
    DLDataTypeCode,
    DLDevice,
    DLDeviceType,
    DLTensor,
    ManagedDLTensor,
    dltensor_to_numpy,
    make_dltensor,
    numpy_to_dldatatype,
)
from ._src.path_dictionary import OvxError, PathDictionary, PathList
from ._src.stage import Hierarchy, Map, OrdinalQuery, Query, Read, Stage
from ._src.types import (
    AttributeMeta,
    AttributeSemantic,
    ErrorCode,
    Filter,
    FilterOp,
    HierarchyComputationModel,
    HierarchyComputationModelDesc,
    HierarchyItem,
    HierarchyRelation,
    HierarchyResult,
    LogSeverity,
    MapGroup,
    Operation,
    OrdinalRange,
    OvstageError,
    Predicate,
    PopulationDomain,
    PrimMode,
    QueryResult,
    ReadGroup,
    Scope,
    StageConfig,
    TIMEOUT_INFINITE,
    WriteDesc,
)

# __version__ mirrors the installed wheel's distribution version. The build injects
# an auto-generated ``_version.py`` carrying the exact distribution version string;
# import it so ``ovstage.__version__`` always agrees with ``pip show ovstage`` / the
# dist filename. A source / editable checkout has no ``_version.py`` (or a malformed
# one), so fall back to the source default rather than breaking ``import ovstage``.
try:
    from ._version import version as __version__
except Exception:
    __version__ = "0.2.0"

# ``from ._src import instancing, population`` above only *binds* the module objects
# as attributes of this package; it does not register them as submodules, so
# ``import ovstage.population`` / ``from ovstage.population import open_usd`` would
# raise ModuleNotFoundError even though the docs (and ``.. automodule::``) publish
# that dotted path. Register the public aliases so every import spelling resolves.
# The same module object is reused, so ``ovstage.population is
# ovstage._src.population`` and patching either path affects both.
_sys.modules[f"{__name__}.instancing"] = instancing
_sys.modules[f"{__name__}.population"] = population

__all__ = [
    "Stage",
    "StageConfig",
    "Query",
    "Read",
    "Map",
    "OrdinalQuery",
    "Hierarchy",
    "PathDictionary",
    "PathList",
    "OvxError",
    "OvstageError",
    "ErrorCode",
    "FilterOp",
    "PrimMode",
    "AttributeSemantic",
    "HierarchyRelation",
    "HierarchyComputationModel",
    "Scope",
    "PopulationDomain",
    "OrdinalRange",
    "Predicate",
    "Filter",
    "Operation",
    "WriteDesc",
    "ReadGroup",
    "MapGroup",
    "AttributeMeta",
    "QueryResult",
    "HierarchyItem",
    "HierarchyResult",
    "HierarchyComputationModelDesc",
    "DLTensor",
    "ManagedDLTensor",
    "DLDataType",
    "DLDevice",
    "DLDeviceType",
    "DLDataTypeCode",
    "make_dltensor",
    "dltensor_to_numpy",
    "numpy_to_dldatatype",
    "instancing",
    "population",
    "library_version",
    "set_log_callback",
    "flush_log",
    "LogSeverity",
    "TIMEOUT_INFINITE",
    "OVSTAGE_TIMEOUT_INFINITE",
    "OVX_API_SUCCESS",
    "OVX_API_ERROR",
    "__version__",
]
