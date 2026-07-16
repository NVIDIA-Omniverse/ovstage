# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage write-mode test: UPSERT creates-or-updates; INSERT is create-only
# admission and rejects a write whose target prims already exist. CPU-only. The
# write-flavors example is the workflow tour; this file asserts the admission rule.

import numpy as np
import pytest

from ovstage import ErrorCode, OrdinalRange, OvstageError, PathDictionary, PrimMode


def _scalar_values(stage, query, attr, end_ordinal, order):
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


def test_upsert_vs_insert(stage):
    with PathDictionary(stage) as paths:
        order = ["/World/Admission/A", "/World/Admission/B"]
        plist = paths.create_path_list_from_strings(order)
        query = stage.query_from_path_list(plist)
        try:
            score = paths.intern_token("score")
            stage.write_attribute(
                query, score, ordinal=1, tensors=np.array([1.0, 2.0], np.float32),
                is_array=False, prim_mode=PrimMode.INSERT,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()
            assert _scalar_values(stage, query, score, 1, order) == [1.0, 2.0]

            # [snippet:upsert-vs-insert]
            # With the target prims already present, INSERT rejects the write
            # (PRIM_NOT_FOUND, before anything is written). UPSERT updates present
            # prims and creates absent ones. The INSERT rejection surfaces from
            # .wait() as an OvstageError.
            with pytest.raises(OvstageError) as exc:
                stage.write_attribute(
                    query, score, ordinal=2, tensors=np.array([9.0, 9.0], np.float32),
                    is_array=False, prim_mode=PrimMode.INSERT,
                ).wait()
            assert exc.value.code == ErrorCode.PRIM_NOT_FOUND

            stage.write_attribute(
                query, score, ordinal=2, tensors=np.array([10.0, 20.0], np.float32),
                is_array=False, prim_mode=PrimMode.UPSERT,
            ).wait()
            stage.advance_write_floor(ordinal=2).wait()
            # [/snippet:upsert-vs-insert]

            assert _scalar_values(stage, query, score, 2, order) == [10.0, 20.0]
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


def test_write_below_floor_rejected(stage):
    with PathDictionary(stage) as paths:
        order = ["/World/Floor/A", "/World/Floor/B"]
        plist = paths.create_path_list_from_strings(order)
        query = stage.query_from_path_list(plist)
        try:
            score = paths.intern_token("score")
            # Seed and seal at ordinal 5 so the write floor is 5.
            stage.write_attribute(
                query, score, ordinal=5, tensors=np.array([1.0, 2.0], np.float32), is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=5).wait()

            # A write at or below the floor is rejected synchronously with
            # WRITE_FLOOR_VIOLATION; .wait() surfaces it as an OvstageError.
            with pytest.raises(OvstageError) as exc:
                stage.write_attribute(
                    query, score, ordinal=3, tensors=np.array([9.0, 9.0], np.float32), is_array=False,
                ).wait()
            assert exc.value.code == ErrorCode.WRITE_FLOOR_VIOLATION

            # An ordinal above the floor is admitted.
            stage.write_attribute(
                query, score, ordinal=6, tensors=np.array([10.0, 20.0], np.float32), is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=6).wait()
            assert _scalar_values(stage, query, score, 6, order) == [10.0, 20.0]
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)
