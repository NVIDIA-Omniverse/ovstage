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
# One timeline, two data sources, one mapping. A saxpy simulator steps three
# sphere prims with NON-UNIFORM dt (position += velocity * dt), and a
# time-sampled USD clip animates a conveyor. ovstage stores no time -- it
# only orders writes by opaque uint64 ordinals -- so the application owns an
# explicit ordinal <-> time table and uses it to land BOTH sources in the
# same ordinal slot per tick: the sim write and the clip sample for the same
# simulation time share one ordinal. Because the slots are shared, anything
# a consumer reads at a sealed ordinal describes one instant of time. A
# final tick rewinds the clip: USD sampling time is playback policy, free to
# diverge from the timeline. Verifying read-back values is the test suite's
# job, not this example's.
#
# Expected output: see README.md. Snippet markers are referenced by the
# skills under ../../../skills/ -- keep them intact.

"""ovstage time-and-ordinals example (app-owned ordinal <-> time table, two time-coherent sources)."""

import sys

import numpy as np

import ovstage
from ovstage import (AttributeSemantic, DLDataType, DLDataTypeCode, OvstageError,
                     PopulationDomain, Scope, make_dltensor, population)

# A short time-sampled clip, inline: xformOp:translate.x animates linearly
# 0 -> 120 across timecodes 0..12 with timeCodesPerSecond = 16, so
# translate.x = 160 * t seconds. Prim bodies must be multi-line -- a
# single-line "def X { ... }" is a parse error.
CLIP_USDA = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1.0
    upAxis = "Y"
    timeCodesPerSecond = 16
    startTimeCode = 0
    endTimeCode = 12
)

def Xform "World"
{
    def Cube "Conveyor"
    {
        double size = 1.0

        double3 xformOp:translate.timeSamples = {
            0: (0, 1, 0),
            12: (120, 1, 0),
        }
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
"""

# Non-uniform step sizes (all exact binary fractions, so every printed time
# and position is exact). Ordinals are ordinal = tick + 1; because dt varies,
# time is NOT a formula of the ordinal -- the app must keep the table.
TICKS = 6
DT_OF_TICK = [0.125, 0.125, 0.0625, 0.0625, 0.25, 0.125]
TIMECODES_PER_SECOND = 16.0  # authored in the clip's layer metadata
REWIND_USD_TIME = 0.125      # section 5 points the clip clock back here
SPHERE_PATHS = ["/World/Sphere_0", "/World/Sphere_1", "/World/Sphere_2"]
START = np.array([[0.0, 0.0, 0.0], [10.0, 0.0, 0.0], [20.0, 0.0, 0.0]], np.float32)
VELOCITY = np.array([[8.0, 0.0, 0.0], [16.0, 0.0, 0.0], [24.0, 0.0, 0.0]], np.float32)


def main() -> int:
    if not population.available():
        print("ovstage was built without the population bridge.")
        return 1

    # ---- 1. setup: instance, tokens, the time table, clip + t=0 state at ordinal 1 ----
    # A Stage owns the ovstage instance; its path dictionary is instance-owned.
    with ovstage.Stage("example.time-and-ordinals") as stage, ovstage.PathDictionary(stage) as paths:
        position = paths.intern_token("sim:position")
        velocity = paths.intern_token("sim:velocity")
        sphere_paths = paths.create_path_list_from_strings(SPHERE_PATHS)
        sphere_query = stage.query_from_path_list(sphere_paths)

        # [snippet:time-to-ordinal-table]
        # ovstage stores no time; its ordering key is an opaque uint64 ordinal.
        # The app owns the mapping, and because dt varies it is a TABLE, not a
        # formula: time_of_ordinal[ordinal] is the simulation time whose state
        # that ordinal holds. Ordinal 1 holds the t = 0 state; tick N lands at
        # N + 1.
        time_of_ordinal = [0.0] * (TICKS + 3)
        for tick in range(1, TICKS + 1):
            time_of_ordinal[tick + 1] = time_of_ordinal[tick] + DT_OF_TICK[tick - 1]
        # [/snippet:time-to-ordinal-table]
        print("time model: non-uniform dt; the app owns an ordinal <-> time table (ordinal = tick + 1)")
        print("clip: conveyor translate.x animates 0 -> 120 over 12 timecodes @ 16 codes/s"
              " (translate.x = 160*t)")

        # Populate the clip at ordinal 1, evaluated at USD time 0.0 s, and
        # write the spheres' t=0 state at the same ordinal: both sources share
        # the one ordinal axis from the start. Sealing ordinal 1 makes it
        # readable.
        population.open_usd_from_string(stage, CLIP_USDA, ordinal=1, time_code=0.0,
                                        domains=PopulationDomain.ALL)

        # [snippet:float3-attribute-write]
        # One float3 per prim travels as a single tensor of dtype lanes=3 /
        # shape=[prim count], not a 2-D [3][3] of lanes=1. The semantic stamps
        # the authored USD interpretation on the column: POINT for positions,
        # VECTOR for velocities. UPSERT creates the prims on first write.
        stage.write_attribute(sphere_query, position, ordinal=1, tensors=_float3_tensor(START),
                              is_array=False, semantic=AttributeSemantic.POINT).wait()
        stage.write_attribute(sphere_query, velocity, ordinal=1, tensors=_float3_tensor(VELOCITY),
                              is_array=False, semantic=AttributeSemantic.VECTOR).wait()
        # [/snippet:float3-attribute-write]

        stage.advance_write_floor(ordinal=1).wait()
        print(f"setup: clip populated + {len(SPHERE_PATHS)} spheres written at t = 0.0000 s"
              " (ordinal 1); write floor -> 1")

        # ---- 2. tick loop: land both sources at the tick's ordinal and seal it ----
        # Per tick the app steps the saxpy sim (position += velocity * dt),
        # writes it at the tick's ordinal, and re-samples the clip at the same
        # simulation time (population and direct writes share the one ordinal
        # axis). Advancing the write floor seals the tick for consumers. Each
        # write is waited, so one position buffer is reused.
        sim_pos = START.copy()
        for tick in range(1, TICKS + 1):
            dt = DT_OF_TICK[tick - 1]
            t = time_of_ordinal[tick + 1]  # this tick's simulation time...
            ordinal = tick + 1             # ...lands at this ordinal
            sim_pos += VELOCITY * np.float32(dt)  # saxpy: x += v * dt
            stage.write_attribute(sphere_query, position, ordinal=ordinal,
                                  tensors=_float3_tensor(sim_pos), is_array=False,
                                  semantic=AttributeSemantic.POINT).wait()
            population.update_from_usd_time(stage, ordinal=ordinal, time_code=t)
            stage.advance_write_floor(ordinal=ordinal, scope=Scope.ALL).wait()
            print(f"tick {tick} (dt = {dt:.4f} s) -> t = {t:.4f} s"
                  f" = timecode {t * TIMECODES_PER_SECOND:2.0f} -> ordinal {ordinal}:"
                  " sim + clip written, sealed")

        # ---- 3. USD time is playback policy: rewind the clip ----
        # The clip's clock is not the timeline's. apply_usd_time can point it
        # anywhere -- rewind, loop, hold -- while simulation time and ordinals
        # only ever climb. One more tick: sim time advances to 0.875 s
        # (ordinal 8) but the clip is pointed BACK to usd t = 0.125 s.
        time_of_ordinal[TICKS + 2] = time_of_ordinal[TICKS + 1] + 0.125
        rewind_ordinal = TICKS + 2
        population.update_from_usd_time(stage, ordinal=rewind_ordinal, time_code=REWIND_USD_TIME)
        stage.advance_write_floor(ordinal=rewind_ordinal, scope=Scope.ALL).wait()

        print(f"rewind: sim t = {time_of_ordinal[rewind_ordinal]:.4f} s (ordinal {rewind_ordinal})"
              f" but the clip is pointed at usd t = {REWIND_USD_TIME:.4f} s"
              " -- playback policy, not the timeline")

        # Release the query handles and the path-list references; leaving the
        # `with` blocks then destroys the instance, which requires every op
        # and handle released first.
        stage.release_query(sphere_query).wait()
        paths.destroy_path_list(sphere_paths)
    return 0


def _float3_tensor(rows):
    """One float3 per prim: a single tensor of dtype lanes=3 / shape=[prim count]."""
    data = np.ascontiguousarray(rows, np.float32)
    return make_dltensor(data, dtype=DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3),
                         shape=[len(rows)])


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OvstageError as err:
        print(f"ovstage error (code {err.code}): {err.message}", file=sys.stderr)
        raise SystemExit(1)
