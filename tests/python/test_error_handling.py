# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage error-handling test: assert clone's create-only contract fails
# a second clone onto an already-existing target. CPU-only. The minimal example
# illustrates error checking; this file asserts a specific failure mode and the
# code/message it surfaces.

import numpy as np
import pytest

from ovstage import ErrorCode, Filter, FilterOp, OvstageError, PathDictionary, Predicate


def _prim_count(stage, path: str) -> int:
    query = stage.query(filter=Filter([Predicate("usd-path", FilterOp.IN, [path])]))
    query.wait()
    count = stage.fetch_query_result(query).total_prim_count
    stage.release_query(query).wait()
    return count


def test_clone_to_existing_target_fails(stage):
    with PathDictionary(stage) as paths:
        attr = paths.intern_token("temperature")
        prim_paths = paths.create_path_list_from_strings(["/World/Source"])
        query = stage.query_from_path_list(prim_paths)
        try:
            # Establish the clone source (a write creates a queryable prim) and
            # seal it so the clone at a later ordinal may reproduce it.
            stage.write_attribute(
                query,
                attr,
                ordinal=1,
                tensors=np.array([1.0], np.float32),
                is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            # [snippet:clone-target-exists-error]
            # Clone targets are create-only. The first clone to a fresh path
            # succeeds; a second clone onto the now-existing target fails. The
            # failure surfaces from the blocking call as an OvstageError whose
            # op-level code is OP_FAILED, with the reason ("already exists") in
            # the message.
            stage.clone("/World/Source", ["/World/Target"], ordinal=2)
            stage.advance_write_floor(ordinal=2).wait()
            assert _prim_count(stage, "/World/Target") == 1

            with pytest.raises(OvstageError) as exc:
                stage.clone("/World/Source", ["/World/Target"], ordinal=3)
            assert exc.value.code == ErrorCode.OP_FAILED
            assert "already exists" in exc.value.message
            # [/snippet:clone-target-exists-error]
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)
