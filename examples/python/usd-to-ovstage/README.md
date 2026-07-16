# USD to ovstage migration (Python)

A small, headless Python program that runs the **same workflow twice in one
process** — create typed prims, author attribute values, batch a group of
edits, read everything back — first with the plain USD API via `pxr`
([`usd-core`](https://pypi.org/project/usd-core/) from PyPI), on a stage that
is **never bound to ovstage**, then with the ovstage equivalents, authored
client-side with zero USD. The op-count lines carry the migration lesson: the
USD habit is one call per prim, ovstage is one **vectorized** write per column
(and one **batched** op for several columns). Python-only — the released
ovstage package ships no USD headers to build a C USD half against.

## At a glance

1. **USD half:** create an in-memory `pxr` stage and define five typed prims
   (an Xform parent plus four typed children) — one `DefinePrim` call per prim.
2. Author a `temperature` attribute the USD way — one
   `CreateAttribute(...).Set(...)` per prim — and read each value back with
   `attr.Get()`.
3. Batch a group of edits the USD way: 8 `Set` calls (humidity + pressure on
   four prims) under one `Sdf.ChangeBlock` — only the change *notification* is
   coalesced; the edits still happen one call at a time.
4. **ovstage half:** create the same five prims with ONE INSERT-mode write of
   the reserved `usd-prim-type` column — the write itself creates the prims
   and stamps their types — and read the stamped types back.
5. Write the same temperatures the naive way (a loop of 4 one-prim write ops),
   then the idiomatic way (1 vectorized write op covering all 4 prims).
6. Land humidity + pressure as ONE batched `write_attributes` op (two columns,
   one op), then close with the pointer: automatically mirroring an existing
   USD stage is population, not migration.

## What you'll see

```
== Part 1: plain USD via pxr (usd-core), never bound to ovstage ==
create: 5 DefinePrim calls (one per prim)
created prims: /World (Xform), /World/S0 (Cube), /World/S1 (Sphere), /World/S2 (Cone), /World/S3 (Cylinder)
one-by-one: 4 CreateAttribute+Set calls (one per prim)
temperature: S0 = 20.5, S1 = 21.5, S2 = 22.5, S3 = 23.5
changeblock: 8 Set calls under one Sdf.ChangeBlock (coalesced notification, still per-prim edits)
humidity: S0 = 40.0, S1 = 41.0, S2 = 42.0, S3 = 43.0
pressure: S0 = 101.3, S1 = 101.4, S2 = 101.5, S3 = 101.6

== Part 2: the ovstage equivalents, client-authored ==
create: 1 INSERT write op (the write itself creates all 5 prims and stamps their types)
created prims: /World (Xform), /World/S0 (Cube), /World/S1 (Sphere), /World/S2 (Cone), /World/S3 (Cylinder)
one-by-one: 4 write ops (one per prim)
temperature: S0 = 20.5, S1 = 21.5, S2 = 22.5, S3 = 23.5
vectorized: 1 write op (one column covering all 4 prims)
temperature: S0 = 20.5, S1 = 21.5, S2 = 22.5, S3 = 23.5
batched: 1 write op (2 columns: humidity + pressure)
humidity: S0 = 40.0, S1 = 41.0, S2 = 42.0, S3 = 43.0
pressure: S0 = 101.3, S1 = 101.4, S2 = 101.5, S3 = 101.6

mirroring an existing USD stage into ovstage automatically is population -- see the runtime-loop example
```

- The two halves print the same value lines (`created prims:`, `temperature:`,
  `humidity:`, `pressure:`); the `create:` / `one-by-one:` / `changeblock:` /
  `vectorized:` / `batched:` lines count how the values got there.
- Creation: 5 `DefinePrim` calls vs 1 INSERT-mode (create-only) write op —
  ovstage has no create-prim call; the `usd-prim-type` write creates all five
  prims and stamps their types in one op.
- Group edits: `Sdf.ChangeBlock` coalesces only the change notification around
  8 per-prim `Set` calls; `write_attributes` lands both columns in genuinely
  one op — a grouping, not an atomic transaction.
- Reads migrate the same way: one `attr.Get()` per prim becomes one
  `read_attributes` op per column, after `advance_write_floor` seals the write
  ordinal so readers can trust it.

## Build and run

The example is a [uv](https://docs.astral.sh/uv/) project pinning the released
`ovstage` wheel plus `usd-core` (see `pyproject.toml`). The wheel bundles the
native library at `<package>/bin`, which the bindings load automatically, and
`usd-core` provides the `pxr` modules for Part 1. No scene file is needed.

```bash
uv run main.py
```

> **Pre-release:** if `uv` cannot resolve the pinned `ovstage` wheel, no package
> index available to you carries it yet — check the repository releases page for
> current availability. (`usd-core` resolves from public PyPI.)


## Snippets

The `[snippet:name]` markers in `main.py` fence regions referenced by the
ovstage skills under `../../../skills/`; keep them intact when editing.

- `usd-create-prims` — the in-memory `pxr` stage and one `DefinePrim` per prim
- `usd-author-one-by-one` — the USD habit: `CreateAttribute(...).Set(...)` per prim, `attr.Get()` per prim
- `usd-changeblock-batch` — 8 `Set` calls under one `Sdf.ChangeBlock` (notification-only batching)
- `ovstage-create-prims` — ONE INSERT write of `usd-prim-type` creates the prims and stamps types
- `ovstage-write-one-by-one` — the naive port: a loop of one-prim write ops (works, unbatched)
- `ovstage-write-vectorized` — the idiomatic port: one write op, one column over all prims
- `ovstage-write-batched` — `write_attributes`: two columns in one batched op
- `ovstage-read-column` — one read op returns the whole column, rows placed via `prim_index`

## Notes

- Two USD runtimes coexist in this one process: `usd-core` provides `pxr` for
  Part 1, and ovstage internally uses its own bundled USD runtime. They never
  share objects — no `pxr` handle crosses the boundary.
- USD `timeSamples` and ovstage ordinals are different concepts: a timeSample
  is scene content (a value on a scene-time axis); an ordinal is an
  application-owned version number on the write axis. This example authors USD
  *defaults* only, and spends one ordinal per step (1..4).
- The example fails fast: an unexpected `OvstageError`/`OvxError` propagates
  to the top-level handler, which prints one line and exits nonzero.

