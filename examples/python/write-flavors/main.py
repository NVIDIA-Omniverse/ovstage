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
# A tour of the higher-level ovstage write workflows: batched multi-attribute
# writes, clone, pipelined (submit-ahead) submission, and GPU ingest. Each
# section writes at its own ordinals, seals them with advance_write_floor, reads
# back, and prints. The GPU ingest section needs warp and a CUDA device; without
# them it prints a skip line.
#
# The fine-grained write *contracts* (column shapes, semantics, UPSERT/INSERT
# admission, sparse index_map/mask, delete tombstones, CPU map/unmap) are asserted
# by the public tests under ../../../tests/ -- see that tree's AGENTS.md.
#
# Expected output: see README.md. Snippet markers are referenced by the skills
# under ../../../skills/ -- keep them intact.

"""ovstage write workflows: batched writes, clone, pipelined submission, and GPU ingest."""

from contextlib import contextmanager

# [snippet:setup]
import numpy as np

import ovstage
from ovstage import (AttributeSemantic, DLDataType, DLDataTypeCode, OrdinalRange, OvstageError,
                     PrimMode, WriteDesc, make_dltensor)
# [/snippet:setup]

# One float3 storage type: the per-element tuple width is carried in dtype.lanes
# (a 3-lane float32), NOT in the tensor shape.
FLOAT3 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)


def main() -> int:
    # One call per numbered section, in print order. Unexpected API failures
    # raise OvstageError and abort the run (fail-fast).
    with ovstage.Stage("example.write-flavors") as stage, ovstage.PathDictionary(stage) as paths:
        section1_batched_writes(stage, paths)
        section2_clone(stage, paths)
        section3_pipelined_submission(stage, paths)
        section4_gpu_ingest(stage, paths)
    return 0


# ---- 1. batched writes ----
def section1_batched_writes(stage, paths) -> None:
    print("== 1. batched writes ==")
    order = ["/World/Batch/A", "/World/Batch/B"]
    plist = paths.create_path_list_from_strings(order)
    with stage.query_from_path_list(plist) as query:
        # [snippet:batched-write-attributes]
        # write_attributes lands several attribute columns in ONE operation: one op id
        # groups completion and one structural precreate covers every entry. It is a
        # grouping, not an atomic transaction -- entries may apply incrementally.
        heat = paths.intern_token("heat")
        tint = paths.intern_token("tint")
        stage.write_attributes(query, [
            WriteDesc(attribute=heat, tensors=np.array([7.0, 8.0], np.float32), is_array=False),
            WriteDesc(attribute=tint, tensors=_float3_rows([0.1, 0.2, 0.3, 0.4, 0.5, 0.6]),
                      is_array=False, semantic=AttributeSemantic.COLOR),
        ], ordinal=1).wait()
        stage.advance_write_floor(ordinal=1).wait()
        # [/snippet:batched-write-attributes]

        print("heat:", _latest_values(stage, paths, query, heat, 1, order))
        with _single_group(stage, query, tint, 1) as group:
            values = group.array(0)
            print("tint:", values[0:3], values[3:6])
    paths.destroy_path_list(plist)


# ---- 2. clone ----
def section2_clone(stage, paths) -> None:
    print("== 2. clone ==")
    proto_list = paths.create_path_list_from_strings(["/World/Proto/Rig"])
    with stage.query_from_path_list(proto_list) as proto_query:
        # [snippet:clone-and-requery]
        # clone stamps the subtree under one source path onto N target paths in one
        # ordinal-keyed call (the multi-environment pattern). The source must exist;
        # each target must not. Build the readback query AFTER the clone: clone changes
        # which prims exist, and a fresh path-list query pins the clones themselves.
        mass = paths.intern_token("mass")
        stage.write_attribute(proto_query, mass, ordinal=2, tensors=np.array([5.0], np.float32),
                              is_array=False).wait()
        stage.advance_write_floor(ordinal=2).wait()

        stage.clone("/World/Proto/Rig", ["/World/Env0/Rig", "/World/Env1/Rig"], ordinal=3)
        stage.advance_write_floor(ordinal=3).wait()

        clone_order = ["/World/Env0/Rig", "/World/Env1/Rig"]
        clone_list = paths.create_path_list_from_strings(clone_order)
        with stage.query_from_path_list(clone_list) as clone_query:
            rows = _latest_rows(stage, paths, clone_query, mass, 3)
            for path in clone_order:
                print("mass", path, "=", rows[path])
        paths.destroy_path_list(clone_list)
        # [/snippet:clone-and-requery]
    paths.destroy_path_list(proto_list)


# ---- 3. pipelined submission ----
def section3_pipelined_submission(stage, paths) -> None:
    print("== 3. pipelined submission ==")
    order = ["/World/Pipelined/A", "/World/Pipelined/B", "/World/Pipelined/C"]
    plist = paths.create_path_list_from_strings(order)
    with stage.query_from_path_list(plist) as query:
        # [snippet:pipelined-submission]
        # Every write so far enqueued and immediately waited, but the enqueue
        # itself is asynchronous: it returns an Operation right away, so a
        # producer can submit several ordinals ahead WITHOUT waiting and keep
        # the CPU busy while the stage executes. This shows the programming
        # model, not a speedup -- current releases may execute enqueued
        # operations serially. Client memory must stay valid until the op
        # completes, hence one buffer per in-flight write; the bindings pin
        # each caller-owned tensor on its returned Operation.
        sample = paths.intern_token("sample")
        pending = []
        for n in range(4):
            batch = np.array([100 * (n + 1) + i for i in range(3)], np.float32)
            pending.append(stage.write_attribute(query, sample, ordinal=4 + n, tensors=batch,
                                                 is_array=False, prim_mode=PrimMode.UPSERT))
        # [/snippet:pipelined-submission]
        print("4 writes enqueued (ordinals 4..7) with zero waits; the CPU stays busy meanwhile")

        # [snippet:poll-wait-release]
        # Drain without blocking: poll each op (in submission order) with the
        # low-level Stage.wait_op(timeout=0); it returns (code, error_op_ids,
        # lowest_pending_op_id). A TIMEOUT code means "still executing" -- a
        # real application does more CPU work and polls again (while pending,
        # lowest_pending names the op the waited chain is stalled on).
        # Completed ops are retired with release_op.
        for op in pending:
            if not op.ok:
                raise OvstageError(op.status, op.error_message())  # enqueue was rejected
            while True:
                code, error_op_ids, lowest_pending = stage.wait_op(op.op_id, timeout=0)
                if code == ovstage.ErrorCode.TIMEOUT:
                    continue  # still executing: do other CPU work, poll again
                if code != ovstage.ErrorCode.OK:
                    msg = op.error_message()  # read while the op is alive -- it's released next
                    stage.release_op(op.op_id)
                    raise OvstageError(code, msg)
                stage.release_op(op.op_id)
                break
        stage.advance_write_floor(ordinal=7).wait()
        # [/snippet:poll-wait-release]
        print("all 4 drained by zero-timeout polls and released; floor -> 7")

        # Example plumbing: print integer-style to match the C sibling's %.0f.
        values = _latest_values(stage, paths, query, sample, 7, order)
        print("latest sample after the pipeline:", " ".join(f"{v:.0f}" for v in values))
    paths.destroy_path_list(plist)


# ---- 4. GPU ingest (Python-only) ----
def section4_gpu_ingest(stage, paths) -> None:
    print("== 4. GPU ingest (Python-only) ==")
    wp = _load_warp()
    if wp is None:
        print("gpu ingest skipped: warp with a CUDA device is not available")
        return
    order = ["/World/Gpu/A", "/World/Gpu/B", "/World/Gpu/C"]
    plist = paths.create_path_list_from_strings(order)
    with stage.query_from_path_list(plist) as query:
        # [snippet:gpu-warp-ingest]
        # write_attribute ingests any DLPack producer -- a warp CUDA array's device
        # buffer is handed to ovstage directly, no host round-trip on the write leg.
        # Synchronize the producing device first (or pass a cuda_event) and keep the
        # source array alive until the write's Operation completes; .wait() covers
        # both. Reads always return CPU tensors, so the read-back lands on the CPU.
        gpu_samples = paths.intern_token("gpu-samples")
        device_values = wp.array(np.array([5.0, 6.0, 7.0], np.float32), dtype=wp.float32, device="cuda:0")
        wp.synchronize()
        stage.write_attribute(query, gpu_samples, ordinal=8, tensors=device_values, is_array=False).wait()
        stage.advance_write_floor(ordinal=8).wait()

        print("gpu-samples read back on cpu:", _latest_values(stage, paths, query, gpu_samples, 8, order))
        # [/snippet:gpu-warp-ingest]
    paths.destroy_path_list(plist)


# ---- plumbing ----
def _float3_rows(values) -> "ovstage.DLTensor":
    """Wrap a flat float array as one 3-lane element per prim (shape = [len/3])."""
    data = np.asarray(values, np.float32)
    return make_dltensor(data, dtype=FLOAT3, shape=[len(data) // 3])


def _latest_rows(stage, paths, query, attr, end_ordinal):
    """Latest committed value per prim of a 1-lane float column, as {path: value}.

    Robust to how the backend groups the result: a latest read may cover the
    query's prims through several groups, each carrying a prim index map (which
    prims of its list are covered) and a data index map (which tensor row holds
    each covered prim's value). Delete groups (tombstones) carry no data.
    """
    rows = {}
    with stage.read_attributes(query, [attr], OrdinalRange.latest(end_ordinal)) as read:
        read.wait()
        for group in read.groups():
            if not group.is_delete:
                group_paths = paths.get_paths(group.prim_list)
                values = group.array(0)  # zero-copy view; valid until release_group
                for local in range(group.prim_count):
                    path = paths.path_to_string(group_paths[group.prim_index(local)])
                    row = group.data_row_index(local) if group.has_data_index_map else local
                    rows[path] = float(values[row])
            stage.release_group(group)
    return rows


def _latest_values(stage, paths, query, attr, end_ordinal, order):
    """The `_latest_rows` values as a float32 array in `order` (path-string order)."""
    rows = _latest_rows(stage, paths, query, attr, end_ordinal)
    return np.array([rows[path] for path in order], np.float32)


@contextmanager
def _single_group(stage, query, attr, end_ordinal):
    """The one read group of a dense column just written at a single ordinal
    over the whole query (the general, multi-group case is `_latest_rows`)."""
    with stage.read_attributes(query, [attr], OrdinalRange.latest(end_ordinal)) as read:
        read.wait()
        group = read.fetch_next()
        if group is None:
            raise SystemExit(f"read returned no group at ordinal {end_ordinal}")
        try:
            yield group
        finally:
            stage.release_group(group)


def _load_warp():
    """Return the warp module if a usable release imports and a CUDA device is present, else None."""
    try:
        import warp as wp
        wp.config.log_level = wp.LOG_WARNING  # keep the init banner off stdout
    except ImportError:
        return None
    except AttributeError:  # older warp without these knobs cannot keep stdout deterministic
        return None
    wp.init()
    return wp if wp.get_cuda_devices() else None


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OvstageError as err:
        print(f"ovstage error (code {int(err.code)}): {err.message}")
        raise SystemExit(1)
