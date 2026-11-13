# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Dictionary and resource identities for the ovstage bindings.

Keep handles typed through API calls, arrays, callbacks, and results. Handle
arguments require the matching type; plain integers and other handle kinds are
rejected with ``TypeError``. Construct the matching type from a raw uint64 only
at an explicit storage or interchange boundary. Use ``raw_id`` to extract that
number; ``int(handle)`` is not supported.

Handles identify objects within the scope defined by their API. Their kind and
raw ID do not prove ownership, liveness, or membership in a particular owner.
Copying a handle does not retain its resource or extend its lifetime.

Equality and hashing use the handle kind and raw ID. An invalid ``QueryHandle()``
differs from integer zero; compare it with ``ovstage.INVALID_QUERY_HANDLE``.
For types with an invalid sentinel, ``bool(handle)`` checks only whether the
value differs from that sentinel. It does not establish resource liveness.
Resource and operation IDs also support same-kind ordering with ``<``.

Named invalid handles are shared ctypes objects for comparisons and value copies.
Never pass them as writable native output buffers, including via ``ctypes.byref()``:
native writes can mutate their storage despite Python attribute assignment guards.
Allocate a fresh ``QueryHandle()`` or copy the sentinel with
``QueryHandle(ovstage.INVALID_QUERY_HANDLE)`` for an output argument.
"""

import ctypes
import operator


def _public_name(t):
    package = t.__module__.partition(".")[0]
    return t.__qualname__ if package == "builtins" else f"{package}.{t.__qualname__}"


class _Handle(ctypes.Structure):
    _fields_ = [("_value", ctypes.c_uint64)]
    _invalid = 0

    def __init__(self, value=0):
        if isinstance(value, _Handle):
            value = self._kind._require(value).raw_id
        if isinstance(value, bool):
            raise TypeError("a handle value must be an integer, not bool")
        value = operator.index(value)
        if not 0 <= value < 2**64:
            raise ValueError("a handle value must fit uint64")
        super().__init__()
        super().__setattr__("_value", value)

    @classmethod
    def _require(cls, value, name="handle"):
        if not isinstance(value, cls):
            raise TypeError(f"{name} must be {_public_name(cls)}, got {_public_name(type(value))}")
        return value

    @property
    def raw_id(self):
        """The raw uint64, for explicit storage/interchange conversions."""
        return self._value

    def __setattr__(self, name, value):
        if name in ("raw_id", "_value"):
            raise AttributeError("handle values cannot be assigned from Python")
        super().__setattr__(name, value)

    def __int__(self):
        # Without this guard, int() can parse the ctypes buffer as ASCII digits.
        raise TypeError("use .raw_id for explicit numeric conversion")

    def __bool__(self):
        return self.raw_id != self._invalid

    def __eq__(self, other):
        if not isinstance(other, _Handle) or self._kind is not other._kind:
            return NotImplemented
        return self.raw_id == other.raw_id

    def __hash__(self):
        return hash((self._kind, self.raw_id))

    def __repr__(self):
        return f"{self._kind.__name__}({self.raw_id})"

    def __copy__(self):
        return self._kind(self.raw_id)

    def __deepcopy__(self, memo):
        return self.__copy__()

    def __reduce__(self):
        return (self._kind, (self.raw_id,))


class Token(_Handle):
    """An interned string (ovx_token_t); zero is the valid empty token."""

    _invalid = 2**64 - 1


class PrimPath(_Handle):
    """An interned prim path (ovx_primpath_t); zero is invalid."""


class PrimPathList(_Handle):
    """A non-owning path-list handle (ovx_primpath_list_t); zero is invalid."""


Token._kind = Token
PrimPath._kind = PrimPath
PrimPathList._kind = PrimPathList


class _ResourceHandle(_Handle):
    """Typed resource identity; does not establish ownership or liveness."""

    def __lt__(self, other):
        if not isinstance(other, _ResourceHandle) or self._kind is not other._kind:
            return NotImplemented
        return self.raw_id < other.raw_id


class OpId(_ResourceHandle):
    """Binding class for ``ovstage_op_id_t``, identifying an asynchronous data-plane operation on a stage, used to poll
    or wait.
    """


OpId._kind = OpId


class QueryHandle(_ResourceHandle):
    """Binding class for ``ovstage_query_handle_t``, identifying a logical set of prims queried on a stage."""


QueryHandle._kind = QueryHandle


class ReadHandle(_ResourceHandle):
    """Binding class for ``ovstage_read_handle_t``, identifying an enqueued multi-attribute read on a stage."""


ReadHandle._kind = ReadHandle


class MapHandle(_ResourceHandle):
    """Binding class for ``ovstage_map_handle_t``, identifying an enqueued mapped-write session on a stage."""


MapHandle._kind = MapHandle


class OrdinalQueryHandle(_ResourceHandle):
    """Binding class for ``ovstage_ordinal_query_handle_t``, identifying an enqueued read of an ordinal scalar from a
    stage, such as a write floor.
    """


OrdinalQueryHandle._kind = OrdinalQueryHandle


class ReadGroupId(_ResourceHandle):
    """Binding class for ``ovstage_read_group_id_t``, identifying a fetched read-group payload scoped to a stage;
    release it after consuming its tensors.
    """


ReadGroupId._kind = ReadGroupId


class QueryResultId(_ResourceHandle):
    """Binding class for ``ovstage_query_result_id_t``, identifying a fetched query-result payload from a stage query.
    """


QueryResultId._kind = QueryResultId


class HierarchyHandle(_ResourceHandle):
    """Binding class for ``ovstage_hierarchy_handle_t``, identifying an enqueued hierarchy lookup batch on a stage."""


HierarchyHandle._kind = HierarchyHandle


class HierarchyResultId(_ResourceHandle):
    """Binding class for ``ovstage_hierarchy_result_id_t``, identifying a fetched hierarchy-result payload from a stage
    lookup.
    """


HierarchyResultId._kind = HierarchyResultId


class PopulationOpId(_ResourceHandle):
    """Binding class for ``ovstage_population_op_id_t``, identifying a population operation on a stage; wait through the
    population API.
    """


PopulationOpId._kind = PopulationOpId


class PopulationUsdReferenceHandle(_ResourceHandle):
    """Binding class for ``ovstage_population_usd_reference_handle_t``, identifying a USD reference added to a stage's
    population state, used to remove that reference.
    """


PopulationUsdReferenceHandle._kind = PopulationUsdReferenceHandle


class PopulationExportDestinationHandle(_ResourceHandle):
    """Binding class for ``ovstage_population_export_destination_handle_t``, identifying a reusable USD export
    destination bound to its source stage.
    """


PopulationExportDestinationHandle._kind = PopulationExportDestinationHandle


# Named sentinels retain the identity kind at checks and reset boundaries.
INVALID_OP_ID = OpId()
INVALID_QUERY_HANDLE = QueryHandle()
INVALID_READ_HANDLE = ReadHandle()
INVALID_MAP_HANDLE = MapHandle()
INVALID_ORDINAL_QUERY_HANDLE = OrdinalQueryHandle()
INVALID_READ_GROUP_ID = ReadGroupId()
INVALID_QUERY_RESULT_ID = QueryResultId()
INVALID_HIERARCHY_HANDLE = HierarchyHandle()
INVALID_HIERARCHY_RESULT_ID = HierarchyResultId()
POPULATION_INVALID_OP_ID = PopulationOpId()
POPULATION_INVALID_USD_REFERENCE_HANDLE = PopulationUsdReferenceHandle()
POPULATION_EXPORT_INVALID_DESTINATION_HANDLE = PopulationExportDestinationHandle()
