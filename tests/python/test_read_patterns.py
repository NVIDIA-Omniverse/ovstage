# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Attribute selector patterns in :meth:`ovstage.Stage.read_attributes`.

A selector is an attribute name, ``"*"``, or ``"prefix*"`` (one ``*``, as the
last character). Each read finds the matching names again. Names that contain
``*`` are reserved for read selection, so writes and other operations reject
them.
"""

import numpy as np
import pytest

from ovstage import ErrorCode, OrdinalRange, OvstageError, Scope

PRIMS = ["/World/Patterns/A", "/World/Patterns/B"]
NAMES = ["primvars:displayOpacity", "primvars:roughness", "mass"]


@pytest.fixture
def scene(ovstage_mod, stage):
    """Three float columns written at ordinal 1 and sealed, plus the query of their prims."""
    with ovstage_mod.PathDictionary(stage) as paths:
        with paths.create_path_list_from_strings(PRIMS) as plist:
            query = stage.query_from_path_list(plist)
            try:
                for name in NAMES:
                    _write(stage, query, name, 1)
                stage.advance_write_floor(ordinal=1).wait()
                yield stage, paths, query
            finally:
                stage.release_query(query).wait()


def _write(stage, query, name, ordinal, value=1.0):
    stage.write_attribute(
        query, name, ordinal=ordinal, tensors=np.full(len(PRIMS), value, np.float32), is_array=False,
    ).wait()


def _read(stage, paths, query, selectors, ordinal_range):
    """Read the selectors. Return (name, ordinal) for each group, in fetch order."""
    read = stage.read_attributes(query, [paths.intern_token(s) for s in selectors], ordinal_range)
    read.wait()
    result = []
    try:
        for group in read.groups():
            with group:
                for column in group.columns:
                    result.append((paths.token_to_string(column.attribute), column.ordinal))
    finally:
        read.release().wait()
    return result


def _names(stage, paths, query, selectors, ordinal_range):
    return [name for name, _ in _read(stage, paths, query, selectors, ordinal_range)]


def test_prefix_pattern_reads_matching_attributes(scene):
    stage, paths, query = scene
    # One more attribute on prim B only gives the two prims different
    # attributes. Thus one attribute can come back in more than one group.
    stage.write_attribute(
        query, "primvars:roughness", ordinal=2, tensors=np.array([0.25, 0.75], np.float32), is_array=False,
    ).wait()
    with paths.create_path_list_from_strings([PRIMS[1]]) as b_list:
        b_query = stage.query_from_path_list(b_list)
        try:
            stage.write_attribute(b_query, "tag", ordinal=2, tensors=np.ones(1, np.float32), is_array=False).wait()
        finally:
            stage.release_query(b_query).wait()
    stage.advance_write_floor(ordinal=2).wait()

    # [snippet:read-attribute-patterns]
    # "primvars:*" selects every attribute whose name starts with "primvars:".
    # A latest read returns the current value of each matched attribute.
    read = stage.read_attributes(query, [paths.intern_token("primvars:*")], OrdinalRange.latest(2))
    read.wait()
    matched = {}  # attribute name -> {prim path: value}
    for group in read.groups():
        with group:
            # A group may carry several columns over the same prim rows.
            group_paths = paths.get_paths(group.prim_list)
            for column in group.columns:
                # Keep every group's rows, keyed by column name and prim path.
                name = paths.token_to_string(column.attribute)
                rows = matched.setdefault(name, {})
                values = column.array(0)  # Copy values before releasing the group.
                for local in range(group.prim_count):
                    path = paths.path_to_string(group_paths[group.prim_index(local)])
                    row = column.data_row_index(local) if column.has_data_index_map else local
                    rows[path] = float(values[row])
    read.release().wait()
    # [/snippet:read-attribute-patterns]
    # The fixture must split the groups, or the test does not show that the
    # snippet keeps the rows of each group.
    assert _names(stage, paths, query, ["primvars:roughness"], OrdinalRange.latest(2)).count("primvars:roughness") > 1
    assert sorted(matched) == ["primvars:displayOpacity", "primvars:roughness"]
    assert matched["primvars:roughness"] == {PRIMS[0]: 0.25, PRIMS[1]: 0.75}
    assert matched["primvars:displayOpacity"] == {PRIMS[0]: 1.0, PRIMS[1]: 1.0}


@pytest.mark.parametrize("ordinal_range", [OrdinalRange.latest(1), OrdinalRange.between(1, 1)], ids=["latest", "range"])
def test_explicit_names_come_first_and_patterns_add_each_name_once(scene, ordinal_range):
    stage, paths, query = scene
    # Explicit names first, in caller order. Then the names that patterns add, sorted.
    assert _names(stage, paths, query, ["primvars:*", "mass"], ordinal_range) == [
        "mass", "primvars:displayOpacity", "primvars:roughness",
    ]
    # A pattern does not add a name again.
    assert _names(stage, paths, query, ["primvars:roughness", "primvars:*", "primvars:r*"], ordinal_range) == [
        "primvars:roughness", "primvars:displayOpacity",
    ]
    # Repeated explicit names are deduplicated by exact token.
    assert _names(stage, paths, query, ["mass", "mass"], ordinal_range) == ["mass"]


@pytest.mark.parametrize("ordinal_range", [OrdinalRange.latest(1), OrdinalRange.between(1, 1)], ids=["latest", "range"])
def test_unmatched_patterns_give_no_groups(scene, ordinal_range):
    stage, paths, query = scene
    assert _names(stage, paths, query, ["colors:*"], ordinal_range) == []
    # The match is case sensitive.
    assert _names(stage, paths, query, ["Primvars:*"], ordinal_range) == []
    with paths.create_path_list_from_strings([]) as empty:
        empty_query = stage.query_from_path_list(empty)
        try:
            assert _names(stage, paths, empty_query, ["primvars:*"], ordinal_range) == []
            assert _names(stage, paths, empty_query, ["*"], ordinal_range) == []
        finally:
            stage.release_query(empty_query).wait()


@pytest.mark.parametrize("selector", ["*mass", "a*b", "**", "primvars:**", "*:*"])
def test_invalid_selector_forms_fail_the_read(scene, selector):
    stage, paths, query = scene
    read = stage.read_attributes(query, [paths.intern_token(selector)], OrdinalRange.latest(1))
    with pytest.raises(OvstageError) as exc:
        read.wait()
    assert exc.value.code == ErrorCode.INVALID_ARGUMENT


@pytest.mark.parametrize("reserved", ["*", "primvars:*", "a*b"])
def test_other_operations_reject_names_with_star(scene, reserved):
    stage, paths, query = scene
    operations = [
        lambda: stage.write_attribute(query, reserved, 2, np.zeros(len(PRIMS), np.float32), is_array=False),
        lambda: stage.delete_attributes(query, [reserved], 2),
        lambda: stage.advance_write_floor(2, Scope.INCLUDE, [paths.intern_token("mass"), paths.intern_token(reserved)]),
    ]
    for operation in operations:
        with pytest.raises(OvstageError) as exc:
            operation().wait()
        assert exc.value.code == ErrorCode.INVALID_ARGUMENT
        assert "reserved for read selection" in exc.value.message
    # No call did part of its work: the floor of "mass" did not move, and
    # ordinal 2 has no change.
    floor = stage.get_attribute_write_floor("mass")
    assert floor.fetch() == 1
    floor.release().wait()
    stage.advance_write_floor(2).wait()
    assert _names(stage, paths, query, ["*"], OrdinalRange.between(2, 2)) == []


def test_patterns_find_attributes_written_after_the_query_is_made(scene):
    stage, paths, query = scene
    _write(stage, query, "primvars:metallic", 2)
    stage.advance_write_floor(2).wait()
    # The same query handle sees the new name. No new query is necessary.
    assert _names(stage, paths, query, ["primvars:*"], OrdinalRange.latest(2)) == [
        "primvars:displayOpacity", "primvars:metallic", "primvars:roughness",
    ]
    assert _names(stage, paths, query, ["primvars:*"], OrdinalRange.between(2, 2)) == ["primvars:metallic"]


def test_patterns_match_built_in_attributes(scene):
    stage, paths, query = scene
    mesh = paths.intern_token("Mesh")
    stage.write_attribute(
        query, "usd-prim-type", ordinal=2, tensors=np.full(len(PRIMS), mesh.raw_id, np.uint64), is_array=False,
    ).wait()
    stage.advance_write_floor(ordinal=2).wait()
    # "usd-*" matches the built-ins by name, as other attributes.
    latest = set(_names(stage, paths, query, ["usd-*"], OrdinalRange.latest(2)))
    assert {"usd-path", "usd-prim-type"} <= latest
    assert all(name.startswith("usd-") for name in latest)
    # A range read matches a recorded built-in that changed in the range.
    assert set(_names(stage, paths, query, ["usd-*"], OrdinalRange.between(2, 2))) == {"usd-prim-type"}


def test_patterns_hide_underscore_and_usd_names(scene):
    stage, paths, query = scene
    mesh = paths.intern_token("Mesh")
    stage.write_attribute(
        query, "usd-prim-type", ordinal=2, tensors=np.full(len(PRIMS), mesh.raw_id, np.uint64), is_array=False,
    ).wait()
    _write(stage, query, "_cache", 2)
    _write(stage, query, "mass", 2)
    stage.advance_write_floor(ordinal=2).wait()
    # "*" selects usd-path, but not the other names that start with "usd-" or "_".
    latest = set(_names(stage, paths, query, ["*"], OrdinalRange.latest(2)))
    assert "usd-path" in latest
    assert not any(name.startswith("_") or (name.startswith("usd-") and name != "usd-path") for name in latest)
    assert set(_names(stage, paths, query, ["*"], OrdinalRange.between(2, 2))) == {"mass"}
    # A pattern whose prefix starts with the namespace selects the hidden names.
    assert _names(stage, paths, query, ["_*"], OrdinalRange.latest(2)) == ["_cache"]
    assert _names(stage, paths, query, ["_*"], OrdinalRange.between(2, 2)) == ["_cache"]
    assert "usd-prim-type" in _names(stage, paths, query, ["usd-*"], OrdinalRange.latest(2))
    # A shorter prefix, such as "u*", does not.
    assert _names(stage, paths, query, ["u*"], OrdinalRange.latest(2)) == ["usd-path"]
    # A hidden name in the list of attribute names is read as usual.
    assert _names(stage, paths, query, ["_cache", "*"], OrdinalRange.between(2, 2)) == ["_cache", "mass"]


def test_range_reads_with_patterns_at_fixed_past_ordinals(scene):
    stage, paths, query = scene
    _write(stage, query, "primvars:roughness", 2, value=2.0)
    stage.advance_write_floor(2).wait()
    first = _read(stage, paths, query, ["primvars:*"], OrdinalRange.between(2, 2))
    assert first == [("primvars:roughness", 2)]
    # No later change: the same read gives the same result.
    stage.advance_write_floor(3).wait()
    assert _read(stage, paths, query, ["primvars:*"], OrdinalRange.between(2, 2)) == first

    # A matched name changed after the range end. Reads return only the latest
    # payload, so the range read fails instead of returning a later value.
    _write(stage, query, "primvars:roughness", 4, value=4.0)
    stage.advance_write_floor(4).wait()
    read = stage.read_attributes(query, [paths.intern_token("primvars:*")], OrdinalRange.between(2, 2))
    with pytest.raises(OvstageError) as exc:
        read.wait()
    assert exc.value.code == ErrorCode.OUT_OF_RANGE

    # A range read with a pattern must start at or above the oldest preserved
    # ordinal. More seals move that frontier up.
    for ordinal in range(5, 9):
        stage.advance_write_floor(ordinal).wait()
    frontier = stage.get_oldest_preserved_ordinal()
    oldest = frontier.fetch()
    frontier.release().wait()
    assert oldest > 1
    read = stage.read_attributes(query, [paths.intern_token("*")], OrdinalRange.between(oldest - 1, 8))
    with pytest.raises(OvstageError) as exc:
        read.wait()
    assert exc.value.code == ErrorCode.OUT_OF_RANGE
