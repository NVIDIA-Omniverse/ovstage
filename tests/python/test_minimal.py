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
# First ovstage public Python test (Phase 1 vertical slice). The Python sibling
# of tests/c/test_minimal.cpp: it asserts the write -> advance write floor ->
# read round-trip (the minimal example only prints it) and doubles as the tested
# source for the write-read-roundtrip snippet. CPU-only — no GPU required.

import numpy as np
import pytest

from ovstage import AttributeSemantic, ErrorCode, OrdinalRange, OvxError, PathDictionary


@pytest.mark.parametrize("value", ["pre\x00post", "\x00lead", "trail\x00"])
def test_path_dictionary_rejects_embedded_nul(stage, value):
    with PathDictionary(stage) as paths:
        with pytest.raises(OvxError, match="Token string contains embedded NUL"):
            paths.intern_token(value)
        with pytest.raises(OvxError, match="Path string contains embedded NUL"):
            paths.intern_path(value)


def test_path_dictionary_errors_are_actionable(stage):
    with PathDictionary(stage) as paths:
        with pytest.raises(OvxError, match="Path string must be non-empty"):
            paths.intern_path("")
        with pytest.raises(OvxError, match="Path list handle was not found"):
            paths.path_list_count(0xDEADBEEF)


def test_path_dictionary_creates_empty_path_lists(stage):
    with PathDictionary(stage) as paths:
        empty_path_lists = []
        try:
            empty_path_lists.append(paths.create_path_list([]))
            empty_path_lists.append(paths.create_path_list_from_strings([]))
            assert empty_path_lists[0] != empty_path_lists[1]
            for empty_path_list in empty_path_lists:
                assert paths.path_list_count(empty_path_list) == 0
                assert paths.get_paths(empty_path_list) == []
        finally:
            for empty_path_list in empty_path_lists:
                paths.destroy_path_list(empty_path_list)


def test_write_advance_read(stage):
    with PathDictionary(stage) as paths:
        attr = paths.intern_token("temperature")
        prim_paths = paths.create_path_list_from_strings(["/World/A", "/World/B", "/World/C"])
        query = stage.query_from_path_list(prim_paths)
        try:
            # [snippet:write-read-roundtrip]
            # Write one float per prim into "temperature" at ordinal 1, seal it by
            # advancing the write floor to 1, then read the column back. Async ops
            # return an Operation; .wait() blocks and raises on failure.
            stage.write_attribute(
                query,
                attr,
                ordinal=1,
                tensors=np.array([1.0, 2.0, 3.0], np.float32),
                is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
            read.wait()
            group = read.fetch_next()
            assert group is not None
            # group.array(i) is a zero-copy view into the group's storage; copy it
            # out (np.array) so it stays valid after the group is released.
            values = np.array(group.array(0))
            stage.release_group(group)
            read.release().wait()
            # [/snippet:write-read-roundtrip]

            np.testing.assert_allclose(values, [1.0, 2.0, 3.0])
        finally:
            # Release every handle before the stage is destroyed (on `with` exit):
            # the query handle and the path-list reference.
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)


def test_convenience_fixed_shapes_read_back_in_canonical_lane_form(stage):
    with PathDictionary(stage) as paths:
        point = paths.intern_token("point3-convenience")
        matrix = paths.intern_token("matrix4-convenience")
        flat = paths.intern_token("point3-flat-invalid")
        prim_paths = paths.create_path_list_from_strings(["/World/A", "/World/B"])
        query = stage.query_from_path_list(prim_paths)
        try:
            # [snippet:canonical-fixed-shapes]
            point_values = np.arange(6, dtype=np.float32).reshape(2, 3)
            matrix_values = np.arange(32, dtype=np.float64).reshape(2, 4, 4)
            rejected_flat = stage.write_attribute(
                query,
                flat,
                ordinal=1,
                tensors=point_values.reshape(-1),
                is_array=False,
                semantic=AttributeSemantic.POINT,
            )
            assert rejected_flat.status == ErrorCode.INVALID_ARGUMENT
            assert rejected_flat.op_id == 0

            stage.write_attribute(
                query,
                point,
                ordinal=1,
                tensors=point_values,
                is_array=False,
                semantic=AttributeSemantic.POINT,
            ).wait()
            stage.write_attribute(
                query,
                matrix,
                ordinal=1,
                tensors=matrix_values,
                is_array=False,
                semantic=AttributeSemantic.MATRIX,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            expected = {
                point: (3, (2, 3), point_values),
                matrix: (16, (2, 16), matrix_values.reshape(2, 16)),
            }
            read = stage.read_attributes(query, [point, matrix], OrdinalRange.latest(1))
            read.wait()
            seen = set()
            while True:
                group = read.fetch_next()
                if group is None:
                    break
                lanes, exported_shape, values = expected[group.attribute]
                raw = group.tensor(0)
                assert raw.ndim == 1
                assert raw.shape_tuple == (2,)
                assert raw.dtype.lanes == lanes
                exported = np.from_dlpack(group.dlpack(0))
                assert exported.shape == exported_shape
                np.testing.assert_allclose(exported, values)
                seen.add(group.attribute)
                stage.release_group(group)
            assert seen == {point, matrix}
            read.release().wait()
            # [/snippet:canonical-fixed-shapes]
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)
