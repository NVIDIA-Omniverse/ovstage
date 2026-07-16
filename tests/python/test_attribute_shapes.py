# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage attribute-shapes test: the write→read round-trip preserves the
# three fixed/ragged column shapes — a 1-lane scalar, a fixed multi-lane tuple
# (float3, lanes in the dtype not the shape), and a ragged per-prim array. CPU-only.
# The write-flavors example is the workflow tour; this file asserts the shapes.

import numpy as np

from ovstage import DLDataType, DLDataTypeCode, OrdinalRange, PathDictionary, make_dltensor

# One float3 storage type: the per-element tuple width lives in dtype.lanes
# (a 3-lane float32), NOT in the tensor shape (which stays [prim_count]).
FLOAT3 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)


def _read_one_group(stage, query, attr, end_ordinal):
    """Read a dense column written at one ordinal over the whole query: one group.
    Returns (read_handle, group); the caller releases both."""
    read = stage.read_attributes(query, [attr], OrdinalRange.latest(end_ordinal))
    read.wait()
    return read, read.fetch_next()


def test_scalar_and_fixed_lane_shapes(stage):
    with PathDictionary(stage) as paths:
        order = ["/World/A", "/World/B", "/World/C"]
        plist = paths.create_path_list_from_strings(order)
        query = stage.query_from_path_list(plist)
        try:
            # [snippet:attribute-shapes-fixed]
            # A fixed-size column stacks one row per prim in ONE tensor: a scalar
            # is a 1-lane dtype; a float3 is the SAME write with a 3-lane dtype.
            # The shape stays [prim_count]; the tuple width lives in dtype.lanes.
            temperature = paths.intern_token("temperature")
            velocity = paths.intern_token("velocity")
            stage.write_attribute(
                query, temperature, ordinal=1,
                tensors=np.array([18.5, 19.5, 20.5], np.float32), is_array=False,
            ).wait()
            stage.write_attribute(
                query, velocity, ordinal=1,
                tensors=make_dltensor(
                    np.array([1, 0, 0, 0, 1, 0, 0, 0, 1], np.float32), dtype=FLOAT3, shape=[3]
                ),
                is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()
            # [/snippet:attribute-shapes-fixed]

            read, group = _read_one_group(stage, query, temperature, 1)
            assert group is not None
            scalar = group.array(0)
            assert group.tensor(0).dtype.lanes == 1
            assert bool(np.allclose(scalar, [18.5, 19.5, 20.5]))
            stage.release_group(group)
            read.release().wait()

            read, group = _read_one_group(stage, query, velocity, 1)
            assert group is not None
            # A 3-lane column reads back as a flat view of prim_count * 3 values.
            vec = np.asarray(group.array(0)).reshape(-1)
            assert group.tensor(0).dtype.lanes == 3
            assert vec.shape[0] == 9
            assert bool(np.allclose(vec, [1, 0, 0, 0, 1, 0, 0, 0, 1]))
            stage.release_group(group)
            read.release().wait()
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


def test_ragged_array_shape(stage):
    with PathDictionary(stage) as paths:
        order = ["/World/A", "/World/B", "/World/C"]
        plist = paths.create_path_list_from_strings(order)
        query = stage.query_from_path_list(plist)
        try:
            # [snippet:attribute-shapes-ragged]
            # is_array=True declares a ragged (variable-length per prim) column —
            # never inferred from the payload. A list of tensors carries one row
            # per prim (lengths 2 / 3 / 1 here); read groups mirror that with one
            # tensor per prim.
            samples = paths.intern_token("samples")
            rows = [
                np.array([1.0, 2.0], np.float32),
                np.array([3.0, 4.0, 5.0], np.float32),
                np.array([6.0], np.float32),
            ]
            stage.write_attribute(query, samples, ordinal=1, tensors=rows, is_array=True).wait()
            stage.advance_write_floor(ordinal=1).wait()
            # [/snippet:attribute-shapes-ragged]

            read, group = _read_one_group(stage, query, samples, 1)
            assert group is not None
            got = {}
            for local in range(group.prim_count):
                path = order[group.prim_index(local)]
                row = group.data_row_index(local) if group.has_data_index_map else local
                got[path] = [float(v) for v in np.asarray(group.array(row))]
            stage.release_group(group)
            read.release().wait()

            assert got == {"/World/A": [1.0, 2.0], "/World/B": [3.0, 4.0, 5.0], "/World/C": [6.0]}
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)
