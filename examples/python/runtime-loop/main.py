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
# Headless ovstage runtime loop: load a USD scene (torus-plane.usda) into the
# ovstage runtime table, then exercise the two update paths a client has once
# a scene is live -- write omni:xform straight into the table (24 animation
# frames, no USD round-trip), and edit the USD source (reference a cube) and
# propagate it with apply_usd_changes. The scene is Y-up, so the animation
# slides the Torus along +X, keeping its authored y=25 offset.
#
# Run: python main.py with a population-enabled libovstage on the loader path (see
# README.md). Expected output: see README.md. Snippet markers are referenced
# by the skills under ../../../skills/ -- keep them intact.

"""Headless ovstage runtime loop: populate from USD, read, animate via table + USD, read."""

# [snippet:setup]
import pathlib
import sys

import numpy as np

import ovstage
from ovstage import (AttributeSemantic, DLDataType, DLDataTypeCode, OrdinalRange, OvstageError,
                     PopulationDomain, make_dltensor, population)
# [/snippet:setup]

# The ovrtx minimal example scene, copied next to this file so the example is
# self-contained; open_usd(...) takes any .usda/.usdc path.
SCENE = pathlib.Path(__file__).resolve().parent / "torus-plane.usda"

# A one-prim layer referenced onto a new /World/EditCube below, to show a
# USD-source edit propagating into the already-populated runtime stage.
EDIT_CUBE_USDA = """#usda 1.0
(
    defaultPrim = "Ref"
)

def Cube "Ref"
{
    double size = 1.0
}
"""

FRAMES = 24


def main() -> int:
    if not population.available():
        print("ovstage was built without the population bridge.")
        return 1

    # A Stage owns the ovstage instance; its path dictionary is instance-owned.
    with ovstage.Stage("example.runtime-loop") as stage, ovstage.PathDictionary(stage) as paths:
        prim_type = paths.intern_token("usd-prim-type")
        xform = paths.intern_token("omni:xform")  # canonical 4x4 transform column

        # Stage teardown requires all handles released first, so the queries
        # and path lists below are released in the finally even when an error
        # interrupts the flow. They start as None: an early failure leaves the
        # later ones uncreated, and the finally skips those.
        scene_paths = torus_paths = expanded_paths = None
        scene_query = torus_query = expanded_query = None
        try:
            # ---- 1. populate: USD file -> runtime table at ordinal 1 ----
            # [snippet:populate]
            # Load a USD file and populate the runtime table in one op, at the
            # caller-owned ordinal 1, then seal that ordinal so reads can see it.
            # The application owns the ordinal lifecycle; population never opens
            # its own.
            population.open_usd(
                stage, str(SCENE), ordinal=1, time_code=0.0, domains=PopulationDomain.RENDERING
            )
            stage.advance_write_floor(ordinal=1).wait()
            # [/snippet:populate]

            scene_paths = paths.create_path_list_from_strings(
                ["/World", "/World/Plane", "/World/Torus"])
            torus_paths = paths.create_path_list_from_strings(["/World/Torus"])
            scene_query = stage.query_from_path_list(scene_paths)
            torus_query = stage.query_from_path_list(torus_paths)

            # ---- 2. read back: confirm the populate landed ----
            # [snippet:read-populated]
            # Read the reserved usd-prim-type metadata ovstage auto-maintains for
            # every populated prim.
            print("populated prim types:", ", ".join(_prim_types(stage, paths, scene_query, prim_type, 1)))
            # [/snippet:read-populated]

            # ---- 3. update path 1: animate straight into the table ----
            # [snippet:update-table]
            # Write the Torus transform into the ovstage table over 24 frames (one
            # ordinal per frame), no USD round-trip. omni:xform is a 4x4 matrix
            # column -> semantic=MATRIX.
            for frame in range(FRAMES):
                ordinal = 2 + frame
                tx = 100.0 * frame / (FRAMES - 1)  # 0 -> 100 across the animation
                stage.write_attribute(
                    torus_query, xform, ordinal=ordinal,
                    tensors=_translate_matrix(tx, 25.0, 0.0),
                    is_array=False, semantic=AttributeSemantic.MATRIX,
                ).wait()
                stage.advance_write_floor(ordinal=ordinal).wait()

            # Read back the final frame's transform (our own written column).
            # The read is a context manager -- block exit releases its handle even
            # when a guard raises -- and the fetched group is released in a finally.
            with stage.read_attributes(torus_query, [xform], OrdinalRange.latest(2 + FRAMES - 1)) as read:
                read.wait()
                group = read.fetch_next()
                if group is None:  # fetch_next() returns None when the read yields no group
                    raise SystemExit("no data group returned for the final-frame omni:xform read")
                try:
                    m = np.asarray(group.array(0)).ravel()  # row-major 4x4
                    if m.size != 16:  # single-path query -> exactly one 16-lane matrix element
                        raise SystemExit(f"unexpected omni:xform read layout: {m.size} values, expected 16")
                    print("final Torus xform translation (row [3][0:3]):", m[12:15])  # -> [100. 25. 0.]
                finally:
                    stage.release_group(group)
            # [/snippet:update-table]

            # ---- 4. update path 2: edit the USD source and propagate it ----
            # [snippet:update-usd]
            # Reference a cube onto a new /World/EditCube in the USD source, then
            # propagate the change into the runtime table with apply_usd_changes
            # at a fresh ordinal (above the animation's floor). The runtime stage
            # now sees a prim that existed only in USD.
            usd_ordinal = 2 + FRAMES
            population.add_usd_reference_from_string(stage, EDIT_CUBE_USDA, "/World/EditCube")
            population.apply_usd_changes(stage, ordinal=usd_ordinal)
            stage.advance_write_floor(ordinal=usd_ordinal).wait()

            expanded_paths = paths.create_path_list_from_strings(
                ["/World", "/World/Plane", "/World/Torus", "/World/EditCube"])
            expanded_query = stage.query_from_path_list(expanded_paths)
            print("after USD edit, prim types:",
                  ", ".join(_prim_types(stage, paths, expanded_query, prim_type, usd_ordinal)))
            # [/snippet:update-usd]
        finally:
            # Cleanup runs on every exit path. When an exception is already
            # propagating, releases are best-effort so they cannot mask it;
            # on the normal path a release failure raises like any other API
            # failure (a silent leak here would be invisible).
            failing = sys.exc_info()[0] is not None
            for query in (scene_query, torus_query, expanded_query):
                if query is not None:
                    try:
                        stage.release_query(query).wait()
                    except Exception:
                        if not failing:
                            raise
            for path_list in (scene_paths, torus_paths, expanded_paths):
                if path_list is not None:
                    try:
                        paths.destroy_path_list(path_list)
                    except Exception:
                        if not failing:
                            raise

    return 0


def _translate_matrix(tx, ty, tz):
    """A 4x4 double transform as omni:xform expects it (row-major, translation in
    the last row). This example uses the canonical transport form: one 16-lane
    element per prim. A convenience 4x4 of lanes=1 is accepted on copy-in but
    normalized back to shape [1], lanes=16 on raw reads and maps."""
    m = np.array([1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  tx, ty, tz, 1], dtype=np.float64)
    return make_dltensor(m, dtype=DLDataType(code=DLDataTypeCode.kDLFloat, bits=64, lanes=16), shape=[1])


def _prim_types(stage, paths, query, prim_type_tok, ordinal):
    """Read the reserved usd-prim-type column (one uint64 token id per prim,
    is_array=False) and resolve each token back to its type-name string. The
    read is a context manager and each group is released in a finally, so no
    handle outlives an error."""
    names = []
    with stage.read_attributes(query, [prim_type_tok], OrdinalRange.latest(ordinal)) as read:
        read.wait()
        for group in read.groups():
            try:
                for token_id in group.array(0).tolist():
                    names.append(paths.token_to_string(int(token_id)))
            finally:
                stage.release_group(group)
    return names


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OvstageError as err:
        print(f"ovstage error (code {int(err.code)}): {err.message}")
        raise SystemExit(1)
