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
# A producer writes a "temperature" column one tick at a time, sealing each
# tick by advancing the write floor; a pull-based consumer polls that floor
# and range-reads only the delta -- "what changed since the last ordinal it
# saw" -- including a whole-prim delete that arrives as a tombstone group.
#
# Run: python main.py (deterministic interleaved mode) or python main.py
# --threads (concurrent mode; output varies run to run). Expected output:
# see README.md. Snippet markers are referenced by the skills under
# ../../../skills/ -- keep them intact.

"""Producer/consumer over one ovstage: sealed ticks, pull-based delta reads, tombstones."""

# [snippet:setup]
import sys
import threading
import time

import numpy as np

import ovstage
from ovstage import OrdinalRange, OvstageError
# [/snippet:setup]

# Four "sensor" prims, created on demand by the first UPSERT write that
# targets them. S0..S3 in the prints = last path component.
SENSOR_PATHS = [
    "/World/Sensors/S0",
    "/World/Sensors/S1",
    "/World/Sensors/S2",
    "/World/Sensors/S3",
]

# Six ticks, each writing a rotating two-prim subset (the changing membership
# keeps the consumer's deltas interesting). Tick 5 deletes sensor S2 entirely
# instead of writing values, so it has no entry here.
TICK_SUBSETS = {1: (0, 1), 2: (1, 2), 3: (2, 0), 4: (3, 0), 6: (1, 3)}
DELETE_TICK = 5
DELETED_SENSOR = 2
TICKS = 6


def main() -> int:
    threaded = "--threads" in sys.argv[1:]

    # ---- setup: create the stage, intern S0..S3, open queries ----
    with ovstage.Stage("example.producer-consumer") as stage, ovstage.PathDictionary(stage) as paths:
        temperature = paths.intern_token("temperature")

        # The consumer's query covers all four sensors; the producer holds one
        # single-prim query per sensor so each tick targets just its subset.
        all_paths = paths.create_path_list_from_strings(SENSOR_PATHS)
        sensor_paths = [paths.create_path_list_from_strings([p]) for p in SENSOR_PATHS]
        consumer_query = stage.query_from_path_list(all_paths)
        sensor_queries = [stage.query_from_path_list(pl) for pl in sensor_paths]

        ok = True
        if threaded:
            # ---- concurrent mode (--threads): both roles on one stage ----
            ok = run_threaded(stage, paths, consumer_query, sensor_queries, temperature)
        else:
            last_seen = 0

            # ---- producer ticks 1..3 ----
            for tick in (1, 2, 3):
                run_producer_tick(stage, sensor_queries, temperature, tick)

            # ---- consumer catch-up: reads delta [1, 3] ----
            last_seen = consume_deltas(stage, paths, consumer_query, temperature, last_seen)

            # ---- producer ticks 4..6 (tick 5 deletes S2) ----
            for tick in (4, 5, 6):
                run_producer_tick(stage, sensor_queries, temperature, tick)

            # ---- consumer catch-up: reads delta [4, 6] ----
            consume_deltas(stage, paths, consumer_query, temperature, last_seen)

        # Happy-path cleanup: Stage teardown requires all handles released first.
        for query in [consumer_query, *sensor_queries]:
            query.release().wait()
        for path_list in [all_paths, *sensor_paths]:
            paths.destroy_path_list(path_list)

    return 0 if ok else 1


def run_producer_tick(stage, sensor_queries, temperature, tick):
    """Dispatch one producer tick: value ticks write their subset; the delete tick tombstones."""
    if tick == DELETE_TICK:
        producer_delete_tick(stage, sensor_queries, tick)
    else:
        producer_value_tick(stage, sensor_queries, temperature, tick)


# [snippet:producer-tick]
def producer_value_tick(stage, sensor_queries, temperature, tick):
    """Write this tick's subset at ordinal ``tick`` (several writes may share
    one ordinal), then seal the tick by advancing the GLOBAL write floor -- the
    producer's publish signal: data at ordinals <= floor never changes. The
    application owns the ordinal counter; the store never mints ordinals."""
    written = []
    for sensor in TICK_SUBSETS[tick]:
        value = 10.0 * tick + sensor
        stage.write_attribute(
            sensor_queries[sensor],
            temperature,
            ordinal=tick,
            tensors=np.array([value], np.float32),
            is_array=False,
        ).wait()
        written.append(f"S{sensor} = {value:.1f}")
    stage.advance_write_floor(ordinal=tick).wait()
    print(f"producer tick {tick}: wrote {', '.join(written)} (floor -> {tick})")
# [/snippet:producer-tick]


# [snippet:tombstone-delete]
def producer_delete_tick(stage, sensor_queries, tick):
    """delete_attributes with an EMPTY attribute list deletes the prim
    entirely. Like any write it is ordinal-keyed and sealed by the same floor
    advance; consumers see it as an is_delete read group (no tensors)."""
    stage.delete_attributes(sensor_queries[DELETED_SENSOR], [], ordinal=tick).wait()
    stage.advance_write_floor(ordinal=tick).wait()
    print(f"producer tick {tick}: deleted S{DELETED_SENSOR} entirely (floor -> {tick})")
# [/snippet:tombstone-delete]


# [snippet:consume-delta]
def read_delta_groups(stage, paths, query, temperature, begin, end):
    """Range-read [begin, end] over the query and return the result as-is,
    group by group: (ordinal, is_delete, paths, values). The EXPLICIT begin
    makes it a delta ("what changed since begin - 1"); an open begin would be
    a snapshot read. Payloads are the LATEST-COMMITTED values, and ordinal is
    the column's latest write ordinal -- it can exceed the requested range
    end. Tombstone groups carry no tensors, so their values list is empty."""
    groups = []
    with stage.read_attributes(query, [temperature], OrdinalRange.between(begin, end)) as read:
        read.wait()
        for group in read.groups():
            # prim_index(local) resolves the group's offset/index_map to a
            # position in the group's OWN prim list (the query's pinned copy,
            # in query order), so resolve paths through the dictionary.
            group_paths = paths.get_path_strings(group.prim_list)
            covered = [group_paths[group.prim_index(i)] for i in range(group.prim_count)]
            values = []
            if not group.is_delete:
                # One latest-committed row per covered prim; the row index
                # honors data.index_map when present.
                data = group.array(0)
                for local in range(group.prim_count):
                    row = group.data_row_index(local) if group.has_data_index_map else local
                    values.append(float(data[row]))
            groups.append((group.ordinal, group.is_delete, covered, values))
            stage.release_group(group)
    return groups


def consume_deltas(stage, paths, query, temperature, last_seen) -> int:
    """One consumer catch-up: fetch the global floor; if it moved past
    last_seen, range-read the delta [last_seen + 1, floor] and report what
    changed."""
    floor = poll_write_floor(stage)
    if floor <= last_seen:
        return last_seen  # nothing sealed since the last catch-up

    print(f"consumer: floor {floor}, last_seen {last_seen} -> reading delta [{last_seen + 1}, {floor}]")
    groups = read_delta_groups(stage, paths, query, temperature, last_seen + 1, floor)

    # Example plumbing: report each group; S0..S3 = last path component. Note
    # the tombstone group prints ordinal 6 though the delete landed at 5 --
    # ordinal is the column's latest write ordinal, not a range clamp.
    changes = 0
    for ordinal, is_delete, group_paths, values in groups:
        names = [path.rsplit("/", 1)[-1] for path in group_paths]
        if is_delete:
            print(f"  tombstone group (ordinal {ordinal}): {', '.join(names)} deleted")
        else:
            cells = [f"{name} = {value:.1f}" for name, value in zip(names, values)]
            print(f"  value group (ordinal {ordinal}): {', '.join(cells)}")
        changes += len(group_paths)
    print(f"  delta batch: {changes} prim changes")
    print(f"consumer: last_seen -> {floor}")
    return floor  # the consumer's cursor: the next catch-up starts at floor + 1
# [/snippet:consume-delta]


# [snippet:threaded-producer-consumer]
def run_threaded(stage, paths, query, sensor_queries, temperature) -> bool:
    """Concurrent mode (--threads): producer and consumer run on the SAME
    stage from two threads -- the ovstage API is thread-safe on a shared
    instance, and the ctypes bindings release the GIL during calls. The
    consumer only ever reads up to a floor it fetched, so it never observes a
    half-written tick; only the batching varies run to run, which is why this
    mode's output is not part of the expected-output block."""
    failed = threading.Event()

    def produce():
        try:
            for tick in range(1, TICKS + 1):
                if failed.is_set():
                    break  # the consumer flagged a failure: stop producing
                run_producer_tick(stage, sensor_queries, temperature, tick)
                time.sleep(0.002)  # the producer's own rate; lets catch-ups interleave
        except OvstageError as err:
            # Expected race under load: a write is rejected while it overlaps an
            # outstanding consumer read. A stalled floor would hang the consumer,
            # so flag the failure before printing.
            failed.set()
            print(f"producer failed (code {int(err.code)}): {err.message}")
        except Exception as err:
            failed.set()
            print(f"producer failed: {err!r}")

    def consume():
        try:
            last_seen = 0
            while last_seen < TICKS and not failed.is_set():
                last_seen = consume_deltas(stage, paths, query, temperature, last_seen)
                if last_seen < TICKS:
                    time.sleep(0.001)
        except OvstageError as err:
            failed.set()
            print(f"consumer failed (code {int(err.code)}): {err.message}")
        except Exception as err:
            failed.set()
            print(f"consumer failed: {err!r}")

    producer = threading.Thread(target=produce, name="producer")
    consumer = threading.Thread(target=consume, name="consumer")
    producer.start()
    consumer.start()
    producer.join()
    consumer.join()
    return not failed.is_set()
# [/snippet:threaded-producer-consumer]


# ---- plumbing ----

# [snippet:poll-write-floor]
def poll_write_floor(stage) -> int:
    """Fetch the GLOBAL write floor (attribute=None) -- the only coordination
    the consumer needs: everything at or below the floor is sealed, so reading
    up to it can never race an in-flight producer write."""
    with stage.get_attribute_write_floor(None) as floor_query:
        return floor_query.fetch()
# [/snippet:poll-write-floor]


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OvstageError as err:
        print(f"ovstage error (code {int(err.code)}): {err.message}")
        raise SystemExit(1)
