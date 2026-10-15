# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage clone test: write a source prim, clone its subtree to new
# targets, and assert the clones are queryable with copied attribute values.
# CPU-only. The minimal example demonstrates clone; this file asserts it.

import numpy as np

from ovstage import CloneResult, Filter, FilterOp, OrdinalRange, PathDictionary, Predicate


def _prim_count(stage, path: str) -> int:
    query = stage.query(filter=Filter([Predicate("usd-path", FilterOp.IN, [path])]))
    query.wait()
    count = stage.fetch_query_result(query).total_prim_count
    stage.release_query(query).wait()
    return count


def test_clone_subtree_copies_values(stage):
    with PathDictionary(stage) as paths:
        attr = paths.intern_token("temperature")
        prim_paths = paths.create_path_list_from_strings(["/World/A"])
        query = stage.query_from_path_list(prim_paths)
        try:
            # [snippet:clone-and-verify]
            # Write a source prim, seal ordinal 1, clone its subtree to new
            # targets at ordinal 2, seal again, then read back matching values.
            stage.write_attribute(
                query,
                attr,
                ordinal=1,
                tensors=np.array([42.0], np.float32),
                is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            stage.clone("/World/A", ["/World/A_env0", "/World/A_env1"], ordinal=2)
            stage.advance_write_floor(ordinal=2).wait()

            for target in ("/World/A_env0", "/World/A_env1"):
                assert _prim_count(stage, target) == 1
                target_query = stage.query(
                    filter=Filter([Predicate("usd-path", FilterOp.IN, [target])])
                )
                target_query.wait()
                read = stage.read_attributes(target_query, [attr], OrdinalRange.latest(2))
                read.wait()
                group = read.fetch_next()
                assert group is not None
                assert float(np.array(group.array(0))[0]) == 42.0
                stage.release_group(group)
                read.release().wait()
                stage.release_query(target_query).wait()
            # [/snippet:clone-and-verify]
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)


def test_clone_reports_created_prims(stage):
    with PathDictionary(stage) as paths:
        attr = paths.intern_token("temperature")
        prim_paths = paths.create_path_list_from_strings(["/World/A"])
        query = stage.query_from_path_list(prim_paths)
        try:
            stage.write_attribute(
                query,
                attr,
                ordinal=1,
                tensors=np.array([42.0], np.float32),
                is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            # [snippet:clone-result]
            # A blocking clone returns its completion report: the targets' scene
            # hierarchy subtree sizes after cloning, including each target root,
            # summed over all targets, alongside the inputs it was given.
            report = stage.clone("/World/A", ["/World/A_env2", "/World/A_env3"], ordinal=2)
            assert isinstance(report, CloneResult)
            assert report.prims_cloned == 2  # one source prim, two targets
            assert report.target_count == 2
            assert report.source_prim_count == 1
            assert report.ordinal == 2

            # The asynchronous form: wait() returns the operation, result() its report.
            op = stage.clone_async("/World/A", ["/World/A_env4", "/World/A_env5"], ordinal=2)
            assert op.wait().result().prims_cloned == 2
            # [/snippet:clone-result]
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)
