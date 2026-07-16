# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage delete test: a named-attribute delete tombstones just that column
# on the target prim; an empty attribute list tombstones the prim entirely. A HAS
# filter query (latest committed state) shows the prim set shrinking. CPU-only.
# The write-flavors example is the workflow tour; this file asserts the tombstones.

import numpy as np

from ovstage import Filter, FilterOp, PathDictionary, Predicate


def _has_count(stage, attr) -> int:
    with stage.query(filter=Filter([Predicate(attr, FilterOp.HAS)])) as q:
        q.wait()
        return q.result().total_prim_count


def test_delete_attribute_then_prim(stage):
    with PathDictionary(stage) as paths:
        del_heat = paths.intern_token("del-heat")
        both = paths.create_path_list_from_strings(["/World/Del/A", "/World/Del/B"])
        a_only = paths.create_path_list_from_strings(["/World/Del/A"])
        b_only = paths.create_path_list_from_strings(["/World/Del/B"])
        query = stage.query_from_path_list(both)
        a_query = stage.query_from_path_list(a_only)
        b_query = stage.query_from_path_list(b_only)
        try:
            stage.write_attribute(
                query, del_heat, ordinal=1, tensors=np.array([1.0, 2.0], np.float32), is_array=False
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()
            assert _has_count(stage, del_heat) == 2

            # [snippet:delete-attribute-then-prim]
            # delete_attributes writes a tombstone: reads at or above the delete
            # ordinal no longer see the attribute. A named attribute list deletes
            # just those columns on the target prims; an EMPTY list tombstones the
            # prims entirely. A HAS filter query shows the prim set shrinking.
            stage.delete_attributes(b_query, [del_heat], ordinal=2).wait()  # one attribute, one prim
            stage.advance_write_floor(ordinal=2).wait()
            assert _has_count(stage, del_heat) == 1  # only A still carries del-heat

            stage.delete_attributes(a_query, [], ordinal=3).wait()  # empty list = whole-prim tombstone
            stage.advance_write_floor(ordinal=3).wait()
            assert _has_count(stage, del_heat) == 0
            # [/snippet:delete-attribute-then-prim]
        finally:
            stage.release_query(b_query).wait()
            stage.release_query(a_query).wait()
            stage.release_query(query).wait()
            paths.destroy_path_list(b_only)
            paths.destroy_path_list(a_only)
            paths.destroy_path_list(both)
