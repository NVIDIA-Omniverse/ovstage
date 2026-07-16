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
# Minimal ovstage Python example: create a stage, intern paths/tokens via its
# instance-owned path dictionary, write an attribute column, advance the write
# floor, and read it back.
#
# Run: python main.py with libovstage on the loader path (see README.md).
# Expected output: see README.md. Snippet markers are referenced by the
# skills under ../../../skills/ -- keep them intact.

"""Minimal ovstage Python example (write → advance write floor → read)."""

# [snippet:setup]
import numpy as np

import ovstage
from ovstage import OrdinalRange, OvstageError
# [/snippet:setup]


def main() -> int:
    # ---- 1. setup: stage, instance-owned path dictionary, token, query ----
    # Both are context managers; PathDictionary(stage) borrows the dictionary
    # the instance owns (no app-side create/destroy).
    with ovstage.Stage("example.minimal") as stage, ovstage.PathDictionary(stage) as paths:
        # [snippet:intern-and-resolve]
        # Intern a string -> a stable integer token, and resolve it back.
        attr = paths.intern_token("temperature")
        print("attribute token", attr, "=", paths.token_to_string(attr))
        # [/snippet:intern-and-resolve]

        # [snippet:path-list-query]
        # Build an interned prim-path list and open a query over those prims.
        # The query is a context manager: block exit releases its handle, and
        # Stage teardown requires all handles released first -- the try/finally
        # releases the path list even when an error interrupts the flow.
        prim_paths = paths.create_path_list_from_strings(["/World/A", "/World/B", "/World/C"])
        try:
            with stage.query_from_path_list(prim_paths) as query:
                # [/snippet:path-list-query]

                # ---- 2. write, seal with the write floor, read back ----
                # [snippet:minimal-write-read]
                # Write one float per prim into the "temperature" column at ordinal 1,
                # seal it by advancing the write floor to 1, then read it back. Tensor
                # data crosses as a numpy array (CPU) via DLPack; async ops return an
                # Operation whose .wait() raises OvstageError on failure. The read is
                # a context manager -- block exit releases its handle even when an
                # error interrupts -- and the fetched group is released in a finally.
                stage.write_attribute(
                    query, attr, ordinal=1, tensors=np.array([1.0, 2.0, 3.0], np.float32), is_array=False
                ).wait()
                stage.advance_write_floor(ordinal=1).wait()

                with stage.read_attributes(query, [attr], OrdinalRange.latest(1)) as read:
                    read.wait()
                    group = read.fetch_next()
                    if group is None:
                        raise SystemExit("read returned no group at ordinal 1")
                    try:
                        # group.array(i) is a zero-copy numpy view of tensor i (CPU).
                        print("read back ordinal", group.ordinal, group.array(0))  # -> [1. 2. 3.]
                    finally:
                        stage.release_group(group)
                # [/snippet:minimal-write-read]

                # ---- 3. attribute argument as a plain string ----
                # [snippet:string-or-token-arg]
                # Attribute arguments accept an interned token (int) or a plain str:
                # a token skips the per-call dictionary lookup, a str is interned for
                # you at call time.
                stage.write_attribute(
                    query, "temperature", ordinal=2, tensors=np.array([4.0, 5.0, 6.0], np.float32),
                    is_array=False,
                ).wait()
                stage.advance_write_floor(ordinal=2).wait()
                # [/snippet:string-or-token-arg]

                # ---- 4. clone one prim's subtree to several targets ----
                # [snippet:clone-subtree-multienv]
                # The multi-environment pattern: stamp out N copies of a prototype
                # (e.g. one scene/robot per RL environment) in a single call. Clone is
                # an ordinal-keyed write (pick an ordinal above the current write
                # floor) that copies the source subtree's attributes verbatim; the
                # source must exist and each target must not. clone() blocks and
                # raises OvstageError on failure.
                stage.clone("/World/A", ["/World/A_env0", "/World/A_env1"], ordinal=3)
                stage.advance_write_floor(ordinal=3).wait()  # seal the clones so they're readable
                print("cloned /World/A -> A_env0, A_env1")
                # [/snippet:clone-subtree-multienv]

                # ---- 5. zero-copy tensor interchange via DLPack ----
                # [snippet:dlpack-interchange]
                # ovstage speaks the standard DLPack protocol, so it interoperates
                # zero-copy with numpy / warp / torch / cupy on CPU or CUDA. Ingest:
                # hand write_attribute any object exposing __dlpack__ (keep the source
                # alive until the write's Operation is waited). Export: group.dlpack(i)
                # is a zero-copy view, valid only until the group is released -- copy
                # it if it must outlive the read.
                producer = np.array([7.0, 8.0, 9.0], np.float32)
                stage.write_attribute(
                    query, attr, ordinal=4, tensors=ovstage.DLTensor.from_dlpack(producer),
                    is_array=False,
                ).wait()
                stage.advance_write_floor(ordinal=4).wait()

                with stage.read_attributes(query, [attr], OrdinalRange.latest(4)) as read:
                    read.wait()
                    group = read.fetch_next()
                    if group is None:
                        raise SystemExit("read returned no group at ordinal 4")
                    try:
                        column = np.from_dlpack(group.dlpack(0))
                        # The earlier clone widened the "temperature" column past our
                        # 3 prims, so this view spans every prim in the column and the
                        # group carries a data index_map: narrow to this group's rows
                        # via data_count + data_row_index (a gather -- it copies,
                        # unlike the zero-copy view above). Without an index_map the
                        # group is dense -- data_count is not meaningful then, and the
                        # view's rows are already exactly this group's rows, in order.
                        if group.has_data_index_map:
                            values = np.array(
                                [column[group.data_row_index(i)] for i in range(group.data_count)])
                        else:
                            values = column
                        print("dlpack read back ordinal", group.ordinal, values)  # -> [7. 8. 9.]
                    finally:
                        stage.release_group(group)
                # [/snippet:dlpack-interchange]
        finally:
            # Release the one reference create_path_list_* handed us -- in a
            # finally, so an error anywhere above still leaves the Stage cleanly
            # destroyable. The query's `with` block releases its handle on block
            # exit, and the Stage/PathDictionary context managers release the
            # rest.
            paths.destroy_path_list(prim_paths)

    return 0


# The two reference functions below are not called by main(); they document
# the exception surface and the non-blocking poll pattern.

# [snippet:error-handling]
# ovstage surfaces failures as exceptions: OvstageError for data-plane ops
# (a numeric .code mapping to ovstage.ErrorCode, plus .message) and OvxError
# for path-dictionary calls. Operation.wait() raises OvstageError if the
# enqueue was rejected, or the op (or its ordinal-keyed dependencies) failed.
def write_checked(stage, query, attr, ordinal, values) -> bool:
    try:
        stage.write_attribute(query, attr, ordinal=ordinal, tensors=values, is_array=False).wait()
        return True
    except OvstageError as err:
        print(f"ovstage write failed (code {int(err.code)}): {err.message}")
        return False
# [/snippet:error-handling]


# [snippet:nonblocking-poll]
# Run the CPU ahead of execution: enqueue without blocking, then poll with
# timeout=0 and do other work between polls instead of stalling. The low-level
# Stage.wait_op returns (code, error_op_ids, lowest_pending_op_id); a TIMEOUT
# code means "not ready yet". Release the op once it completes.
def poll_until_done(stage, op) -> None:
    if not op.ok:
        raise OvstageError(op.status, op.error_message())  # enqueue was rejected
    while True:
        code, error_op_ids, lowest_pending = stage.wait_op(op.op_id, timeout=0)
        if code == ovstage.ErrorCode.TIMEOUT:
            continue  # not done yet — go do other CPU work / submit the next ordinal
        if code != ovstage.ErrorCode.OK:
            msg = op.error_message()  # read while the op is alive — it's released next
            stage.release_op(op.op_id)
            raise OvstageError(code, msg)
        stage.release_op(op.op_id)
        return
# [/snippet:nonblocking-poll]


if __name__ == "__main__":
    raise SystemExit(main())
