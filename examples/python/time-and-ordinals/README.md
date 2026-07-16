# ovstage time and ordinals (Python)

A standalone Python program that shows **how to place time-indexed data on
the ordinal axis**: one timeline, two data sources. A saxpy simulator steps
three sphere prims with **non-uniform dt** (`position += velocity * dt`), and
a time-sampled USD clip animates a conveyor. ovstage stores no time — it
orders writes by opaque `uint64` ordinals — so the application owns an
explicit ordinal ↔ time table and uses it to land **both** sources in the
same ordinal slot per tick. Because the slots are shared, anything a consumer
reads at a sealed ordinal is **time-coherent** — every value describes the
same instant. It is the Python sibling of
`../../c/time-and-ordinals/main.cpp` (same sections, byte-identical output).

## At a glance

1. Populate an inline USD clip (the conveyor's translate.x is time-sampled) and write the spheres' t = 0 state, both at ordinal 1.
2. Build the app-owned ordinal ↔ time table — dt varies per tick, so it is a table, not a formula.
3. Per tick: step the saxpy sim, write it at the tick's ordinal, re-sample the clip at the same simulation time, and seal both columns.
4. Rewind the clip: one more tick where simulation time climbs to 0.875 s but the clip is pointed back to USD time 0.125 s — the clip's clock is playback policy, free to diverge from the timeline.

Because dt is non-uniform, time is not derivable from the ordinal — the table
is the only bridge (timecode = t × 16, the clip's timeCodesPerSecond):

```text
tick      1      2      3      4      5      6
dt (s)    0.125  0.125  0.0625 0.0625 0.25   0.125
t (s)     0.125  0.25   0.3125 0.375  0.625  0.75
timecode  2      4      5      6      10     12
ordinal   2      3      4      5      6      7      (ordinal 1 = the t 0 setup)
```

## What you'll see

```text
time model: non-uniform dt; the app owns an ordinal <-> time table (ordinal = tick + 1)
clip: conveyor translate.x animates 0 -> 120 over 12 timecodes @ 16 codes/s (translate.x = 160*t)
setup: clip populated + 3 spheres written at t = 0.0000 s (ordinal 1); write floor -> 1
tick 1 (dt = 0.1250 s) -> t = 0.1250 s = timecode  2 -> ordinal 2: sim + clip written, sealed
tick 2 (dt = 0.1250 s) -> t = 0.2500 s = timecode  4 -> ordinal 3: sim + clip written, sealed
tick 3 (dt = 0.0625 s) -> t = 0.3125 s = timecode  5 -> ordinal 4: sim + clip written, sealed
tick 4 (dt = 0.0625 s) -> t = 0.3750 s = timecode  6 -> ordinal 5: sim + clip written, sealed
tick 5 (dt = 0.2500 s) -> t = 0.6250 s = timecode 10 -> ordinal 6: sim + clip written, sealed
tick 6 (dt = 0.1250 s) -> t = 0.7500 s = timecode 12 -> ordinal 7: sim + clip written, sealed
rewind: sim t = 0.8750 s (ordinal 8) but the clip is pointed at usd t = 0.1250 s -- playback policy, not the timeline
```

- Each tick line is the mapping at work: the app derives t from its dt
  schedule, looks up (or extends) the table, and lands the sim write and the
  clip sample in the same ordinal slot — `population.update_from_usd_time` takes seconds and
  the tick's ordinal, so USD's time-sampled data is placed on the same axis
  as the direct writes.
- The rewind line shows the flip side of the alignment: `population.update_from_usd_time` can
  point the clip's clock anywhere (rewind, loop, hold) while simulation time
  and ordinals only ever climb — USD sampling time is the application's
  playback policy, not a property of the timeline.
- The example only writes. Consumers read snapshots by ordinal, and because
  both sources share slots, any read at a sealed ordinal is time-coherent;
  verifying read-back values is the public test suite's job
  (`../../../tests/`), not the example's.
- Every dt is an exact binary fraction, so the printed times and timecodes
  are exact and the output is deterministic.

## Build and run

The example depends on a published `ovstage` wheel (pinned in
`pyproject.toml`); [uv](https://docs.astral.sh/uv/) resolves, installs, and
runs it in one step. Requirements: Python 3.10–3.13 and `uv`.

```bash
uv run main.py
```

> **Pre-release:** if `uv` cannot resolve the pinned `ovstage` wheel, no package
> index available to you carries it yet — check the repository releases page for
> current availability.

The wheel bundles the native library under `<package>/bin`, which the bindings
search automatically, so no loader-path setup is needed. To run against a
locally built `libovstage` instead, put the build output dir (the one holding
`ovstage.dll` / `libovstage.so`) on the loader path — `PATH` on Windows,
`LD_LIBRARY_PATH` on Linux — and use a plain interpreter:

```bash
pip install numpy
# Linux
export LD_LIBRARY_PATH=/path/to/ovstage/build/output:$LD_LIBRARY_PATH
# Windows (PowerShell)
#   $env:PATH = "C:\path\to\ovstage\build\output;$env:PATH"
python main.py
```


## Snippets

The `[snippet:name]` markers in `main.py` fence regions the ovstage skills
under `../../../skills/` can reference; keep them intact when editing.

- `time-to-ordinal-table` — the app-owned ordinal ↔ time table (a table, not a formula)
- `float3-attribute-write` — float3-per-prim column write (`lanes=3`, POINT/VECTOR semantic)

## Notes

- ovstage stores no time. Per the API header: applications that need physical
  time "should carry an explicit ordinal-to-time mapping outside this scalar"
  — the table in this example is exactly that.
- Population (`update_from_usd_time`) and direct writes share the one ordinal
  axis; putting the clip sample for time t and the sim write for time t at
  the same ordinal is what makes the slot a coherent snapshot.
- The sphere writes use UPSERT prim mode, so the first write at ordinal 1
  creates the prims; the conveyor comes from the populated clip.
- Client-managed tensors must stay valid until their op completes — each
  tick's write is waited before the position buffer is reused — and destroying
  the instance requires every op and handle released first, hence the releases
  at the end of `main`.
- The example fails fast: any unexpected API failure raises `OvstageError` and
  the entry point exits nonzero. A real application would handle errors
  instead.
- Everything runs on the CPU; no GPU is needed.
- The population API's `time_code` arguments are **seconds**, converted to
  timecodes via the layer's `timeCodesPerSecond` (16 here), so 0.375 s samples
  timecode 6.
- `update_from_usd_time` re-evaluates time-sampled values in the domains
  chosen at populate time. For RENDERING it currently also picks up pending
  structural USD edits, which the other domains leave to `apply_usd_changes`;
  this example authors no USD edits after populate. The behaviors are
  expected to converge.

