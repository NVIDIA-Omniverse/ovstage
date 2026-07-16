# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage map/unmap test: map_attribute reserves writable storage-side
# buffers you fill directly (the zero-copy programming model), committed per group
# via unmap_group or all at once by the final unmap. Covers an existing column and
# a freshly-created one. CPU-only. The write-flavors example is the workflow tour.

import numpy as np

from ovstage import DLDataType, DLDataTypeCode, OrdinalRange, PathDictionary

FLOAT3 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)


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
            # map_attribute reserves storage-side buffers you fill directly. The
            # mapped buffer is a WRITE-ONLY staging buffer for the session's ordinal
            # (uninitialized, not a view of current values), so fill every element.
            # Commit per group via unmap_group (streaming) or all at once when the
            # session's with-block exits (the final unmap).
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

            # A NEW column: the descriptor dtype (3-lane float32) defines it. No
            # unmap_group this time — the final unmap commits everything.
            fill3 = {0: [1.0, 2.0, 3.0], 1: [4.0, 5.0, 6.0]}
            with stage.map_attribute(query, fresh, ordinal=3, dtype=FLOAT3) as session:
                session.wait()
                for group in session.groups():
                    buffer = group.array(0)
                    for local in range(group.prim_count):
                        row = group.data_row_index(local) if group.has_data_index_map else local
                        buffer[row * 3:row * 3 + 3] = fill3[group.prim_index(local)]
            stage.advance_write_floor(ordinal=3).wait()
            # [/snippet:map-unmap-cpu]

            assert _rows_by_prim(stage, query, existing, 2, 1) == {0: [10.0], 1: [20.0]}
            assert _rows_by_prim(stage, query, fresh, 3, 3) == {0: [1.0, 2.0, 3.0], 1: [4.0, 5.0, 6.0]}
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)
