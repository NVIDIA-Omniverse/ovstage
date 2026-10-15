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

from ovstage import (AttributeSemantic, DLDataType, DLDataTypeCode, ErrorCode,
                     OrdinalRange, OVX_API_ERROR, OvxError, PathDictionary, make_dltensor)


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
        for handle in (0, 0xDEADBEEF):
            with pytest.raises(
                OvxError, match="Prim path handle was not found in this dictionary"
            ) as exc:
                paths.path_to_string(handle)
            assert exc.value.code == OVX_API_ERROR


def test_path_dictionary_root_succeeds_while_invalid_raises(stage):
    """A valid root path and a bad handle must stay distinguishable.

    The root path decodes to zero tokens, and an invalid handle used to be
    reported the same way, so both surfaced as "". The zero-token root is a
    success; only the bad handle is an error.

    In C the root decomposes to one processed path with zero component tokens;
    get_tokens_from_paths returns tokens, not a path string. Python joins that
    result and spells the root "/", matching SdfPath, so every valid handle
    round-trips back through intern_path and the empty string is never a
    successful result.
    """
    with PathDictionary(stage) as paths:
        root = paths.intern_path("/")
        assert paths.path_to_string(root) == "/"
        assert paths.intern_path(paths.path_to_string(root)) == root

        with pytest.raises(OvxError, match="Prim path handle was not found in this dictionary"):
            paths.path_to_string(0xDEADBEEF)


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
        # [snippet:dlpack-three-prim-query]
        attr = paths.intern_token("temperature")
        prim_paths = paths.create_path_list_from_strings(["/World/A", "/World/B", "/World/C"])
        query = stage.query_from_path_list(prim_paths)
        # [/snippet:dlpack-three-prim-query]
        try:
            # [snippet:write-read-roundtrip]
            # Write one float per prim into "temperature" at ordinal 1, seal it by
            # advancing the write floor to 1, then read the column back. Async ops
            # return an Operation; .wait() blocks and raises on failure.
            # [snippet:dlpack-pass-values]
            stage.write_attribute(
                query,
                attr,
                ordinal=1,
                tensors=np.array([1.0, 2.0, 3.0], np.float32),
                is_array=False,
            ).wait()
            # [/snippet:dlpack-pass-values]
            stage.advance_write_floor(ordinal=1).wait()

            read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
            read.wait()
            group = read.fetch_next()
            assert group is not None
            # group.array(i) is a zero-copy view into the group's storage; copy it
            # out (np.array) so it stays valid after the group is released.
            # [snippet:dlpack-copy-result]
            values = np.array(group.array(0))
            stage.release_group(group)
            # [/snippet:dlpack-copy-result]
            read.release().wait()
            # [/snippet:write-read-roundtrip]

            np.testing.assert_allclose(values, [1.0, 2.0, 3.0])
        finally:
            # Release every handle before the stage is destroyed (on `with` exit):
            # the query handle and the path-list reference.
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)


def test_wait_returns_its_receiver_for_chaining(stage):
    """``wait()`` returns the object it was called on.

    Success path only: the public wheel has no deterministic way to stall an op,
    so the timeout contract (a timed-out ``wait()`` leaves the op waitable) is
    asserted by the source-tree suite.
    """
    with PathDictionary(stage) as paths:
        attr = paths.intern_token("temperature")
        prim_paths = paths.create_path_list_from_strings(["/World/Chain/A", "/World/Chain/B"])
        query = stage.query_from_path_list(prim_paths)
        try:
            # [snippet:wait-chaining]
            # wait() returns its receiver: an Operation returns the Operation, and a
            # handle (Query / Read / Map / OrdinalQuery / Hierarchy) returns that
            # handle, so the follow-on call chains. Bind the handle in a `with` block
            # first: chaining on the temporary (stage.read_attributes(...).wait()
            # .fetch_next()) drops the Read unreleased. wait() is not redundant with
            # fetch_next()'s own wait — it is what releases the read's reserving op.
            write = stage.write_attribute(
                query, attr, ordinal=1, tensors=np.array([1.0, 2.0], np.float32), is_array=False
            )
            assert write.wait() is write
            stage.advance_write_floor(ordinal=1).wait()

            with stage.read_attributes(query, [attr], OrdinalRange.latest(1)) as read:
                group = read.wait().fetch_next()
                assert group is not None
                with group:
                    values = np.array(group.array(0))
            # [/snippet:wait-chaining]
            np.testing.assert_allclose(values, [1.0, 2.0])
            # A synchronously-created handle has no reserving op; wait() still
            # returns it, so the same chaining works.
            assert query.op is None
            assert query.wait() is query
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)


def test_read_group_array_is_read_only(stage):
    with PathDictionary(stage) as paths:
        attr = paths.intern_token("read-only-array")
        prim_paths = paths.create_path_list_from_strings(["/World/ReadOnly"])
        query = stage.query_from_path_list(prim_paths)
        try:
            stage.write_attribute(
                query,
                attr,
                ordinal=1,
                tensors=np.array([1.0], np.float32),
                is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
            group = None
            try:
                read.wait()
                group = read.fetch_next()
                assert group is not None
                tensor = group.tensor(0)
                values = group.array(0)

                assert values.ctypes.data == int(tensor.data) + int(tensor.byte_offset)
                assert not values.flags.writeable
                with pytest.raises(ValueError):
                    values[0] = 2.0
                with pytest.raises(ValueError):
                    values.setflags(write=True)
                np.testing.assert_array_equal(values, [1.0])
            finally:
                if group is not None:
                    stage.release_group(group)
                read.release().wait()
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)


def test_convenience_fixed_shapes_read_back_with_type_aware_shapes(stage):
    with PathDictionary(stage) as paths:
        point = paths.intern_token("point3-convenience")
        point_array = paths.intern_token("point3-array-convenience")
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
                point_array,
                ordinal=1,
                tensors=[
                    make_dltensor(values.reshape(-1),
                                  dtype=DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3),
                                  shape=[len(values)])
                    for values in (point_values[:1], point_values)
                ],
                is_array=True,
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
                point: ((2, 3), point_values),
                matrix: ((2, 4, 4), matrix_values),
            }
            read = stage.read_attributes(query, [point, matrix], OrdinalRange.latest(1))
            read.wait()
            seen = set()
            while True:
                group = read.fetch_next()
                if group is None:
                    break
                exported_shape, values = expected[group.attribute]
                raw = group.tensor(0)
                assert raw.ndim == len(exported_shape)
                assert raw.shape_tuple == exported_shape
                assert raw.dtype.lanes == 1
                exported = np.from_dlpack(group.dlpack(0))
                assert exported.shape == exported_shape
                assert group.array(0).shape == exported_shape
                np.testing.assert_allclose(exported, values)
                seen.add(group.attribute)
                stage.release_group(group)
            assert seen == {point, matrix}
            read.release().wait()
            with stage.read_attributes(query, [point_array], OrdinalRange.latest(1)) as array_read:
                array_read.wait()
                with array_read.fetch_next() as array_group:
                    assert array_group.is_array
                    assert array_group.tensor_count == 2
                    for row, values in enumerate((point_values[:1], point_values)):
                        raw = array_group.tensor(row)
                        assert raw.shape_tuple == values.shape
                        assert raw.dtype.lanes == 1
                        assert array_group.array(row).shape == values.shape
                        np.testing.assert_allclose(np.from_dlpack(array_group.dlpack(row)), values)
            # [/snippet:canonical-fixed-shapes]
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)
