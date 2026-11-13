# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
"""Signed representability checks run without a native runtime; stage checks need it."""

import ctypes

import numpy as np
import pytest

from ovstage import ORDINAL_MAX as MAX, ORDINAL_MIN as MIN
from ovstage import ErrorCode, OrdinalRange, OvstageError, PathDictionary, population
from ovstage._src import bindings as b
from ovstage._src.types import check_ordinal

def test_public_ordinal_bounds():
    import ovstage

    assert MIN == ovstage.OVSTAGE_ORDINAL_MIN == b.OVSTAGE_ORDINAL_MIN == -(1 << 63)
    assert MAX == ovstage.OVSTAGE_ORDINAL_MAX == b.OVSTAGE_ORDINAL_MAX == (1 << 63) - 1


@pytest.mark.parametrize("value", [MIN, MIN + 1, -1, 0, 1, MAX])
def test_signed_ordinal_ctypes_roundtrip(value):
    assert check_ordinal(np.int64(value)) == value
    assert b.ovstage_ordinal_t(value).value == value
    raw = OrdinalRange.between(value, value).to_c()
    assert raw.start_ordinal == raw.end_ordinal == value
    assert raw.has_start_ordinal
    assert ctypes.sizeof(b.ovstage_ordinal_t) == 8


@pytest.mark.parametrize("value", [MIN - 1, MAX + 1, (1 << 64) - 1])
def test_signed_ordinal_bounds(value):
    with pytest.raises(ValueError, match="int64"):
        check_ordinal(value)
    with pytest.raises(ValueError, match="int64"):
        OrdinalRange.latest(value).to_c()


@pytest.mark.parametrize("value", [1.0, -0.5, "0", None])
def test_signed_ordinal_requires_integer(value):
    with pytest.raises(TypeError, match="ordinal"):
        check_ordinal(value)


def _floor(stage, attribute=None):
    with stage.get_attribute_write_floor(attribute) as query:
        return query.wait().fetch()


def test_signed_write_seal_read(stage):
    assert _floor(stage) == MIN
    with stage.get_oldest_preserved_ordinal() as query:
        assert query.wait().fetch() == MIN
    with PathDictionary(stage) as paths:
        prims = paths.create_path_list_from_strings(["/Signed"])
        value = paths.intern_token("value")
        try:
            with stage.query_from_path_list(prims) as query:
                with stage.read_attributes(query, [value], OrdinalRange.latest(MIN)) as read:
                    assert read.wait().fetch_next() is None
                def write(ordinal):
                    stage.write_attribute(query, value, ordinal, np.array([3.0], np.float32), is_array=False).wait()
                with pytest.raises(OvstageError) as exc:
                    write(MIN)
                assert exc.value.code in (ErrorCode.WRITE_FLOOR_VIOLATION, ErrorCode.OP_FAILED)
                for ordinal in [MIN + 1, -2, -1, 0, 1, MAX]:
                    write(ordinal)
                    stage.advance_write_floor(ordinal).wait()
                    assert _floor(stage) == ordinal
                    with stage.read_attributes(query, [value], OrdinalRange.latest(ordinal)) as read:
                        group = read.wait().fetch_next()
                        assert group is not None
                        try:
                            assert group.columns[0].ordinal == ordinal
                            np.testing.assert_array_equal(group.columns[0].array(0), [3.0])
                        finally:
                            stage.release_group(group)
                    with pytest.raises(OvstageError) as exc:
                        write(ordinal)
                    assert exc.value.code in (ErrorCode.WRITE_FLOOR_VIOLATION, ErrorCode.OP_FAILED)
                    stage.advance_write_floor(MIN).wait()
                    assert _floor(stage) == ordinal
        finally:
            paths.destroy_path_list(prims)


@pytest.mark.parametrize("ordinal", [MIN + 1, -1, 0])
def test_population_at_signed_ordinal(stage, ordinal):
    if not population.available():
        pytest.skip("population bridge unavailable")
    population.open_usd_from_string(stage, '#usda 1.0\ndef Xform "World" {}', ordinal=ordinal)
    assert _floor(stage) == MIN
    stage.advance_write_floor(ordinal).wait()
    assert _floor(stage) == ordinal


def test_scoped_negative_floors_and_retention(stage):
    from ovstage import Scope

    with PathDictionary(stage) as paths:
        prims = paths.create_path_list_from_strings(["/Signed"])
        a, b = paths.intern_token("a"), paths.intern_token("b")
        try:
            with stage.query_from_path_list(prims) as query:
                for attribute in (a, b):
                    stage.write_attribute(query, attribute, -10, np.array([1.0], np.float32), is_array=False).wait()
                stage.advance_write_floor(-9, scope=Scope.INCLUDE, attributes=[a]).wait()
                assert _floor(stage, a) == -9
                assert _floor(stage, b) == MIN
                assert _floor(stage) == MIN
                stage.advance_write_floor(-20, scope=Scope.INCLUDE, attributes=[a]).wait()
                assert _floor(stage, a) == -9
                stage.advance_write_floor(-9, scope=Scope.EXCLUDE, attributes=[a]).wait()
                assert _floor(stage, b) == -9
                assert _floor(stage) == -9
                for ordinal in range(-8, 3):
                    stage.write_attribute(query, a, ordinal, np.array([float(ordinal)], np.float32), is_array=False).wait()
                    stage.advance_write_floor(ordinal).wait()
                with stage.get_oldest_preserved_ordinal() as retained:
                    oldest = retained.wait().fetch()
                assert -8 < oldest <= 2
                with stage.read_attributes(query, [a], OrdinalRange.between(2, 2)) as read:
                    group = read.wait().fetch_next()
                    assert group is not None
                    try:
                        np.testing.assert_array_equal(group.columns[0].array(0), [2.0])
                    finally:
                        stage.release_group(group)
                pattern = paths.intern_token("a*")
                with pytest.raises(OvstageError) as exc:
                    with stage.read_attributes(query, [pattern], OrdinalRange.between(-10, -10)) as read:
                        read.wait()
                assert exc.value.code == ErrorCode.OUT_OF_RANGE
        finally:
            paths.destroy_path_list(prims)
