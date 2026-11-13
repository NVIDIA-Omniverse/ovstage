# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
"""The handle kind must survive ctypes, Python collections and borrowed copies."""

import copy
import ctypes
import pickle

import pytest
from ovstage._src import bindings as b
from ovstage._src.types import check_path_list, check_primpath, check_token

from ovstage import PrimPath, PrimPathList, Token


@pytest.mark.parametrize("kind", [Token, PrimPath, PrimPathList])
def test_layout_and_explicit_raw_conversion(kind):
    handle = kind(42)
    assert ctypes.sizeof(kind) == ctypes.sizeof(ctypes.c_uint64)
    assert ctypes.alignment(kind) == ctypes.alignment(ctypes.c_uint64)
    assert handle.raw_id == 42
    with pytest.raises(TypeError, match="raw_id"):
        int(handle)
    # The ctypes buffer must not be interpreted as ASCII digits by int().
    with pytest.raises(TypeError, match="raw_id"):
        int(kind(int.from_bytes(b"12345678", "little")))
    assert not isinstance(handle, int)
    for other in (copy.copy(handle), copy.deepcopy(handle), pickle.loads(pickle.dumps(handle))):
        assert type(other) is kind
        assert other == handle
        assert hash(other) == hash(handle)
    with pytest.raises(AttributeError):
        handle.raw_id = 13


@pytest.mark.parametrize("kind", [Token, PrimPath, PrimPathList])
@pytest.mark.parametrize("raw", [-1, 2**64])
def test_raw_constructor_rejects_overflow(kind, raw):
    with pytest.raises(ValueError):
        kind(raw)


@pytest.mark.parametrize("kind", [Token, PrimPath, PrimPathList])
@pytest.mark.parametrize("raw", [True, 1.5, "7"])
def test_raw_constructor_rejects_noninteger(kind, raw):
    with pytest.raises(TypeError):
        kind(raw)


def test_overlapping_values_remain_distinct():
    assert len({Token(7), PrimPath(7), PrimPathList(7), 7}) == 4
    assert bool(Token(0))  # empty token is valid
    assert not b.OVX_INVALID_TOKEN
    assert not b.OVX_INVALID_PRIMPATH
    assert not b.OVX_INVALID_PRIMPATH_LIST


@pytest.mark.parametrize(
    "kind,check", [(Token, check_token), (PrimPath, check_primpath), (PrimPathList, check_path_list)]
)
def test_api_and_ctypes_reject_wrong_kind(kind, check):
    for wrong in (7, *[other(7) for other in (Token, PrimPath, PrimPathList) if other is not kind]):
        with pytest.raises(TypeError):
            check(wrong)
        with pytest.raises(TypeError):
            (kind * 1)(wrong)
    assert check(kind(7)) == kind(7)


def test_token_or_string_rejects_other_handles():
    assert b.make_token_or_string(Token(7)).token == Token(7)
    assert str(b.make_token_or_string("name").string) == "name"
    for wrong in (7, PrimPath(7), PrimPathList(7)):
        with pytest.raises(TypeError):
            b.make_token_or_string(wrong)


def test_dictionary_returns_typed_handles_and_preserves_ownership(stage, ovstage_mod):
    with ovstage_mod.PathDictionary(stage) as paths:
        token = paths.intern_token("name")
        path = paths.intern_path("/World/A")
        assert isinstance(token, Token)
        assert isinstance(path, PrimPath)
        assert paths.token_to_string(token) == "name"
        assert paths.path_to_string(path) == "/World/A"
        with paths.create_path_list([path]) as owned:
            borrowed = copy.copy(owned)
            assert type(borrowed) is PrimPathList
            assert owned == borrowed
            assert paths.get_paths(borrowed) == [path]
            paths.add_path_list_reference(borrowed)
        assert owned.released
        assert paths.path_list_count(borrowed) == 1
        paths.destroy_path_list(borrowed)
        with pytest.raises(TypeError):
            paths.token_to_string(path)
        with pytest.raises(TypeError):
            paths.path_to_string(token)
        with pytest.raises(TypeError):
            paths.path_list_count(path)


@pytest.mark.parametrize("kind", [Token, PrimPath, PrimPathList])
def test_ordering_requires_explicit_raw_ids(kind):
    assert sorted([kind(8), kind(3)], key=lambda handle: handle.raw_id) == [kind(3), kind(8)]
    for other in (3, Token(3), PrimPath(3), PrimPathList(3)):
        with pytest.raises(TypeError):
            kind(8) < other


_STAGE_INVALID = {
    b.ovstage_op_id_t: b.OVSTAGE_INVALID_OP_ID,
    b.ovstage_query_handle_t: b.OVSTAGE_INVALID_QUERY_HANDLE,
    b.ovstage_read_handle_t: b.OVSTAGE_INVALID_READ_HANDLE,
    b.ovstage_map_handle_t: b.OVSTAGE_INVALID_MAP_HANDLE,
    b.ovstage_ordinal_query_handle_t: b.OVSTAGE_INVALID_ORDINAL_QUERY_HANDLE,
    b.ovstage_read_group_id_t: b.OVSTAGE_INVALID_READ_GROUP_ID,
    b.ovstage_query_result_id_t: b.OVSTAGE_INVALID_QUERY_RESULT_ID,
    b.ovstage_hierarchy_handle_t: b.OVSTAGE_INVALID_HIERARCHY_HANDLE,
    b.ovstage_hierarchy_result_id_t: b.OVSTAGE_INVALID_HIERARCHY_RESULT_ID,
    b.ovstage_population_op_id_t: b.OVSTAGE_POPULATION_INVALID_OP_ID,
    b.ovstage_population_usd_reference_handle_t: b.OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE,
    b.ovstage_population_export_destination_handle_t: b.OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE,
}


_STAGE_RESOURCE_KINDS = tuple(_STAGE_INVALID)


@pytest.mark.parametrize("kind,invalid", _STAGE_INVALID.items())
def test_invalid_resource_identity_is_typed_and_separate_from_output_storage(kind, invalid):
    assert kind() == invalid
    assert kind() != 0 and 0 != kind()
    assert not kind()

    @ctypes.CFUNCTYPE(None, ctypes.POINTER(kind))
    def write_identity(output):
        output[0] = kind(2**63 + 17)

    for output in (kind(), kind(invalid)):
        assert ctypes.addressof(output) != ctypes.addressof(invalid)
        write_identity(ctypes.byref(output))
        assert output.raw_id == 2**63 + 17
        assert invalid == kind()
        assert not invalid


@pytest.mark.parametrize("kind", _STAGE_RESOURCE_KINDS)
def test_resource_identity_survives_structure_array_and_callback(kind):
    """An operation/result/resource identity stays typed at every ctypes boundary."""

    class Record(ctypes.Structure):
        _fields_ = [("identity", kind)]

    value = kind(2**63 + 17)
    record = Record(value)
    values = (kind * 2)(record.identity, kind())
    seen = []

    @ctypes.CFUNCTYPE(None, kind, ctypes.POINTER(kind))
    def copy_identity(identity, output):
        seen.append(identity)
        output[0] = identity

    copy_identity(values[0], ctypes.byref(values[1]))
    assert seen == [value]
    assert type(seen[0]) is kind
    assert values[1] == value
    assert ctypes.sizeof(kind) == ctypes.sizeof(ctypes.c_uint64)
    assert ctypes.alignment(kind) == ctypes.alignment(ctypes.c_uint64)
    assert kind() == _STAGE_INVALID[kind]
    assert sorted([value, kind(7)]) == [kind(7), value]
    for copied in (copy.copy(value), copy.deepcopy(value), pickle.loads(pickle.dumps(value))):
        assert type(copied) is kind
        assert copied == value and hash(copied) == hash(value)
    with pytest.raises(TypeError, match="raw_id"):
        int(value)
    for wrong in (17, Token(17), *[other(17) for other in _STAGE_RESOURCE_KINDS if other is not kind]):
        assert value != wrong
        with pytest.raises(TypeError):
            kind._require(wrong)
        with pytest.raises(TypeError):
            Record(wrong)
        with pytest.raises(TypeError):
            (kind * 1)(wrong)
        with pytest.raises((TypeError, ctypes.ArgumentError)):
            copy_identity(wrong, ctypes.byref(values[1]))


def test_stage_resource_wrappers_reject_other_kinds():
    from ovstage import Hierarchy, Map, Operation, OrdinalQuery, Query, Read, population

    wrappers = [
        (Query, b.ovstage_query_handle_t),
        (Read, b.ovstage_read_handle_t),
        (Map, b.ovstage_map_handle_t),
        (Hierarchy, b.ovstage_hierarchy_handle_t),
        (OrdinalQuery, b.ovstage_ordinal_query_handle_t),
    ]
    for wrapper, kind in wrappers:
        resource = wrapper(None, kind(7), None)
        resource._released = True  # No native resource was allocated by this ABI test.
        assert type(resource.handle) is kind
        for wrong in (7, b.ovstage_op_id_t(7)):
            with pytest.raises(TypeError):
                wrapper(None, wrong, None)
    assert type(Operation(None, b.OVSTAGE_OK, b.ovstage_op_id_t(7)).op_id) is b.ovstage_op_id_t
    assert (
        type(population.Operation(None, b.OVSTAGE_OK, b.ovstage_population_op_id_t(7)).op_id)
        is b.ovstage_population_op_id_t
    )
    with pytest.raises(TypeError):
        Operation(None, b.OVSTAGE_OK, b.ovstage_population_op_id_t(7))
    with pytest.raises(TypeError):
        population.Operation(None, b.OVSTAGE_OK, b.ovstage_op_id_t(7))


def test_query_result_copies_handle_before_releasing_native_result():
    from types import SimpleNamespace

    from ovstage import Stage

    identity = b.ovstage_query_handle_t(37)

    def fetch(_instance, query, _timeout, result):
        assert query == identity
        result._obj.all_handle = identity
        return b.OVSTAGE_OK

    def release(_instance, result):
        result._obj.all_handle = b.ovstage_query_handle_t()
        return b.OVSTAGE_OK

    stage = Stage.__new__(Stage)
    stage._inst = None
    stage._api = SimpleNamespace(ovstage_fetch_query_result=fetch, ovstage_release_query_result=release)
    assert stage.fetch_query_result(identity).all_handle == identity


def test_population_export_uses_typed_invalid_destination():
    from ovstage._src import population_export_bindings as export

    assert (
        type(export.OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE)
        is b.ovstage_population_export_destination_handle_t
    )
    assert (
        export.OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE
        == b.OVSTAGE_POPULATION_EXPORT_INVALID_DESTINATION_HANDLE
    )


@pytest.mark.parametrize(
    "symbol,kind,arguments",
    [
        ("ovstage_fetch_hierarchy_result", b.ovstage_hierarchy_handle_t, (None,)),
        ("ovstage_population_wait_op", b.ovstage_population_op_id_t, (0, None)),
        ("ovstage_population_export_wait_op", b.ovstage_population_op_id_t, (0, None)),
        ("ovstage_population_export_destination_destroy", b.ovstage_population_export_destination_handle_t, ()),
    ],
)
def test_family_receivers_preserve_handle_kind(symbol, kind, arguments):
    family, member = b.EXT_INSTANCE_MEMBER_BY_SYMBOL[symbol]
    pair_type = dict(b.ovstage_ext_instances_t._fields_)[family]
    table_type = dict(pair_type._fields_)["vtable"]._type_
    member_type = dict(table_type._fields_)[member]
    received = []

    def record(_context, identity, *args):
        received.append(identity)
        return b.OVSTAGE_OK

    callback = member_type(record)
    table = table_type(**{member: callback})
    pair = pair_type(ctypes.pointer(table), None)
    instance = b.ovstage_instance_t()
    setattr(instance.ext_instances, family, pair)
    api = b.instance_api(None)
    identity = kind(2**63 + 7)
    for receiver in (ctypes.pointer(pair), ctypes.pointer(instance)):
        assert getattr(api, symbol)(receiver, identity, *arguments) == b.OVSTAGE_OK
        assert received[-1] == identity
        assert type(received[-1]) is kind
        for wrong in (7, b.ovstage_query_handle_t(7)):
            with pytest.raises((TypeError, ctypes.ArgumentError)):
                getattr(api, symbol)(receiver, wrong, *arguments)
    assert len(received) == 2
