# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage attribute-semantics test: a semantic (the authored USD meaning of
# a column's bytes) round-trips through the write→read cycle, orthogonal to the
# storage dtype; TOKEN_ID pins uint64 storage carrying pre-interned token ids.
# CPU-only. The write-flavors example is the workflow tour; this file asserts it.

from types import SimpleNamespace

import numpy as np
import pytest

from ovstage import (AttributeSemantic, DLDataType, DLDataTypeCode, OrdinalRange, PathDictionary,
                     ReadGroup, make_dltensor)

FLOAT3 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)
MAT4 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=64, lanes=16)
UINT64X2 = DLDataType(code=DLDataTypeCode.kDLUInt, bits=64, lanes=2)


def _semantic_payload(paths, semantic):
    """Return one valid single-prim payload for each public semantic."""
    if semantic == AttributeSemantic.STRING:
        return [np.frombuffer(b"text", dtype=np.uint8)], True
    if semantic == AttributeSemantic.ASSET_PATH_ID:
        values = np.array([paths.intern_token("authored.png"), paths.intern_token("resolved.png")], np.uint64)
        return make_dltensor(values, dtype=UINT64X2, shape=[1]), False
    if semantic == AttributeSemantic.CONNECTION_PATH_ID:
        values = np.array([paths.intern_path("/World/Source"), paths.intern_token("outputs:value")], np.uint64)
        return make_dltensor(values, dtype=UINT64X2, shape=[1]), False
    if semantic == AttributeSemantic.RELATIONSHIP_PATH_ID:
        return [np.array([paths.intern_path("/World/Target")], np.uint64)], True
    if semantic in (AttributeSemantic.TOKEN_ID, AttributeSemantic.PATH_EXPRESSION_STRING):
        return np.array([paths.intern_token("value")], np.uint64), False

    lanes = {
        AttributeSemantic.POINT: 3,
        AttributeSemantic.VECTOR: 3,
        AttributeSemantic.NORMAL: 3,
        AttributeSemantic.COLOR: 3,
        AttributeSemantic.QUATERNION: 4,
        AttributeSemantic.MATRIX: 16,
        AttributeSemantic.TEXTURE_COORDINATE: 2,
        AttributeSemantic.FRAME: 16,
    }.get(semantic, 1)
    numpy_dtype = np.float64 if semantic in (
        AttributeSemantic.MATRIX,
        AttributeSemantic.TIME_CODE,
        AttributeSemantic.FRAME,
    ) else np.float32
    dtype = DLDataType(code=DLDataTypeCode.kDLFloat, bits=np.dtype(numpy_dtype).itemsize * 8, lanes=lanes)
    return make_dltensor(np.zeros(lanes, dtype=numpy_dtype), dtype=dtype, shape=[1]), False


def _read_one_group(stage, query, attr, end_ordinal):
    read = stage.read_attributes(query, [attr], OrdinalRange.latest(end_ordinal))
    read.wait()
    return read, read.fetch_next()


def test_semantic_roles_round_trip(stage):
    with PathDictionary(stage) as paths:
        plist = paths.create_path_list_from_strings(["/World/Semantics/A", "/World/Semantics/B"])
        query = stage.query_from_path_list(plist)
        try:
            # [snippet:semantic-roles]
            # A semantic is the authored USD interpretation of a column's bytes,
            # orthogonal to the storage dtype: the write stamps it at creation and
            # reads recover it (group.semantic). The same 3-lane float32 storage
            # is POINT on one column and COLOR on another; a 16-lane float64 is
            # MATRIX. TOKEN_ID differs: it pins uint64 storage and the payload must
            # be pre-interned path-dictionary token ids — ovstage never stringifies.
            steel = paths.intern_token("steel")
            rubber = paths.intern_token("rubber")
            stage.write_attribute(query, "points", ordinal=1, is_array=False,
                                  tensors=make_dltensor(np.array([0, 0, 1, 0, 1, 0], np.float32), dtype=FLOAT3,
                                                        shape=[2]), semantic=AttributeSemantic.POINT).wait()
            stage.write_attribute(query, "display-color", ordinal=1, is_array=False,
                                  tensors=make_dltensor(np.array([1, 0, 0, 0, 0.5, 0], np.float32), dtype=FLOAT3,
                                                        shape=[2]), semantic=AttributeSemantic.COLOR).wait()
            stage.write_attribute(query, "local-matrix", ordinal=1, is_array=False,
                                  tensors=make_dltensor(np.tile(np.eye(4, dtype=np.float64).ravel(), 2), dtype=MAT4,
                                                        shape=[2]), semantic=AttributeSemantic.MATRIX).wait()
            stage.write_attribute(query, "material", ordinal=1, is_array=False,
                                  tensors=np.array([steel, rubber], np.uint64),
                                  semantic=AttributeSemantic.TOKEN_ID).wait()
            stage.advance_write_floor(ordinal=1).wait()
            # [/snippet:semantic-roles]

            for name, expected in (("points", AttributeSemantic.POINT),
                                   ("display-color", AttributeSemantic.COLOR),
                                   ("local-matrix", AttributeSemantic.MATRIX),
                                   ("material", AttributeSemantic.TOKEN_ID)):
                attr = paths.intern_token(name)
                read, group = _read_one_group(stage, query, attr, 1)
                assert group is not None, name
                assert group.semantic == expected, name
                if name == "local-matrix":
                    assert group.tensor(0).dtype.lanes == 1
                    assert group.tensor(0).shape_tuple == (2, 4, 4)
                if name == "material":
                    resolved = [paths.token_to_string(int(t)) for t in np.asarray(group.array(0))]
                    assert set(resolved) == {"steel", "rubber"}  # order-independent
                stage.release_group(group)
                read.release().wait()
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


def test_connection_path_ids_round_trip_for_single_and_multi_prim_reads(stage):
    with PathDictionary(stage) as paths:
        target_names = ["/World/Connection/A", "/World/Connection/B"]
        target_ids = [paths.intern_path(path) for path in target_names]
        source_ids = [paths.intern_path("/World/Source/A"), paths.intern_path("/World/Source/B")]
        source_attr = paths.intern_token("outputs:value")
        connection_attr = paths.intern_token("inputs:source")
        expected = {
            target_names[0]: (source_ids[0], source_attr),
            target_names[1]: (source_ids[1], source_attr),
        }

        multi_plist = paths.create_path_list(target_ids)
        single_plist = paths.create_path_list([target_ids[0]])
        multi_query = stage.query_from_path_list(multi_plist)
        single_query = stage.query_from_path_list(single_plist)
        try:
            payload = np.array([expected[path] for path in target_names], dtype=np.uint64)
            tensor = make_dltensor(payload.reshape(-1), dtype=UINT64X2, shape=[len(target_names)])
            stage.write_attribute(multi_query, connection_attr, ordinal=1, is_array=False,
                                  tensors=tensor,
                                  semantic=AttributeSemantic.CONNECTION_PATH_ID).wait()
            stage.advance_write_floor(ordinal=1).wait()

            for query, expected_rows in (
                (single_query, {target_names[0]: expected[target_names[0]]}),
                (multi_query, expected),
            ):
                read, group = _read_one_group(stage, query, connection_attr, 1)
                assert group is not None
                assert not group.is_array
                assert group.semantic == AttributeSemantic.CONNECTION_PATH_ID
                dtype = group.tensor(0).dtype
                assert (dtype.code, dtype.bits, dtype.lanes) == (DLDataTypeCode.kDLUInt, 64, 1)
                assert group.tensor(0).shape_tuple[-1] == 2

                list_paths = paths.get_path_strings(group.prim_list)
                values = np.asarray(group.array(0)).reshape(-1, 2)
                actual = {}
                for local in range(group.prim_count):
                    pair = values[group.data_row_index(local)]
                    actual[list_paths[group.prim_index(local)]] = (int(pair[0]), int(pair[1]))
                assert actual == expected_rows

                stage.release_group(group)
                assert read.fetch_next() is None
                read.release().wait()
        finally:
            stage.release_query(single_query).wait()
            stage.release_query(multi_query).wait()
            paths.destroy_path_list(single_plist)
            paths.destroy_path_list(multi_plist)


def test_single_path_relationship_round_trips(stage):
    # RELATIONSHIP_PATH_ID with is_array=False holds exactly one path per prim; the read
    # reports the same semantic and shape, one path id per row.
    with PathDictionary(stage) as paths:
        names = ["/World/Link/A", "/World/Link/B"]
        expected = {names[0]: paths.intern_path("/World/Target/A"), names[1]: paths.intern_path("/World/Target/B")}
        link = paths.intern_token("link")
        plist = paths.create_path_list_from_strings(names)
        query = stage.query_from_path_list(plist)
        try:
            stage.write_attribute(query, link, ordinal=1, is_array=False,
                                  tensors=np.array([expected[name] for name in names], np.uint64),
                                  semantic=AttributeSemantic.RELATIONSHIP_PATH_ID).wait()
            stage.advance_write_floor(ordinal=1).wait()

            read, group = _read_one_group(stage, query, link, 1)
            assert group is not None
            assert not group.is_array
            assert group.semantic == AttributeSemantic.RELATIONSHIP_PATH_ID
            list_paths = paths.get_path_strings(group.prim_list)
            values = np.asarray(group.array(0)).reshape(-1)
            actual = {list_paths[group.prim_index(local)]: int(values[group.data_row_index(local)])
                      for local in range(group.prim_count)}
            assert actual == expected
            stage.release_group(group)
            assert read.fetch_next() is None
            read.release().wait()
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


@pytest.mark.parametrize("semantic", list(AttributeSemantic), ids=lambda semantic: semantic.name)
def test_read_group_semantic_round_trips_every_public_value(stage, semantic):
    with PathDictionary(stage) as paths:
        with paths.create_path_list_from_strings([f"/World/Semantics/{semantic.name}"]) as plist:
            query = stage.query_from_path_list(plist)
            try:
                tensors, is_array = _semantic_payload(paths, semantic)
                attribute = f"test:semantic:{semantic.name.lower()}"
                attribute_token = paths.intern_token(attribute)
                stage.write_attribute(
                    query,
                    attribute_token,
                    ordinal=1,
                    tensors=tensors,
                    is_array=is_array,
                    semantic=semantic,
                ).wait()
                stage.advance_write_floor(ordinal=1).wait()

                with stage.read_attributes(query, [attribute_token], OrdinalRange.latest(1)) as read:
                    group = read.wait().fetch_next()
                    assert group is not None
                    with group:
                        assert group.semantic == semantic
            finally:
                stage.release_query(query).wait()


def test_read_group_semantic_preserves_unknown_native_value():
    unknown = max(semantic.value for semantic in AttributeSemantic) + 1
    group = ReadGroup(SimpleNamespace(semantic=unknown))
    assert group.semantic == unknown
    assert type(group.semantic) is int
