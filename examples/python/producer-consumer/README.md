# ovstage producer/consumer (Python)

A small, headless Python program that shows the ovstage **producer/consumer**
pattern. The producer stamps every write with an **ordinal** (a version number
the application picks; one per tick here) and seals each tick by advancing the
**write floor** (which seals data at or below that ordinal); a
pull-based consumer polls the floor and reads only the **delta**
— "what changed since ordinal N" — with `OrdinalRange.between`. It is the
Python sibling of `../../c/producer-consumer/main.cpp` (same flow,
byte-identical default-mode output).

## At a glance

1. Create a stage and four sensor prims (the first write creates them).
2. Producer runs ticks 1-3: each tick writes two sensors at a fresh ordinal and seals it.
3. Consumer catches up: it fetches the write floor and reads only what changed since its last ordinal.
4. Producer runs ticks 4-6; tick 5 deletes a sensor entirely.
5. Consumer catches up again: the delta is bigger (it lagged three ticks) and includes the delete as a tombstone.
6. The same flow runs concurrently with --threads: producer and consumer on one shared stage, coordinated by a guard, plus an overlap phase showing that an unrelated write completes while a temperature read group is held.

The write floor tells the consumer which ordinals the producer has sealed:

```text
producer                        ovstage                      consumer
  write subset @ ordinal=tick -> temperature column
  advance write floor --------> floor = tick    <----------- poll floor
                                                <----------- read delta [last_seen+1, floor]
                                                             last_seen = floor
```

## What you'll see

Expected output (default mode):

```
producer tick 1: wrote S0 = 10.0, S1 = 11.0 (floor -> 1)
producer tick 2: wrote S1 = 21.0, S2 = 22.0 (floor -> 2)
producer tick 3: wrote S2 = 32.0, S0 = 30.0 (floor -> 3)
consumer: floor 3, last_seen 0 -> reading delta [1, 3]
  value group (ordinal 3): S0 = 30.0, S1 = 21.0, S2 = 32.0
  delta batch: 3 prim changes
consumer: last_seen -> 3
producer tick 4: wrote S3 = 43.0, S0 = 40.0 (floor -> 4)
producer tick 5: deleted S2 entirely (floor -> 5)
producer tick 6: wrote S1 = 61.0, S3 = 63.0 (floor -> 6)
consumer: floor 6, last_seen 3 -> reading delta [4, 6]
  value group (ordinal 6): S0 = 40.0, S1 = 61.0, S3 = 63.0
  tombstone group (ordinal 6): S2 deleted
  delta batch: 4 prim changes
consumer: last_seen -> 6
```

- **A lagging consumer within the retained membership window does not miss a
  change.** It sat out three ticks before the second catch-up, so it just gets
  a bigger delta: 4 prim changes instead of 3.
- **Successful groups carry current state.** The first catch-up includes
  `S1 = 21.0`, written at tick 2.
- **A whole-prim delete arrives as a tombstone**: a read-group column with
  `is_delete=True` and no tensors.
- **A fixed range rejects a later change to the same selected key.** With a
  concurrent producer, a pending overlapping write can fail with `OP_FAILED`.
  Once committed, a later change to the same selected `(attribute, path)` after
  the requested end produces `OUT_OF_RANGE` whether or not it is sealed; a
  selected in-range unsealed change produces `WRITE_FLOOR_VIOLATION`. A
  production consumer should re-poll and widen/rebase its range, or explicitly
  request current state if that is the intended recovery. The `--threads` mode
  avoids both conflicts by coordinating the temperature accesses (see Notes).

## Build and run

The example is a [uv](https://docs.astral.sh/uv/) project pinning the released
`ovstage` wheel (see `pyproject.toml`); the wheel bundles the native shared
library, which the bindings load automatically. No scene file is needed — the
four sensor prims are created by the first UPSERT write (write-or-create) that
targets them:

```bash
uv run main.py             # deterministic interleaved mode
uv run main.py --threads   # concurrent mode
```

> **Pre-release:** if `uv` cannot resolve the pinned `ovstage` wheel, no package
> index available to you carries it yet — check the repository releases page for
> current availability.


## Snippets

The `[snippet:name]` markers in [main.py](main.py) identify the source regions described below and referenced by the [ovstage skills](../../../skills/). Keep the markers intact when editing.

- `setup` — Imports NumPy, ovstage, and threading support for the optional concurrent run. Both execution modes use the same stage, sensor queries, and temperature attribute.
- `producer-tick` — `producer_value_tick()` writes the selected sensors at ordinal `tick`, with each value calculated as `10 * tick + sensor`. It waits for the writes and advances the global write floor to seal that tick before printing the update.
- `tombstone-delete` — `producer_delete_tick()` calls `delete_attributes()` with an empty attribute list to delete sensor S2 entirely, then advances the write floor. A consumer sees this deletion as a column with `is_delete=True` and no value tensors.
- `poll-write-floor` — `poll_write_floor()` calls `get_attribute_write_floor(None)` and fetches the global floor. This tells the consumer how far writes have been sealed; the context manager closes the floor query afterward.
- `consume-delta` — `copy_group()` copies one fetched group into application-owned storage (paths resolved through the dictionary, current values or a tombstone); `read_delta_groups()` reads from `last_seen + 1` through the newly polled floor using `OrdinalRange.between()`, copying and releasing each group at once; `report_delta()` prints the copies; `consume_deltas()` ties them together and returns the new cursor. This is a change query, not a history of every write.
- `threaded-producer-consumer` — `Coordination` holds the guard for the temperature access scope, the condition variable that publishes sealed ticks, and the overlap phase's events; `run_threaded()` pre-creates S0's `humidity`, starts both threads, joins them and checks the overlap phase's order and the final state. The producer holds the guard across each tick's writes and floor advance and then publishes the tick; the consumer waits for newer progress, holds the guard from the floor poll through copying and releasing the groups, and reports outside it. At ordinal 3 the consumer keeps its read group borrowed (`read_delta_holding_groups()`) while the producer updates S0's humidity outside the guard (`write_unrelated_while_read_held()`), proving that an unrelated value write completes while a read is held. Catch-up batch sizes depend on timing.

## Notes

- `--threads` runs both roles concurrently on the same stage — the API is
  thread-safe on a shared instance, and the ctypes bindings release the GIL
  during calls. Both roles touch the `temperature` column, and two things
  conflict there: a write that overlaps a read group the consumer still holds
  is rejected, and a payload that a newer write replaced cannot be read back at
  an older ordinal. The example coordinates those accesses instead of
  discovering them through rejected submissions: a guard covers the producer's
  tick (its writes plus the floor advance) and the consumer's catch-up (floor
  poll, read, copy, release), and a condition variable publishes sealed ticks so
  the consumer waits for the ordinal it needs. The guard deliberately covers
  the consumer's four-sensor temperature scope; that is an application choice
  for this example, not a stage-wide locking requirement, and applications
  with independent access sets can partition their coordination. The normal
  loop requires no per-tick acknowledgment: the producer may publish several
  ticks before a catch-up and can continue writing while the consumer processes
  its copied results. Conflicting accesses wait for the temperature guard; the
  one-time overlap phase at ordinal 3 adds an explicit handshake to demonstrate
  disjoint access: the consumer keeps a temperature read group borrowed while
  the producer updates S0's pre-created `humidity`, and the run asserts that
  the write completed and the borrowed payload stayed intact. That is an
  ordinary value update to a pre-existing, disjoint attribute/path pair;
  creating an attribute or deleting/replacing a prim can introduce broader
  structural conflicts, which is why `humidity` is created before the threads
  start. Only the batching varies run to run. A rejection or an unexpected
  final state is a defect and exits nonzero. The coordination waits have a
  30-second diagnostic timeout (`WATCHDOG_S`); this is not a process-wide
  execution deadline.
- This six-tick example omits retention-boundary recovery: a consumer whose
  next interval starts before `get_oldest_preserved_ordinal()` has fallen
  behind the retained change records and must rebuild its state from a
  current-state read, under appropriate producer coordination, before resuming
  its cursor; widening the delta interval cannot recover discarded change
  records or tombstones. See [When to Use a Change-Range
  Read](../../../docs/scene/reading_attributes.rst#when-to-use-a-change-range-read).
- The explicit begin of the range read is what makes it a delta; an open begin
  would be a snapshot read.
- Prim positions come from the group itself: `group.prim_index(local)` applies
  the group's offset / `index_map`, and the paths resolve through the path
  dictionary.
- The bindings load the native shared library via ctypes; keep the package
  tree intact — ovstage finds its bundled plugins relative to where the
  library loads from.

- This example's producer seals with a single floor advance since it owns
  every column it writes. With several producers, each seals only its own
  columns (`Scope.INCLUDE` + an attribute list) and the **global** floor — the
  minimum across all attributes — is what a conservative consumer watches.

