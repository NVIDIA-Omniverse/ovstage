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

# Concurrent mode (--threads) only. Its overlap phase runs at the sealed
# ordinal OVERLAP_TICK: the consumer keeps that tick's temperature read group
# borrowed while the producer updates S0's "humidity" -- an unrelated
# attribute, created before the threads start -- at the next ordinal.
# WATCHDOG_S is a diagnostic timeout for the coordination waits (waiting for
# the peer); it is not a process-wide execution deadline.
OVERLAP_TICK = 3
HUMIDITY_SENSOR = 0
WATCHDOG_S = 30.0


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
    one ordinal), then seal the tick by advancing the GLOBAL write floor. This
    tells the consumer which ordinals are sealed against further writes; values
    may still change at newer ordinals. The application owns the ordinal counter;
    the store never mints ordinals."""
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
def copy_group(paths, group):
    """Copy one fetched group into application-owned storage:
    (ordinal, is_delete, paths, values). prim_index(local) resolves the
    group's offset/index_map to a position in the group's OWN prim list (the
    query's pinned copy, in query order), so paths resolve through the
    dictionary. The value row honors data.index_map when present. Tombstone
    groups carry no tensors, so their values list is empty."""
    group_paths = paths.get_path_strings(group.prim_list)
    covered = [group_paths[group.prim_index(i)] for i in range(group.prim_count)]
    column = group.columns[0]
    values = []
    if not column.is_delete:
        data = column.array(0)
        for local in range(group.prim_count):
            row = column.data_row_index(local) if column.has_data_index_map else local
            values.append(float(data[row]))
    return (column.ordinal, column.is_delete, covered, values)


def read_delta_groups(stage, paths, query, temperature, begin, end):
    """Range-read [begin, end] over the query and return the result as-is,
    group by group. The EXPLICIT begin makes it a delta ("what changed since
    begin - 1"); an open begin would be a snapshot read. If a selected
    (attribute, path) changed again after end, the read raises OUT_OF_RANGE.
    Each group is copied and released at once: a borrowed group blocks every
    overlapping write for as long as it is held."""
    groups = []
    with stage.read_attributes(query, [temperature], OrdinalRange.between(begin, end)) as read:
        read.wait()
        for group in read.groups():
            groups.append(copy_group(paths, group))
            stage.release_group(group)
    return groups


def report_delta(groups, floor):
    """Example plumbing: report each copied group; S0..S3 = last path component."""
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


def consume_deltas(stage, paths, query, temperature, last_seen) -> int:
    """One consumer catch-up: fetch the global floor; if it moved past
    last_seen, range-read the delta [last_seen + 1, floor] and report what
    changed."""
    floor = poll_write_floor(stage)
    if floor <= last_seen:
        return last_seen  # nothing sealed since the last catch-up

    print(f"consumer: floor {floor}, last_seen {last_seen} -> reading delta [{last_seen + 1}, {floor}]")
    groups = read_delta_groups(stage, paths, query, temperature, last_seen + 1, floor)
    report_delta(groups, floor)
    return floor  # the consumer's cursor: the next catch-up starts at floor + 1
# [/snippet:consume-delta]


# [snippet:threaded-producer-consumer]
class Coordination:
    """Shared state of the concurrent mode (--threads); see run_threaded.

    ``guard`` protects the temperature access scope -- an application choice
    for this example's four-sensor access set, not a stage-wide requirement --
    and ``progress`` publishes the newest sealed tick. ``failed`` is set by whichever role fails; fail()
    wakes every waiter so both roles shut down. The one-time overlap phase
    uses two events plus the observed order of its steps."""

    def __init__(self):
        self.guard = threading.RLock()
        self.progress = threading.Condition(self.guard)
        self.published = 0  # newest sealed tick
        self.done = False  # the producer finished
        self.failed = False
        self.read_held = threading.Event()  # overlap phase: the consumer borrowed the read
        self.humidity_written = threading.Event()  # overlap phase: the unrelated write completed
        self.events = []  # observed order of the overlap phase

    def fail(self):
        # Flag first and wake the phase waiters without the guard: the consumer
        # holds the guard while it waits for the unrelated write, so taking it
        # first would delay the shutdown until its watchdog expired.
        self.failed = True
        self.read_held.set()  # wake a waiter of either phase step; it re-checks failed
        self.humidity_written.set()
        with self.progress:
            self.progress.notify_all()


def run_threaded(stage, paths, query, sensor_queries, temperature) -> bool:
    """Concurrent mode (--threads): producer and consumer run on the SAME
    stage from two threads -- the ovstage API is thread-safe on a shared
    instance, and the ctypes bindings release the GIL during calls. Both roles
    touch the "temperature" column, so the example coordinates the accesses
    that conflict instead of discovering them through rejected submissions: a
    write that overlaps a borrowed read group is rejected, and a payload that
    a newer write replaced cannot be read back at an older ordinal.

    * The producer holds the guard across a tick's temperature writes (or the
      delete) and the floor advance that seals them, then publishes the tick.
    * The consumer waits for a tick newer than its cursor and holds the guard
      from polling the floor through reading the delta, copying every value
      and tombstone, and releasing every group and the read handle; it reports
      the copies outside the guard. A lagging consumer simply reads a larger
      interval. The normal loop needs no per-tick acknowledgment: the producer
      may publish several ticks before a catch-up and keeps writing while the
      consumer processes its copies; conflicting accesses wait for the guard.
    * The overlap phase shows what does NOT conflict: while the consumer holds
      the temperature read group of OVERLAP_TICK, the producer writes S0's
      pre-created humidity outside the guard, and that write completes. It is
      an ordinary value update to a pre-existing, disjoint attribute; adding an
      attribute or deleting a prim would be a broader structural change.

    A failure on either side wakes the other and the run exits 1. WATCHDOG_S is
    a diagnostic timeout for the coordination waits, not a process-wide
    execution deadline. Batching varies run to run, so threaded output is not
    part of the expected-output block."""
    co = Coordination()
    # Pre-create the overlap phase's unrelated attribute before the threads
    # start: adding a column is a structural change that conflicts with every
    # borrowed read of the prim, while updating an existing value does not.
    humidity = paths.intern_token("humidity")
    stage.write_attribute(sensor_queries[HUMIDITY_SENSOR], humidity, ordinal=1,
                          tensors=np.array([0.0], np.float32), is_array=False).wait()

    def produce():
        try:
            for tick in range(1, TICKS + 1):
                if tick == OVERLAP_TICK + 1:
                    write_unrelated_while_read_held(stage, sensor_queries, humidity, tick, co)
                with co.progress:  # the tick's writes and its floor advance are one protected interval
                    if co.failed:
                        return
                    run_producer_tick(stage, sensor_queries, temperature, tick)
                    co.published = tick
                    co.progress.notify_all()
                time.sleep(0.002)  # the producer's own rate; the consumer may lag several ticks
        except OvstageError as err:
            co.fail()
            print(f"producer failed (code {int(err.code)}): {err.message}")
        except Exception as err:
            co.fail()
            print(f"producer failed: {err!r}")
        finally:
            with co.progress:
                co.done = True
                co.progress.notify_all()

    latest = {}  # sensor name -> the latest value the consumer copied
    deleted = set()  # sensor names the consumer saw as tombstones

    def consume():
        try:
            last_seen = 0
            while True:
                with co.progress:
                    if not co.progress.wait_for(
                            lambda: co.published > last_seen or co.done or co.failed, timeout=WATCHDOG_S):
                        raise TimeoutError(f"no producer progress for {WATCHDOG_S:g} s")
                    if co.failed:
                        return
                    if co.published <= last_seen:
                        break  # the producer finished and the final interval is drained
                    # Under the guard: the floor cannot move between the poll
                    # and the read, and no write can touch the borrowed groups.
                    floor = poll_write_floor(stage)
                    print(f"consumer: floor {floor}, last_seen {last_seen} -> reading delta [{last_seen + 1}, {floor}]")
                    if floor == OVERLAP_TICK:
                        groups = read_delta_holding_groups(stage, paths, query, temperature, last_seen + 1, floor, co)
                    else:
                        groups = read_delta_groups(stage, paths, query, temperature, last_seen + 1, floor)
                    last_seen = floor
                report_delta(groups, floor)  # outside the guard: the producer may write meanwhile
                for _ordinal, is_delete, group_paths, values in groups:
                    for index, path in enumerate(group_paths):
                        name = path.rsplit("/", 1)[-1]
                        if is_delete:
                            deleted.add(name)
                            latest.pop(name, None)
                        else:
                            latest[name] = values[index]
        except OvstageError as err:
            co.fail()
            print(f"consumer failed (code {int(err.code)}): {err.message}")
        except Exception as err:
            co.fail()
            print(f"consumer failed: {err!r}")

    producer = threading.Thread(target=produce, name="producer")
    consumer = threading.Thread(target=consume, name="consumer")
    producer.start()
    consumer.start()
    producer.join()
    consumer.join()
    if co.failed:
        return False
    return check_threaded_results(co, latest, deleted)
# [/snippet:threaded-producer-consumer]


# ---- plumbing ----

# [snippet:poll-write-floor]
def poll_write_floor(stage) -> int:
    """Fetch the GLOBAL write floor (attribute=None), which tells the consumer
    which ordinals are sealed for change-range queries. Payload storage is
    latest-only, so a later change to the same selected (attribute, path) can make
    the fixed-range read raise OUT_OF_RANGE."""
    with stage.get_attribute_write_floor(None) as floor_query:
        return floor_query.fetch()
# [/snippet:poll-write-floor]


def write_unrelated_while_read_held(stage, sensor_queries, humidity, tick, co):
    """Producer side of the overlap phase. Wait until the consumer has
    borrowed the temperature read group of OVERLAP_TICK, then update S0's
    humidity at ``tick`` (OVERLAP_TICK + 1) OUTSIDE the guard and wait for the
    write to complete. humidity is a different column that already exists on
    S0, so the write does not overlap the borrowed read: the runtime admits
    and completes it while that read is held."""
    if not co.read_held.wait(timeout=WATCHDOG_S):
        raise TimeoutError("the consumer did not borrow the overlap-phase read")
    if co.failed:
        return
    stage.write_attribute(sensor_queries[HUMIDITY_SENSOR], humidity, ordinal=tick,
                          tensors=np.array([55.0], np.float32), is_array=False).wait()
    co.events.append("humidity_written")
    co.humidity_written.set()


def read_delta_holding_groups(stage, paths, query, temperature, begin, end, co):
    """Consumer side of the overlap phase: read_delta_groups, except that the
    fetched groups stay borrowed while the producer completes the unrelated
    humidity write. The borrowed temperature payload must be unchanged by it;
    only then are the groups released."""
    with stage.read_attributes(query, [temperature], OrdinalRange.between(begin, end)) as read:
        read.wait()
        held = list(read.groups())
        try:
            before = [copy_group(paths, group) for group in held]
            co.events.append("read_held")
            co.read_held.set()
            if not co.humidity_written.wait(timeout=WATCHDOG_S):
                raise TimeoutError("the unrelated humidity write did not complete while the read was held")
            if co.failed:
                raise RuntimeError("the producer failed during the overlap phase")
            after = [copy_group(paths, group) for group in held]
            if after != before:
                raise RuntimeError(f"borrowed temperature payload changed under an unrelated write: {before} -> {after}")
        finally:
            for group in held:
                stage.release_group(group)
        co.events.append("read_released")
    return after


def check_threaded_results(co, latest, deleted) -> bool:
    """Assert what the concurrent run must have observed: the overlap phase's
    order (read held, unrelated write completed, read released) and the final
    state (S2 deleted; S0, S1 and S3 at their tick-4 and tick-6 values)."""
    expected_events = ["read_held", "humidity_written", "read_released"]
    expected_latest = {"S0": 40.0, "S1": 61.0, "S3": 63.0}
    ok = True
    if co.events != expected_events:
        print(f"overlap phase out of order: {co.events} (expected {expected_events})", file=sys.stderr)
        ok = False
    if latest != expected_latest or deleted != {"S2"}:
        print(f"unexpected final state: {latest}, deleted {sorted(deleted)}", file=sys.stderr)
        ok = False
    if ok:
        print(f"overlap phase: humidity on S{HUMIDITY_SENSOR} written at ordinal {OVERLAP_TICK + 1} while the "
              f"temperature read of ordinal {OVERLAP_TICK} was held; final state verified")
    return ok


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OvstageError as err:
        print(f"ovstage error (code {int(err.code)}): {err.message}")
        raise SystemExit(1)
