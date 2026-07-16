# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage sparse-write test: index_map (a gather over a subset of the
# query's prims) and mask (a bitmask over the target rows) each update only their
# targeted prims, leaving the rest unchanged. CPU-only. The write-flavors example
# is the workflow tour; this file asserts the sparse semantics.

import numpy as np

from ovstage import OrdinalRange, PathDictionary


def _values(stage, query, attr, end_ordinal, order):
    """Latest committed value per prim of a 1-lane column, indexed like `order`."""
    read = stage.read_attributes(query, [attr], OrdinalRange.latest(end_ordinal))
    read.wait()
    rows = {}
    for group in read.groups():
        if not group.is_delete:
            values = group.array(0)
            for local in range(group.prim_count):
                row = group.data_row_index(local) if group.has_data_index_map else local
                rows[group.prim_index(local)] = float(values[row])
        stage.release_group(group)
    read.release().wait()
    return [rows.get(i) for i in range(len(order))]


def test_index_map_and_mask(stage):
    with PathDictionary(stage) as paths:
        order = ["/World/Sparse/A", "/World/Sparse/B", "/World/Sparse/C", "/World/Sparse/D"]
        plist = paths.create_path_list_from_strings(order)
        query = stage.query_from_path_list(plist)
        try:
            heat = paths.intern_token("sparse-heat")
            stage.write_attribute(
                query, heat, ordinal=1, tensors=np.array([10.0, 20.0, 30.0, 40.0], np.float32),
                is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()
            assert _values(stage, query, heat, 1, order) == [10.0, 20.0, 30.0, 40.0]

            # [snippet:sparse-index-map-and-mask]
            # Two sparse forms, mutually exclusive per write. index_map is a GATHER:
            # count selects how many of the query's prims are targeted (the first
            # `count`, in query order) and index_map[i] picks the SOURCE tensor row
            # for target i. A non-involutory 3-cycle proves the direction (gather reads
            # source[index_map[i]] into target i; scatter would give a different result).
            # mask is a bitmask over the `count` target rows: only set bits are written.
            # Untouched prims keep their earlier value.
            stage.write_attribute(
                query, heat, ordinal=2, tensors=np.array([111.0, 222.0, 333.0], np.float32),
                is_array=False, index_map=[1, 2, 0],  # A<-row1(222), B<-row2(333), C<-row0(111)
            ).wait()
            stage.advance_write_floor(ordinal=2).wait()
            assert _values(stage, query, heat, 2, order) == [222.0, 333.0, 111.0, 40.0]

            stage.write_attribute(
                query, heat, ordinal=3, tensors=np.array([0.0, 0.0, 0.0, 44.0], np.float32),
                is_array=False, mask=[0b1000], count=4,  # bit 3 set -> only D (row 3)
            ).wait()
            stage.advance_write_floor(ordinal=3).wait()
            # [/snippet:sparse-index-map-and-mask]

            assert _values(stage, query, heat, 3, order) == [222.0, 333.0, 111.0, 44.0]
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)
