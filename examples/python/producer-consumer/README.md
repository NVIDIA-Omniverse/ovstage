# ovstage producer/consumer (Python)

A small, headless Python program that shows the ovstage **producer/consumer**
pattern. The producer stamps every write with an **ordinal** (a version number
the application picks; one per tick here) and seals each tick by advancing the
**write floor** (which publishes everything up to that ordinal as sealed); a
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
6. The same flow runs concurrently with --threads: producer and consumer on one shared stage.

The write floor is the producer's publish signal to the consumer:

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
- **A whole-prim delete arrives as a tombstone**: a read group with
  `is_delete=True` and no tensors.
- **A fixed range rejects a later change to the same selected key.** With a
  concurrent producer, a pending overlapping write can fail with `OP_FAILED`.
  Once committed, a later change to the same selected `(attribute, path)` after
  the requested end produces `OUT_OF_RANGE` whether or not it is sealed; a
  selected in-range unsealed change produces `WRITE_FLOOR_VIOLATION`. A
  production consumer should re-poll and widen/rebase its range, or explicitly
  request current state if that is the intended recovery.

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

The `[snippet:name]` markers in `main.py` fence regions referenced by the
ovstage skills under `../../../skills/`; keep them intact when editing.

- `setup` — imports (`numpy`, `ovstage`, `threading` for the optional concurrent mode)
- `producer-tick` — write one tick's subset at one ordinal, then seal it with `advance_write_floor`
- `tombstone-delete` — whole-prim delete: `delete_attributes` with an empty attribute list
- `poll-write-floor` — fetch the global write floor: `get_attribute_write_floor(None)` → `fetch()`
- `consume-delta` — the pull-based catch-up: explicit-begin range read, value/tombstone groups, cursor update
- `threaded-producer-consumer` — both roles concurrent on one stage (`threading.Thread`)

## Notes

- `--threads` runs both roles concurrently on the same stage — the API is
  thread-safe on a shared instance, and the ctypes bindings release the GIL
  during calls. When no overlap rejection occurs, only the batching varies
  (how many sealed ticks each catch-up happens to see). Under load, a producer
  write can be rejected while a consumer read is outstanding; a read can
  likewise reject a pending overlap or a committed change to a selected
  `(attribute, path)` after the polled range end. This demo reports the
  rejection and exits nonzero rather than retrying.
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

