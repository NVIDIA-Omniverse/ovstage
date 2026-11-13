# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.
#
# Public ovstage 0.2 -> 0.3 upgrade-guide contract tests. Each test backs one claim
# of docs/guides/upgrading_from_0_2.rst that is observable from the Python API, so
# the guide's instructions break loudly if the 0.3 behavior drifts. CPU-only.
# Guide claims that are C-only (ovalign.h, ovstage_get_path_dictionary, initialize
# keys, etc.) are covered by the C suite or not at all; see the guide.

import ctypes
import inspect
import math

import numpy as np
import pytest

from ovstage import (AttributeSemantic, DLDataType, DLDataTypeCode, ErrorCode, Filter, FilterOp,
                     OrdinalRange, OvstageError, PathDictionary, Predicate,
                     Stage, make_dltensor, population)
from ovstage._src import bindings as _bindings
from ovstage._src import population_export_bindings as _export_bindings

UINT64X2 = DLDataType(code=DLDataTypeCode.kDLUInt, bits=64, lanes=2)

needs_population = pytest.mark.skipif(
    not population.available(),
    reason="libovstage was built without the ovstage population bridge",
)

CUBE_A_USDA = """#usda 1.0
def Cube "MergeA"
{
    double size = 1.0
}
"""

CUBE_B_USDA = """#usda 1.0
def Cube "MergeB"
{
    double size = 2.0
}
"""

# A typeless container (`def "Untyped"`) with a typed child, to compare the two.
TYPELESS_USDA = """#usda 1.0
def "Untyped"
{
    def Cube "Child"
    {
        double size = 1.0
    }
}
"""


def _count(stage, *predicates) -> int:
    with stage.query(filter=Filter(list(predicates))) as query:
        query.wait()
        return query.result().total_prim_count


def _at(path: str) -> Predicate:
    return Predicate("usd-path", FilterOp.IN, [path])


def _populate_with_selectors(stage, usda: str, ordinal: int = 1) -> None:
    """Populate every prim and property through a selector, with no RENDERING domain."""
    selector = population.Selector(
        prim_predicate=population.PrimPredicate.all(),
        property_predicate=population.PropertyPredicate.all(),
    )
    population.open_usd_from_string_with_desc(
        stage, usda, ordinal=ordinal, time=population.DEFAULT_TIME,
        descs=[population.Desc(selectors=[selector])],
    )


# ── Breaking changes ─────────────────────────────────────────────────────────


def test_path_dictionary_requires_a_stage(ovstage_mod, stage):
    """Upgrade guide, Breaking changes: PathDictionary requires a Stage."""
    with pytest.raises(TypeError):
        ovstage_mod.PathDictionary()
    with pytest.raises(TypeError):
        ovstage_mod.PathDictionary(None)
    with pytest.raises(TypeError):
        ovstage_mod.PathDictionary(stage=None)

    # The 0.3 form works, and the wrapper borrows the Stage's dictionary.
    with ovstage_mod.PathDictionary(stage) as paths:
        assert paths.token_to_string(paths.intern_token("upgrade-0-3")) == "upgrade-0-3"
    assert stage.get_path_dictionary() is not None  # the Stage outlives the wrapper


_TIME_ENTRY_POINTS = [
    "open_usd",
    "open_usd_from_string",
    "open_usd_with_desc",
    "open_usd_from_string_with_desc",
    "update_from_usd_time",
    "open_usd_async",
    "open_usd_from_string_async",
    "open_usd_with_desc_async",
    "open_usd_from_string_with_desc_async",
    "update_from_usd_time_async",
]


@pytest.mark.parametrize("name", _TIME_ENTRY_POINTS)
def test_time_code_keyword_is_renamed_time(stage, name):
    """Upgrade guide, Breaking changes: time_code renamed time (no alias, TypeError)."""
    fn = getattr(population, name)
    parameters = inspect.signature(fn).parameters
    assert "time" in parameters
    assert "time_code" not in parameters

    kwargs = {"ordinal": 1, "time": 1.0, "time_code": 1.0}
    if "with_desc" in name:
        kwargs["descs"] = []
    # The signature rejects the removed keyword before any native call is made.
    args = (stage,) if name.startswith("update_from") else (stage, "unused-source")
    with pytest.raises(TypeError, match="time_code"):
        fn(*args, **kwargs)


@needs_population
def test_open_accepts_time_by_keyword_and_position(stage):
    """Upgrade guide, Breaking changes: time= works and positional calls are unaffected."""
    population.open_usd_from_string(stage, CUBE_A_USDA, ordinal=1, time=1.0)
    assert _count(stage, _at("/MergeA")) == 1
    population.open_usd_from_string(stage, CUBE_B_USDA, 2, 1.0)  # ordinal, time positionally
    assert _count(stage, _at("/MergeB")) == 1


def test_untyped_type_name_is_not_exposed(ovstage_mod):
    """Upgrade guide, Breaking changes: untyped prim type removed (no symbol in 0.3)."""
    for namespace in (ovstage_mod, population, _bindings):
        exposed = [name for name in dir(namespace) if "untyped" in name.lower()]
        assert not exposed, f"{namespace.__name__} still exposes {exposed}"
    # The native library does not export the C entry point either.
    assert not hasattr(_bindings.load(), "ovstage_population_untyped_type_name")


@needs_population
def test_typeless_prim_populates_without_usd_prim_type(stage):
    """Upgrade guide, Breaking changes: a typeless prim is populated and has no usd-prim-type."""
    _populate_with_selectors(stage, TYPELESS_USDA)
    with PathDictionary(stage) as paths:
        has_type = Predicate(paths.intern_token("usd-prim-type"), FilterOp.HAS)

        assert _count(stage, _at("/Untyped")) == 1  # still populated
        assert _count(stage, _at("/Untyped"), has_type) == 0  # but carries no usd-prim-type
        assert _count(stage, _at("/Untyped/Child"), has_type) == 1  # a typed prim does


def _write_connections(stage, paths, query, attribute, ordinal, pair_count):
    pair = [paths.intern_path("/World/Source"), paths.intern_token("outputs:value")]
    values = np.array([handle.raw_id for handle in pair] * pair_count, dtype=np.uint64)
    tensor = make_dltensor(values, dtype=UINT64X2, shape=[pair_count])
    return stage.write_attribute(
        query, attribute, ordinal=ordinal, tensors=tensor, is_array=False,
        semantic=AttributeSemantic.CONNECTION_PATH_ID,
    )


def test_connection_write_requires_one_pair_per_prim(stage):
    """Upgrade guide, Breaking changes: connection writes no longer broadcast one pair."""
    with PathDictionary(stage) as paths:
        plist = paths.create_path_list_from_strings(["/World/Conn/A", "/World/Conn/B"])
        query = stage.query_from_path_list(plist)
        try:
            attribute = paths.intern_token("inputs:source")
            # 0.3: N pairs for N prims succeeds.
            _write_connections(stage, paths, query, attribute, ordinal=1, pair_count=2).wait()
            stage.advance_write_floor(ordinal=1).wait()

            # 0.2 broadcast form: 1 pair for 2 prims is rejected during preflight
            # or execution, never silently broadcast. Python wait() wraps execution
            # failures as OP_FAILED; submission validation keeps INVALID_ARGUMENT.
            with pytest.raises(OvstageError) as excinfo:
                _write_connections(stage, paths, query, attribute, ordinal=2, pair_count=1).wait()
            assert excinfo.value.code in (ErrorCode.INVALID_ARGUMENT, ErrorCode.OP_FAILED)
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


# ── Behavior changes ─────────────────────────────────────────────────────────


@needs_population
def test_open_merges_instead_of_clearing(stage):
    """Upgrade guide, Behavior changes: open merges instead of clearing."""
    population.open_usd_from_string(stage, CUBE_A_USDA, ordinal=1)
    population.open_usd_from_string(stage, CUBE_B_USDA, ordinal=2)

    assert _count(stage, _at("/MergeA")) == 1  # the first source's prim was not removed
    assert _count(stage, _at("/MergeB")) == 1  # the second source's prim was added


@needs_population
def test_reset_apply_then_open_replaces_content(stage):
    """Upgrade guide, Behavior changes: reset_usd + apply_usd_changes below the open replaces."""
    population.open_usd_from_string(stage, CUBE_A_USDA, ordinal=1)
    assert _count(stage, _at("/MergeA")) == 1

    population.reset_usd(stage)
    population.apply_usd_changes(stage, ordinal=2)
    population.open_usd_from_string(stage, CUBE_B_USDA, ordinal=3)

    assert _count(stage, _at("/MergeA")) == 0
    assert _count(stage, _at("/MergeB")) == 1


def test_wait_returns_the_handle(stage):
    """Upgrade guide, Behavior changes: wait() returns the handle (Python)."""
    with PathDictionary(stage) as paths:
        attribute = paths.intern_token("upgrade:wait")
        plist = paths.create_path_list_from_strings(["/World/Wait/A"])
        query = stage.query_from_path_list(plist)
        try:
            write = stage.write_attribute(
                query, attribute, ordinal=1, tensors=np.array([1.0], np.float32), is_array=False
            )
            assert write.wait() is write  # Operation.wait() returns the Operation
            stage.advance_write_floor(ordinal=1).wait()
            assert query.wait() is query  # Query.wait() returns the Query

            with stage.read_attributes(query, [attribute], OrdinalRange.latest(1)) as read:
                assert read.wait() is read  # Read.wait() returns the Read
                group = read.fetch_next()  # so `read.wait().fetch_next()` chains
                assert group is not None
                with group:
                    np.testing.assert_allclose(np.array(group.columns[0].array(0)), [1.0])
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


def _population_wait_code(stage, op_id: _bindings.ovstage_population_op_id_t) -> int:
    result = _bindings.ovstage_population_op_wait_result_t()
    # The facade serves the population wait through the population family vtable of the instance.
    return int(stage._api.ovstage_population_wait_op(stage._inst, op_id, 0, ctypes.byref(result)))


@needs_population
def test_population_wait_op_rejects_foreign_and_unissued_ids(stage):
    """Upgrade guide, Behavior changes: stale or foreign op ids give INVALID_HANDLE."""
    first = population.open_usd_from_string_async(stage, CUBE_A_USDA, ordinal=1)
    first.wait()

    with Stage("test.ovstage.upgrade.other") as other:
        population.open_usd_from_string(other, CUBE_B_USDA, ordinal=1)
        # An id from another instance's operation stream, and the never-issued id 0.
        for op_id in (first.op_id, _bindings.OVSTAGE_POPULATION_INVALID_OP_ID):
            assert _population_wait_code(other, op_id) == _bindings.OVSTAGE_ERROR_INVALID_HANDLE

    # The owning instance still accepts its own id.
    assert _population_wait_code(stage, first.op_id) != _bindings.OVSTAGE_ERROR_INVALID_HANDLE


@pytest.mark.skipif(
    not population.export_available(),
    reason="libovstage was built without the population export-to-USD bridge",
)
def test_export_wait_op_rejects_an_already_consumed_id(stage, tmp_path):
    """Upgrade guide, Behavior changes: export wait returns INVALID_HANDLE for a consumed id."""
    with PathDictionary(stage) as paths:
        plist = paths.create_path_list_from_strings(["/World/Cube"])
        try:
            with stage.query_from_path_list(plist) as query:
                matrix = np.eye(4, dtype=np.float64)[None, :, :]
                stage.write_attribute(
                    query, "omni:xform", ordinal=1, tensors=matrix, is_array=False,
                    semantic=AttributeSemantic.MATRIX,
                ).wait()
        finally:
            paths.destroy_path_list(plist)
    stage.advance_write_floor(ordinal=1).wait()

    operation = population.export_to_usd_file_async(
        stage, str(tmp_path / "consumed.usda"), ordinal=1, attribute_rules=["omni:xform"]
    )
    operation.wait()  # the terminal wait consumes the export record

    report = _export_bindings.ovstage_population_export_report_t()
    code = stage._api.ovstage_population_export_wait_op(
        stage._inst, operation.op_id, 0, ctypes.byref(report)
    )
    assert int(code) == _bindings.OVSTAGE_ERROR_INVALID_HANDLE


@needs_population
def test_every_selector_populated_prim_has_usd_schemas(stage):
    """Upgrade guide, Behavior changes: every populated prim has usd-schemas (HAS matches all)."""
    _populate_with_selectors(stage, TYPELESS_USDA)
    with PathDictionary(stage) as paths:
        has_schemas = Predicate(paths.intern_token("usd-schemas"), FilterOp.HAS)

        # Neither prim applies a schema, yet HAS matches both: the set is empty, not absent.
        assert _count(stage, _at("/Untyped"), has_schemas) == 1
        assert _count(stage, _at("/Untyped/Child"), has_schemas) == 1
        assert _count(stage, Predicate("usd-schemas", FilterOp.CONTAINS, ["PhysicsRigidBodyAPI"])) == 0


def test_clone_returns_a_result_and_clone_async_a_clone_operation(ovstage_mod, stage):
    """Upgrade guide, Behavior changes and New in 0.3: clone results."""
    with PathDictionary(stage) as paths:
        attribute = paths.intern_token("upgrade:clone")
        plist = paths.create_path_list_from_strings(["/World/CloneSource"])
        query = stage.query_from_path_list(plist)
        try:
            stage.write_attribute(
                query, attribute, ordinal=1, tensors=np.array([1.0], np.float32), is_array=False
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            report = stage.clone("/World/CloneSource", ["/World/CloneA"], ordinal=2)
            assert isinstance(report, ovstage_mod.CloneResult)  # no longer None
            assert report.prims_cloned == 1

            operation = stage.clone_async("/World/CloneSource", ["/World/CloneB"], ordinal=2)
            assert isinstance(operation, ovstage_mod.CloneOperation)
            assert operation.wait() is operation
            assert isinstance(operation.result(), ovstage_mod.CloneResult)
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


# ── New in 0.3 ───────────────────────────────────────────────────────────────


def test_get_build_info_is_available_without_a_stage(ovstage_mod):
    """Upgrade guide, New in 0.3: get_build_info() and BuildInfo."""
    info = ovstage_mod.get_build_info()
    assert isinstance(info, ovstage_mod.BuildInfo)
    assert info.build_string
    assert info.git_hash


def test_stage_config_reads_back_the_captured_configuration(ovstage_mod, stage):
    """Upgrade guide, New in 0.3: Stage.config returns a StageConfig."""
    assert isinstance(ovstage_mod.Stage.config, property)
    assert isinstance(stage.config, ovstage_mod.StageConfig)


def test_default_time_is_a_nan_tested_with_is_default_time(ovstage_mod):
    """Upgrade guide, New in 0.3: DEFAULT_TIME is a NaN; use is_default_time, never ==."""
    default = ovstage_mod.population.DEFAULT_TIME
    assert math.isnan(default)
    assert default != default  # why == cannot be used
    assert ovstage_mod.population.is_default_time(default)
    assert not ovstage_mod.population.is_default_time(0.0)
    assert not ovstage_mod.population.is_default_time(1.0)


def test_export_to_usd_layer_and_stage_entry_points_exist(ovstage_mod):
    """Upgrade guide, New in 0.3: in-memory layer/stage export entry points."""
    for name in (
        "export_to_usd_layer",
        "export_to_usd_stage",
        "export_typed_hierarchy_to_usd_layer",
        "export_to_usd_layer_async",
        "export_to_usd_stage_async",
        "export_typed_hierarchy_to_usd_layer_async",
    ):
        assert callable(getattr(ovstage_mod.population, name)), name


@needs_population
def test_usd_ancestor_types_exclude_the_prims_own_type(stage):
    """Upgrade guide, New in 0.3: usd-ancestor-types lists derived types, not the own type."""
    _populate_with_selectors(stage, TYPELESS_USDA)

    def cube_with_ancestor(name):
        return _count(stage, _at("/Untyped/Child"), Predicate("usd-ancestor-types", FilterOp.CONTAINS, [name]))

    assert cube_with_ancestor("Gprim") == 1
    assert cube_with_ancestor("Xformable") == 1
    assert cube_with_ancestor("Cube") == 0


def test_read_semantic_property_exists(ovstage_mod):
    """The semantic introduced in 0.3 remains available on current read columns."""
    assert isinstance(ovstage_mod.ReadColumn.semantic, property)
