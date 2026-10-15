# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage change-feed test: what a per-tick consumer sees from
# ordinal-range reads on a whole-stage filter query it retains across ticks. A
# target-only tick (sealed Scope.ALL or Scope.INCLUDE) yields an empty structural
# feed, an INCLUDE seal moves only the written column's floor, and a retained
# query keeps its membership while a fresh one sees a new prim. CPU-only; no
# population bridge: the structural built-ins (usd-prim-type, usd-schemas) are
# written through the public write API, as a replication layer would. Tested
# source of the empty-change-range, include-seal-change-range and
# frozen-vs-fresh-query snippets referenced by the application-flow skill.

from types import SimpleNamespace

import numpy as np
import pytest

from ovstage import ErrorCode, Filter, FilterOp, OrdinalRange, OvstageError, PathDictionary, Predicate, Scope

CONTROL_PATHS = ["/World/Controls/Joint0", "/World/Controls/Joint1", "/World/Controls/Joint2"]
BACKGROUND_PATHS = ["/World/Background/Cube0", "/World/Background/Cube1"]
NEW_CONTROL_PATH = "/World/Controls/Joint3"


def _write_tokens(stage, query, attribute, value, count, *, is_array, ordinal):
    """Write one token id to every prim of `query`: a fixed column (usd-prim-type,
    one row per prim) or a ragged column (usd-schemas, one single-element row per prim)."""
    if is_array:
        tensors = [np.array([value], np.uint64) for _ in range(count)]
    else:
        tensors = np.full(count, value, np.uint64)
    stage.write_attribute(query, attribute, ordinal=ordinal, tensors=tensors, is_array=is_array).wait()


def _write_floats(stage, query, attribute, values, ordinal):
    stage.write_attribute(
        query, attribute, ordinal=ordinal, tensors=np.array(values, np.float32), is_array=False
    ).wait()


def _whole_stage_query(stage):
    """Every prim on the stage: HAS(usd-path), the built-in every prim carries."""
    return stage.query(filter=Filter([Predicate("usd-path", FilterOp.HAS)])).wait()


def _floor(stage, attribute):
    """The sealed floor of one attribute, as the stage reports it."""
    with stage.get_attribute_write_floor(attribute) as floor:
        return floor.wait().fetch()


def _change_rows(stage, paths, query, attrs, first, last):
    """Rows (attribute, prim path, value) a range read over [first, last] selects, sorted.

    Returns (ErrorCode, rows). A read rejected for an unsealed selected change
    reports its typed status -- ErrorCode.WRITE_FLOOR_VIOLATION -- and no rows.
    Array columns transport one tensor per covered prim; fixed columns one tensor
    with a row per prim.
    """
    read = stage.read_attributes(query, attrs, OrdinalRange.between(first, last))
    with read:
        try:
            read.wait()
        except OvstageError as exc:
            return ErrorCode(exc.code), []
        rows = []
        for group in read.groups():
            with group:
                assert not group.is_delete
                list_paths = paths.get_path_strings(group.prim_list)
                for i in range(group.prim_count):
                    row = group.data_row_index(i) if group.has_data_index_map else i
                    value = group.array(row)[0] if group.is_array else group.array(0)[row]
                    rows.append((group.attribute, list_paths[group.prim_index(i)], value.item()))
    return ErrorCode.OK, sorted(rows)


def _control_rows(attribute, values):
    return sorted(zip([attribute] * len(CONTROL_PATHS), CONTROL_PATHS, values))


@pytest.fixture
def scene(stage):
    """Baseline tick 1 -- three typed control prims (Xform + PhysicsRigidBodyAPI)
    carrying two float columns, plus two unrelated typed prims (Cube +
    MaterialBindingAPI) in a second attribute layout -- sealed for every column,
    and the consumer's whole-stage query, created once and retained across ticks."""
    with PathDictionary(stage) as paths:
        s = SimpleNamespace(
            stage=stage,
            paths=paths,
            target=paths.intern_token("ctrl:target"),
            stiffness=paths.intern_token("ctrl:stiffness"),
            prim_type=paths.intern_token("usd-prim-type"),
            schemas=paths.intern_token("usd-schemas"),
            xform=paths.intern_token("Xform"),
            cube=paths.intern_token("Cube"),
            rigid_body=paths.intern_token("PhysicsRigidBodyAPI"),
            material_binding=paths.intern_token("MaterialBindingAPI"),
        )
        with paths.create_path_list_from_strings(CONTROL_PATHS) as control_list, \
                paths.create_path_list_from_strings(BACKGROUND_PATHS) as background_list:
            s.control = stage.query_from_path_list(control_list)
            background = stage.query_from_path_list(background_list)
            try:
                _write_tokens(stage, s.control, s.prim_type, s.xform, 3, is_array=False, ordinal=1)
                _write_tokens(stage, s.control, s.schemas, s.rigid_body, 3, is_array=True, ordinal=1)
                _write_floats(stage, s.control, s.target, [0.0, 0.0, 0.0], ordinal=1)
                _write_floats(stage, s.control, s.stiffness, [10.0, 20.0, 30.0], ordinal=1)
                _write_tokens(stage, background, s.prim_type, s.cube, 2, is_array=False, ordinal=1)
                _write_tokens(stage, background, s.schemas, s.material_binding, 2, is_array=True, ordinal=1)
                stage.advance_write_floor(ordinal=1).wait()

                with _whole_stage_query(stage) as whole_stage:
                    assert whole_stage.result().total_prim_count == 5
                    s.whole_stage = whole_stage
                    yield s
            finally:
                stage.release_query(background).wait()
                stage.release_query(s.control).wait()


def test_target_only_tick_sealed_all_yields_empty_structural_range(scene):
    """A target-only tick sealed for every column: the structural feed is empty
    and the value feed carries exactly the written column."""
    stage, paths = scene.stage, scene.paths
    _write_floats(stage, scene.control, scene.target, [0.5, 1.5, 2.5], ordinal=2)
    stage.advance_write_floor(ordinal=2).wait()

    # [snippet:empty-change-range]
    # Tick 2 wrote only `ctrl:target` on the control prims and sealed every
    # column (Scope.ALL). The structural feed over exactly that tick --
    # usd-schemas and usd-prim-type read over [2, 2] on the whole-stage
    # HAS(usd-path) query retained from earlier ticks -- is a successful read
    # with zero groups: nothing structural changed, and a range that selects no
    # change is not an error.
    status, structural = _change_rows(
        stage, paths, scene.whole_stage, [scene.schemas, scene.prim_type], 2, 2
    )
    assert status == ErrorCode.OK
    assert structural == []
    # [/snippet:empty-change-range]

    # The value feed for the same tick on the retained control query carries
    # exactly the written column: one row per control prim for ctrl:target and
    # none for ctrl:stiffness, which did not change at 2.
    status, values = _change_rows(stage, paths, scene.control, [scene.target, scene.stiffness], 2, 2)
    assert status == ErrorCode.OK
    assert values == _control_rows(scene.target, [0.5, 1.5, 2.5])


def test_include_seal_of_target_only_tick_moves_only_that_floor(scene):
    """A target-only tick sealed with Scope.INCLUDE moves only the written
    column's floor; the value feed is readable and the structural feed stays empty."""
    stage, paths = scene.stage, scene.paths
    _write_floats(stage, scene.control, scene.target, [0.25, 0.5, 0.75], ordinal=2)

    # [snippet:include-seal-change-range]
    # A tick that wrote nothing structural may seal just the column it wrote.
    # Scope.INCLUDE on ctrl:target moves that attribute's floor to 2 and leaves
    # every other column -- including usd-prim-type and usd-schemas -- at the
    # previous Scope.ALL seal. The value feed over [2, 2] carries the new rows;
    # the structural feed over the same tick is still a clean zero-group read.
    stage.advance_write_floor(ordinal=2, scope=Scope.INCLUDE, attributes=[scene.target]).wait()
    assert _floor(stage, scene.target) == 2
    assert _floor(stage, scene.stiffness) == 1
    assert _floor(stage, scene.prim_type) == 1
    assert _floor(stage, scene.schemas) == 1

    status, values = _change_rows(stage, paths, scene.control, [scene.target], 2, 2)
    assert status == ErrorCode.OK
    assert values == _control_rows(scene.target, [0.25, 0.5, 0.75])

    status, structural = _change_rows(
        stage, paths, scene.whole_stage, [scene.schemas, scene.prim_type], 2, 2
    )
    assert status == ErrorCode.OK
    assert structural == []
    # [/snippet:include-seal-change-range]


def test_retained_query_keeps_membership_while_fresh_query_sees_new_prim(scene):
    """A new prim enters a fresh query's feed but not a retained query's."""
    stage, paths = scene.stage, scene.paths
    structural = [scene.schemas, scene.prim_type]
    with paths.create_path_list_from_strings([NEW_CONTROL_PATH]) as new_list:
        new_query = stage.query_from_path_list(new_list)
        try:
            # Tick 2 creates a fourth control prim (type, schema, target) and
            # seals every column.
            _write_tokens(stage, new_query, scene.prim_type, scene.xform, 1, is_array=False, ordinal=2)
            _write_tokens(stage, new_query, scene.schemas, scene.rigid_body, 1, is_array=True, ordinal=2)
            _write_floats(stage, new_query, scene.target, [4.0], ordinal=2)
            stage.advance_write_floor(ordinal=2).wait()

            # [snippet:frozen-vs-fresh-query]
            # A filter query keeps the membership it resolved when it ran. The
            # query retained from before tick 2 still covers five prims, and its
            # structural feed over [2, 2] is empty: the new prim is outside its
            # membership. A fresh HAS(usd-path) query resolves against the latest
            # committed state -- six prims -- and the same range read reports the
            # new prim's type and schema rows.
            assert scene.whole_stage.result().total_prim_count == 5
            status, retained_rows = _change_rows(stage, paths, scene.whole_stage, structural, 2, 2)
            assert status == ErrorCode.OK
            assert retained_rows == []

            with _whole_stage_query(stage) as fresh:
                assert fresh.result().total_prim_count == 6
                status, fresh_rows = _change_rows(stage, paths, fresh, structural, 2, 2)
                assert status == ErrorCode.OK
                assert fresh_rows == sorted([
                    (scene.prim_type, NEW_CONTROL_PATH, scene.xform),
                    (scene.schemas, NEW_CONTROL_PATH, scene.rigid_body),
                ])
            # [/snippet:frozen-vs-fresh-query]
        finally:
            stage.release_query(new_query).wait()


def test_include_seal_leaves_structural_changes_unreadable_until_covered(scene):
    """Sealing a tick that also wrote structure with Scope.INCLUDE leaves that
    change above its column's floor: a same-tick range read that selects it is a write-floor
    violation until a seal covering the structural columns lands."""
    stage, paths = scene.stage, scene.paths
    structural = [scene.schemas, scene.prim_type]
    with paths.create_path_list_from_strings([NEW_CONTROL_PATH]) as new_list:
        new_query = stage.query_from_path_list(new_list)
        try:
            # Tick 2 updates the targets and creates a prim, but seals only the targets.
            _write_floats(stage, scene.control, scene.target, [1.0, 2.0, 3.0], ordinal=2)
            _write_tokens(stage, new_query, scene.prim_type, scene.xform, 1, is_array=False, ordinal=2)
            _write_tokens(stage, new_query, scene.schemas, scene.rigid_body, 1, is_array=True, ordinal=2)
            stage.advance_write_floor(ordinal=2, scope=Scope.INCLUDE, attributes=[scene.target]).wait()

            # The value feed is readable: ctrl:target is sealed at 2.
            status, values = _change_rows(stage, paths, scene.control, [scene.target], 2, 2)
            assert status == ErrorCode.OK
            assert values == _control_rows(scene.target, [1.0, 2.0, 3.0])

            with _whole_stage_query(stage) as fresh:
                assert fresh.result().total_prim_count == 6
                # The new prim's type/schema change at 2 sits above the structural
                # floors (still 1): the structural feed is rejected rather than served.
                status, rows = _change_rows(stage, paths, fresh, structural, 2, 2)
                assert status == ErrorCode.WRITE_FLOOR_VIOLATION
                assert rows == []  # documents that no partial rows are returned on rejection

                # A seal covering the structural columns makes the same read succeed.
                stage.advance_write_floor(ordinal=2).wait()
                status, rows = _change_rows(stage, paths, fresh, structural, 2, 2)
                assert status == ErrorCode.OK
                assert rows == sorted([
                    (scene.prim_type, NEW_CONTROL_PATH, scene.xform),
                    (scene.schemas, NEW_CONTROL_PATH, scene.rigid_body),
                ])
        finally:
            stage.release_query(new_query).wait()
