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
        section5_write_options(stage, paths)
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
            print("tint:", values[0], values[1])
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

    # [snippet:clone-duplicate-targets]
    # A target path may appear only once in a clone request.
    try:
        stage.clone_async("/World/Proto/Rig", ["/World/Duplicate/Rig", "/World/Duplicate/Rig"],
                          ordinal=4).wait()
    except OvstageError as error:
        if error.code != ovstage.ErrorCode.INVALID_ARGUMENT:
            raise
        print("Duplicate targets rejected before cloning.")
        # Duplicate targets rejected before cloning.
    else:
        raise RuntimeError("Expected duplicate targets to be rejected")
    # [/snippet:clone-duplicate-targets]

    # [snippet:clone-async-report]
    # Submission returns an operation; wait when the copies are needed.
    operation = stage.clone_async("/World/Proto/Rig", ["/World/Async/Rig"], ordinal=4)
    # Independent application work can run here while the clone is pending.
    report = operation.wait().result()
    print("Async clone copied", report.prims_cloned, "prim to", report.target_count, "target")
    # Async clone copied 1 prim to 1 target
    # Advance the watermark before reading copied values.
    # [/snippet:clone-async-report]

    # [snippet:clone-specific-failure]
    operation = stage.clone_async("/World/Missing", ["/World/MissingCopy"], ordinal=4)
    if not operation.ok:
        operation.wait()  # Raise a submission error before trying to fetch a report.
    try:
        # Fetch waits for completion and preserves the clone-specific failure code.
        # Do not call operation.wait() first: it releases the native report.
        stage.fetch_clone_result(operation)
    except OvstageError as error:
        if error.code != ovstage.ErrorCode.NOT_FOUND:
            raise
        print("Clone source was not found.")
        # Clone source was not found.
    else:
        raise RuntimeError("Expected a missing-source clone failure")
    finally:
        stage.release_op(operation.op_id)  # Fetching the report does not release it.
    # [/snippet:clone-specific-failure]



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
        # both. This unplaced read returns CPU tensors, so the read-back lands on the CPU.
        gpu_samples = paths.intern_token("gpu-samples")
        # [snippet:dlpack-cuda-ingest]
        # Finish producing GPU data, then retain it until the write completes.
        device_values = wp.array(np.array([5.0, 6.0, 7.0], np.float32), dtype=wp.float32, device="cuda:0")
        wp.synchronize()
        write = stage.write_attribute(
            query, gpu_samples, ordinal=8, tensors=device_values, is_array=False
        )
        # Keep device_values alive and unchanged while the write is in progress.
        write.wait()
        del device_values  # Safe to release our reference after completion.
        # [/snippet:dlpack-cuda-ingest]
        stage.advance_write_floor(ordinal=8).wait()

        print("gpu-samples read back on cpu:", _latest_values(stage, paths, query, gpu_samples, 8, order))
        # [/snippet:gpu-warp-ingest]
    paths.destroy_path_list(plist)


def section5_write_options(stage, paths) -> None:
    """Demonstrate write options over the same three prims in a known order."""
    with paths.create_path_list_from_strings(["/World/Writes/A", "/World/Writes/B", "/World/Writes/C"]) as plist:
        with stage.query_from_path_list(plist) as query:
            # [snippet:write-parameters]
            # One scalar per prim; wait while values is still alive.
            values = np.array([10, 20, 30], dtype=np.float32)
            stage.write_attribute(query, "heat", ordinal=9, tensors=values, is_array=False).wait()
            # [/snippet:write-parameters]

            # [snippet:write-fixed-shape]
            # Three rows, each containing one three-component vector.
            velocities = np.array([[1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=np.float32)
            stage.write_attribute(query, "velocity", ordinal=10,
                                  tensors=velocities, is_array=False).wait()
            # [/snippet:write-fixed-shape]

            # [snippet:write-array-width]
            # Two float3 elements for A, one for B, and three for C.
            points = [
                np.array([[0, 0, 0], [1, 0, 0]], dtype=np.float32),
                np.array([[0, 1, 0]], dtype=np.float32),
                np.array([[0, 0, 1], [1, 0, 1], [2, 0, 1]], dtype=np.float32),
            ]
            tensors = [make_dltensor(row,
                dtype=DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3),
                shape=[len(row)]) for row in points]
            stage.write_attribute(query, "points", ordinal=11,
                                  tensors=tensors, is_array=True).wait()
            # [/snippet:write-array-width]

            # [snippet:write-index-map]
            # Reuse source row 0 for A and B, and source row 1 for C.
            values = np.array([100, 200], dtype=np.float32)
            stage.write_attribute(query, "heat", ordinal=12, tensors=values,
                                  is_array=False, index_map=[0, 0, 1], count=3).wait()
            # A = 100, B = 100, C = 200.
            # [/snippet:write-index-map]

            # [snippet:write-mask]
            # Only bit 1 is set, so only B receives a value. A and C keep theirs.
            values = np.array([0, 250, 0], dtype=np.float32)
            stage.write_attribute(query, "heat", ordinal=13, tensors=values,
                                  is_array=False, mask=[0b010], count=3).wait()
            # A = 100, B = 250, C = 200.
            # [/snippet:write-mask]

            # [snippet:write-count]
            # Address just the first two prims in query order: A and B.
            values = np.array([300, 400], dtype=np.float32)
            stage.write_attribute(query, "heat", ordinal=14, tensors=values,
                                  is_array=False, count=2).wait()
            # A = 300, B = 400, C = 200.
            # [/snippet:write-count]

            # [snippet:write-semantic]
            # TOKEN_ID requires interned token identifiers stored as uint64 values.
            materials = np.array([paths.intern_token(name) for name in ["steel", "rubber", "wood"]],
                                 dtype=np.uint64)
            stage.write_attribute(query, "material", ordinal=15, tensors=materials,
                                  is_array=False, semantic=AttributeSemantic.TOKEN_ID).wait()
            # [/snippet:write-semantic]

            # [snippet:write-watermark]
            # Make heat writes through ordinal 14 available for reads.
            heat = paths.intern_token("heat")
            stage.advance_write_floor(ordinal=14, scope=ovstage.Scope.INCLUDE, attributes=[heat]).wait()
            # Finish all attribute writes through ordinal 15, including material.
            stage.advance_write_floor(ordinal=15).wait()
            # [/snippet:write-watermark]

            # [snippet:read-copy-groups]
            # Copy selected values before releasing each group; copies outlive the read.
            heat = paths.intern_token("heat")
            saved = []
            with stage.read_attributes(query, [heat], OrdinalRange.latest(15)) as read:
                read.wait()
                for group in read.groups():
                    with group:
                        rows = group.array(0)
                        for i in range(group.prim_count):
                            row = group.data_row_index(i) if group.has_data_index_map else i
                            saved.append(float(rows[row]))
            # Sorting is for display; read groups need not arrive in query order.
            print("Copied heat values:", sorted(saved))
            # Copied heat values: [200.0, 300.0, 400.0]
            # [/snippet:read-copy-groups]

            # [snippet:read-fixed-vectors]
            velocity = paths.intern_token("velocity")
            with stage.read_attributes(query, [velocity], OrdinalRange.latest(15)) as read:
                read.wait()
                for group in read.groups():
                    with group:
                        # Raw and NumPy shapes are (N, 3), with scalar lanes.
                        vectors = np.from_dlpack(group.dlpack(0))
                        for i in range(group.prim_count):
                            row = group.data_row_index(i) if group.has_data_index_map else i
                            print("Velocity:", vectors[row].tolist())
                            # One of [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0].
            # [/snippet:read-fixed-vectors]

            # [snippet:read-change-range]
            # Only A and B changed heat at ordinal 14; C last changed at ordinal 12.
            heat = paths.intern_token("heat")
            changed = []
            with stage.read_attributes(query, [heat], OrdinalRange.between(14, 14)) as read:
                read.wait()
                for group in read.groups():
                    with group:
                        if group.is_delete:
                            continue  # Deletions carry no value tensor.
                        values = group.array(0)
                        for i in range(group.prim_count):
                            row = group.data_row_index(i) if group.has_data_index_map else i
                            changed.append(float(values[row]))
            print("Heat values changed at 14:", sorted(changed))
            # Heat values changed at 14: [300.0, 400.0]
            # [/snippet:read-change-range]

            # [snippet:read-retained-records]
            # Query the boundary before relying on exact membership of an old interval.
            with stage.get_oldest_preserved_ordinal() as ordinal_query:
                ordinal_query.wait()
                oldest = ordinal_query.fetch()
            requested_start = 14
            if requested_start < oldest:
                raise RuntimeError("Change records for this interval are no longer exact")
            # A subsequent range read can still fail if a newer write replaced its values.
            # [/snippet:read-retained-records]



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
