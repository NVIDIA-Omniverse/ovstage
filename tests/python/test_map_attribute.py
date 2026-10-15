# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage map/unmap test: map_attribute prepares writable staging buffers
# you fill directly. Commit each group via unmap_group, or commit remaining groups
# in the final unmap. Covers existing and newly created columns. CPU-only.
# The write-flavors example is the workflow tour.

import numpy as np
import pytest

from ovstage import DLDataType, DLDataTypeCode, OrdinalRange, PathDictionary

FLOAT3 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)

# Only numpy >= 2.1 requests a versioned DLPack capsule and honors its read-only
# flag. Older numpy takes the legacy capsule, which has no flags field at all, so a
# readonly=True export is silently writable there and the rejection cannot be tested.
_NUMPY_HONORS_READONLY = np.lib.NumpyVersion(np.__version__) >= "2.1.0"


def _rows_by_prim(stage, query, attr, end_ordinal, lanes):
    """Latest committed value(s) per prim (list-relative index -> list of floats)."""
    read = stage.read_attributes(query, [attr], OrdinalRange.latest(end_ordinal))
    read.wait()
    rows = {}
    for group in read.groups():
        if not group.is_delete:
            values = np.asarray(group.array(0)).reshape(-1)
            for local in range(group.prim_count):
                row = group.data_row_index(local) if group.has_data_index_map else local
                rows[group.prim_index(local)] = [float(v) for v in values[row * lanes:row * lanes + lanes]]
        stage.release_group(group)
    read.release().wait()
    return rows


def test_map_existing_and_fresh_column(stage):
    with PathDictionary(stage) as paths:
        order = ["/World/Mapped/A", "/World/Mapped/B"]
        plist = paths.create_path_list_from_strings(order)
        query = stage.query_from_path_list(plist)
        try:
            existing = paths.intern_token("map-existing")
            fresh = paths.intern_token("map-fresh")
            stage.write_attribute(
                query, existing, ordinal=1, tensors=np.array([1.0, 2.0], np.float32), is_array=False
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            # [snippet:map-unmap-cpu]
            # map_attribute prepares writable staging buffers for the session's ordinal.
            # Each buffer is WRITE-ONLY (uninitialized, not a view of current values),
            # so fill every element.
            # Commit per group via unmap_group (streaming), or commit remaining groups
            # when the session's with-block exits (the final unmap).
            # [snippet:dlpack-map-existing]
            fill = {0: 10.0, 1: 20.0}  # by list-relative prim index (A, B)
            with stage.map_attribute(query, existing, ordinal=2) as session:
                session.wait()
                for group in session.groups():
                    buffer = group.array(0)
                    for local in range(group.prim_count):
                        row = group.data_row_index(local) if group.has_data_index_map else local
                        buffer[row] = fill[group.prim_index(local)]
                    session.unmap_group(group).wait()  # streaming commit of this group
            stage.advance_write_floor(ordinal=2).wait()
            # [/snippet:dlpack-map-existing]

            # A NEW column: the descriptor dtype (3-lane float32) defines it. No
            # unmap_group this time — the final unmap commits the remaining groups.
            fill3 = {0: [1.0, 2.0, 3.0], 1: [4.0, 5.0, 6.0]}
            with stage.map_attribute(query, fresh, ordinal=3, dtype=FLOAT3) as session:
                session.wait()
                for group in session.groups():
                    buffer = group.array(0)
                    for local in range(group.prim_count):
                        row = group.data_row_index(local) if group.has_data_index_map else local
                        buffer[row] = fill3[group.prim_index(local)]
            stage.advance_write_floor(ordinal=3).wait()
            # [/snippet:map-unmap-cpu]

            assert _rows_by_prim(stage, query, existing, 2, 1) == {0: [10.0], 1: [20.0]}
            assert _rows_by_prim(stage, query, fresh, 3, 3) == {0: [1.0, 2.0, 3.0], 1: [4.0, 5.0, 6.0]}
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


@pytest.mark.skipif(
    not _NUMPY_HONORS_READONLY, reason="numpy < 2.1 ignores the DLPack read-only flag"
)
def test_map_group_dlpack_readonly_view_rejects_write(stage):
    # A read-only DLPack export must be refused by the consumer with a clean
    # ValueError. Regression guard: the DLPack deleter used to be a Python ctypes
    # callback, so releasing the tensor while that ValueError was propagating
    # aborted the callback at its first C call. The pending exception was reported
    # unraisable and cleared, the caller saw "SystemError: error return without
    # exception set" instead, and the callback's cleanup never ran.
    with PathDictionary(stage) as paths:
        order = ["/World/ReadOnly/A", "/World/ReadOnly/B"]
        plist = paths.create_path_list_from_strings(order)
        query = stage.query_from_path_list(plist)
        try:
            attr = paths.intern_token("map-readonly")
            stage.write_attribute(
                query, attr, ordinal=1, tensors=np.array([1.0, 2.0], np.float32), is_array=False
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            with stage.map_attribute(query, attr, ordinal=2) as session:
                session.wait()
                for group in session.groups():
                    # [snippet:map-dlpack-readonly]
                    # A map group is writable, so dlpack() exports it writable by
                    # default and an in-place fill lands in the mapped buffer.
                    np.from_dlpack(group.dlpack(0, readonly=False))[:] = 5.0

                    # Pass readonly=True to hand out a view the recipient must not
                    # write through — numpy honors the DLPack read-only flag and
                    # refuses the assignment rather than corrupting the buffer.
                    with pytest.raises(ValueError):
                        np.from_dlpack(group.dlpack(0, readonly=True))[:] = 1.0
                    # [/snippet:map-dlpack-readonly]
                    session.unmap_group(group).wait()
            stage.advance_write_floor(ordinal=2).wait()

            # The refused write changed nothing: the committed column still holds
            # what the writable fill put there.
            assert _rows_by_prim(stage, query, attr, 2, 1) == {0: [5.0], 1: [5.0]}
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


@pytest.mark.parametrize("components,matrix", [(1, False), (3, False), (4, False),
                                                (12, False), (16, False),
                                                (4, True), (9, True), (16, True)])
@pytest.mark.parametrize("array", [False, True])
@pytest.mark.parametrize("streaming", [False, True])
def test_read_map_share_typed_layout(stage, components, matrix, array, streaming):
    from ovstage import AttributeSemantic, typed_tensor_to_write

    semantic = AttributeSemantic.MATRIX if matrix else AttributeSemantic.NONE
    side = int(components ** 0.5)
    tail = (side, side) if matrix else (components,) if components > 1 else ()
    with PathDictionary(stage) as paths:
        # Deliberately reverse path order; compare via each group's prim mapping.
        plist = paths.create_path_list_from_strings(["/Typed/B", "/Typed/A"])
        query = stage.query_from_path_list(plist)
        attr = paths.intern_token("typed-map")
        dtype = DLDataType(code=DLDataTypeCode.kDLFloat, bits=64, lanes=components)
        expected = {}
        try:
            with stage.map_attribute(query, attr, ordinal=1, dtype=dtype, semantic=semantic,
                                     element_sizes=[0, 2] if array else None) as session:
                session.wait()
                for group in session.groups():
                    for i in range(group.tensor_count):
                        tensor = group.tensor(i)
                        rows = (0 if i == 0 else 2) if array else 2
                        assert tensor.dtype.lanes == 1
                        assert tensor.shape_tuple == (rows, *tail)
                        view = group.array(i)
                        assert view.shape == tensor.shape_tuple and view.flags.writeable
                        producer = group.dlpack(i)
                        assert producer.shape == tensor.shape_tuple
                        if view.size:
                            assert np.from_dlpack(producer).shape == view.shape
                            assert view.ctypes.data == tensor.data + tensor.byte_offset
                        converted = typed_tensor_to_write(tensor)
                        assert converted.data == tensor.data
                        assert converted.dtype.lanes == components
                        assert converted.shape_tuple == (rows,)
                        view[:] = np.arange(view.size).reshape(view.shape) + 10 * (i + 1)
                        if array:
                            expected[group.prim_index(i)] = view.copy()
                        else:
                            for local in range(group.prim_count):
                                expected[group.prim_index(local)] = view[group.data_row_index(local)].copy()
                    if streaming:
                        session.unmap_group(group).wait()
                        with pytest.raises(RuntimeError):
                            group.tensor(0)
            stage.advance_write_floor(ordinal=1).wait()
            read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
            read.wait()
            try:
                seen = set()
                for group in read.groups():
                    with group:
                        for local in range(group.prim_count):
                            index = group.prim_index(local)
                            row = group.data_row_index(local)
                            value = group.array(row) if array else group.array(0)[row]
                            np.testing.assert_array_equal(value, expected[index])
                            assert not group.array(row if array else 0).flags.writeable
                            seen.add(index)
                assert seen == {0, 1}
            finally:
                read.release().wait()
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


# [snippet:typed-write-adapter-outer-stride]
@pytest.mark.parametrize("stride", [4, 8, 0])
def test_typed_write_adapter_preserves_outer_stride(stride):
    from ovstage import DLTensor, typed_tensor_to_write

    backing = np.arange(16, dtype=np.float64)
    values = np.lib.stride_tricks.as_strided(backing, shape=(2, 2, 2),
        strides=(stride * backing.itemsize, 2 * backing.itemsize, backing.itemsize))
    source = DLTensor.from_dlpack(values)
    converted = typed_tensor_to_write(source)
    assert converted.shape_tuple == (2,)
    assert converted.dtype.lanes == 4
    assert converted.strides[0] == stride // 4
    assert converted.data == source.data
    assert source.shape_tuple == (2, 2, 2) and source.dtype.lanes == 1
    import ctypes
    invalid_strides = (ctypes.c_int64 * 3)(stride, 3, 1)
    source.strides = invalid_strides
    with pytest.raises(ValueError, match="component axes"):
        typed_tensor_to_write(source)

# [/snippet:typed-write-adapter-outer-stride]
