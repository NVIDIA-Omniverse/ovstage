# ovstage producer/consumer (C/C++)

A standalone C++ program that shows the ovstage **producer/consumer** pattern
through the core **C API**: the producer stamps every write with an **ordinal**
(a version number the application picks; one per tick here) and seals each tick
by advancing the **write floor**, and a pull-based consumer polls the floor and
reads only the **delta** — "what changed since ordinal N". It is the C sibling
of `../../python/producer-consumer/main.py` (byte-identical output).

## At a glance

1. Create a stage and four sensor prims (the first write creates them).
2. Producer runs ticks 1-3: each tick writes two sensors at a fresh ordinal and seals it.
3. Consumer catches up: it fetches the write floor and reads only what changed since its last ordinal.
4. Producer runs ticks 4-6; tick 5 deletes a sensor entirely.
5. Consumer catches up again: the delta is bigger (it lagged three ticks) and includes the delete as a tombstone.
6. The same flow runs concurrently with --threads: producer and consumer on one shared stage, coordinated by a guard, plus an overlap phase showing that an unrelated write completes while a temperature read group is held.

```text
producer                          ovstage                       consumer
write subset @ ordinal N  ---->   pending (unsealed)
advance write floor -> N  ---->   ordinals <= N sealed
                                  global floor  <-------------  poll
                                  changed prims, latest  ---->  read delta [last_seen+1, floor]
                                  values + tombstones           last_seen = floor
```

## What you'll see

Expected output (default mode):

```text
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

- A lagging consumer misses nothing: it sat out three ticks before the second
  catch-up, so it just gets a bigger delta (4 prim changes instead of 3).
- Successful groups carry current state. The first catch-up includes
  `S1 = 21.0`, written at tick 2.
- A whole-prim delete arrives as a tombstone: a read-group column with
  `is_delete = true` and no tensors.
- With a concurrent producer, a pending overlapping write can fail the read
  with `OP_FAILED`. Once committed, a later change to the same selected
  `(attribute, path)` after the requested end produces `OUT_OF_RANGE` whether
  or not it is sealed; a selected in-range unsealed change produces
  `WRITE_FLOOR_VIOLATION`. A production consumer should re-poll and widen/rebase
  its range, or explicitly request current state if that is the intended
  recovery. The `--threads` mode avoids both conflicts by coordinating the
  temperature accesses (see Notes).

## Build and run

The example builds standalone with CMake: `find_package(ovstage)` locates an
installed package, otherwise the build fetches the released package zip (see
`../cmake/ovstage.cmake`). The shared check/wait helpers live in
`../common/ovstage_example_utils.h` — like `../cmake/`, copy that directory
along if you relocate this example.

```bash
# Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/producer-consumer            # deterministic interleaved mode
./build/producer-consumer --threads  # concurrent mode (output varies)
```

```powershell
# Windows
cmake -B build
cmake --build build --config Release
# ovstage discovers its bundled plugins relative to where ovstage.dll loads
# from, so keep the package bin/ intact and put it on PATH (do not copy the
# DLL next to the exe):
$env:PATH = "<ovstage-package>\bin;$env:PATH"
.\build\Release\producer-consumer.exe
```

On Linux the build sets an rpath onto the package `bin/`, so the binary runs
from anywhere with no environment setup (no assets needed — the first UPSERT
write creates the sensor prims). To build every C example at once, configure
from the parent directory (`../CMakeLists.txt` aggregates them).


## Snippets

The `[snippet:name]` markers in [main.cpp](main.cpp) identify the source regions described below and referenced by the [ovstage skills](../../../skills/). Keep the markers intact when editing.

- `producer-tick` — `producerValueTick()` writes the selected sensors at ordinal `tick`, calculating each temperature as `10 * tick + sensor`. It waits for the writes and advances the global write floor before printing the update; its boolean result reports success or failure.
- `tombstone-delete` — `producerDeleteTick()` calls `ovstage_delete_attributes()` with no attributes to delete sensor S2 entirely, then advances the write floor. A consumer sees the deletion as a column with `is_delete` set and no value tensors.
- `poll-write-floor` — `fetchGlobalFloor()` requests the global floor with `ovstage_get_attribute_write_floor()`, fetches it with `ovstage_fetch_ordinal()`, and releases the ordinal query after the fetch. It writes the sealed boundary to `outFloor` and returns whether the calls succeeded.
- `consume-delta` — `consumerCatchUp()` polls the floor and calls `consumeDelta()` for the interval from `lastSeen + 1` through that floor. `sensorName()` resolves group prims to display names, `copyGroup()` copies one fetched group (names plus current values, or a tombstone) into application-owned storage so each group is released at once, `reportDelta()` prints the copies, and the consumer advances its cursor after success. The result describes changed state, not every individual write.
- `threaded-producer-consumer` — `Coordination` holds the guard for the temperature access scope, the condition variable that publishes sealed ticks, and the overlap phase's handshake and events; `runThreaded()` pre-creates S0's `humidity`, starts a producer thread, consumes on the calling thread, joins the producer and checks the overlap phase's order and the final state. The producer holds the guard across each tick's writes and floor advance and then publishes the tick; the consumer waits for newer progress, holds the guard from the floor poll through copying and releasing the groups, and reports outside it. At ordinal 3 the consumer keeps its read group borrowed (`consumeDeltaHoldingRead()`) while the producer updates S0's humidity outside the guard (`writeUnrelatedWhileReadHeld()`), proving that an unrelated value write completes while a read is held. Catch-up batch sizes depend on timing.

## Notes

- The explicit begin of the range read is what makes it a delta; an open begin
  would be a snapshot read.
- `--threads` runs both roles concurrently on the same instance — every API
  slot is thread-safe on a shared instance (see *Thread Safety* in
  `ovstage_api.h`). Both roles touch the `temperature` column, and two things
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
  30-second diagnostic timeout (`kWatchdog`); this is not a process-wide
  execution deadline.
- This six-tick example omits retention-boundary recovery: a consumer whose
  next interval starts before `ovstage_get_oldest_preserved_ordinal()` has fallen
  behind the retained change records and must rebuild its state from a
  current-state read, under appropriate producer coordination, before resuming
  its cursor; widening the delta interval cannot recover discarded change
  records or tombstones. See [When to Use a Change-Range
  Read](../../../docs/scene/reading_attributes.rst#when-to-use-a-change-range-read).
- The examples fail fast: any unexpected API failure prints and exits (helpers
  in `../common/ovstage_example_utils.h`). A real application would propagate
  errors instead.
- Everything runs on the CPU; no GPU is needed.

- This example's producer seals with a single floor advance since it owns
  every column it writes. With several producers, each seals only its own
  columns (`SCOPE_INCLUDE` + an attribute list) and the **global** floor — the
  minimum across all attributes — is what a conservative consumer watches.

