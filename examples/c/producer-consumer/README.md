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
6. The same flow runs concurrently with --threads: producer and consumer on one shared stage.

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
- A whole-prim delete arrives as a tombstone: a read group with
  `is_delete = true` and no tensors.
- With a concurrent producer, a pending overlapping write can fail the read
  with `OP_FAILED`. Once committed, a later change to the same selected
  `(attribute, path)` after the requested end produces `OUT_OF_RANGE` whether
  or not it is sealed; a selected in-range unsealed change produces
  `WRITE_FLOOR_VIOLATION`. A production consumer should re-poll and widen/rebase
  its range, or explicitly request current state if that is the intended
  recovery.

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

The `[snippet:name]` markers in `main.cpp` fence regions referenced by the
ovstage skills under `../../../skills/`; keep them intact when editing.

- `producer-tick` — write one tick's subset at one ordinal, then seal it with `advance_write_floor`
- `tombstone-delete` — whole-prim delete: `delete_attributes` with an empty attribute list
- `poll-write-floor` — fetch the global write floor: `get_attribute_write_floor` → `fetch_ordinal` → `release_ordinal_query`
- `consume-delta` — the pull-based catch-up: explicit-begin range read, value/tombstone groups, cursor update
- `threaded-producer-consumer` — both roles concurrent on one instance (`std::thread`)

## Notes

- The explicit begin of the range read is what makes it a delta; an open begin
  would be a snapshot read.
- `--threads` runs both roles concurrently on the same instance — every API
  slot is thread-safe on a shared instance (see *Thread Safety* in
  `ovstage_api.h`). When no overlap rejection occurs, only the batching varies
  run to run (how many sealed ticks each catch-up happens to see). Under load,
  a producer write can be rejected while a consumer read is outstanding; a
  read can likewise reject a pending overlap or a committed change to a
  selected `(attribute, path)` after the polled range end. This demo reports
  the rejection, shuts both roles down, and exits nonzero rather than retrying.
- The examples fail fast: any unexpected API failure prints and exits (helpers
  in `../common/ovstage_example_utils.h`). A real application would propagate
  errors instead.
- Everything runs on the CPU; no GPU is needed.

- This example's producer seals with a single floor advance since it owns
  every column it writes. With several producers, each seals only its own
  columns (`SCOPE_INCLUDE` + an attribute list) and the **global** floor — the
  minimum across all attributes — is what a conservative consumer watches.

