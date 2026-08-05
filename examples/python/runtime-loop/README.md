# ovstage runtime loop (Python)

A small, headless Python program that shows the ovstage **runtime loop**: load
a USD scene, populate it (copy it) into the ovstage runtime table, read it
back, update it two ways, and read again — with **no renderer attached**. It is
the Python sibling of `../../c/runtime-loop/main.cpp`.

## At a glance

1. Load the torus-and-plane USD scene and populate it into the ovstage runtime
   table at ordinal 1 — an ordinal is a version number the application picks.
2. Seal that ordinal by advancing the write floor, then read the reserved
   `usd-prim-type` metadata back to confirm the scene landed.
3. Animate the Torus by writing its transform straight into the runtime table,
   one ordinal per frame for 24 frames — the fast path, no USD round-trip.
4. Read the final transform back: the Torus slid 100 units along +X.
5. Add a Cube the other way, through the USD source: reference an inline USDA
   layer, then apply the USD changes so the new prim propagates into the
   runtime table.
6. Read the prim types one last time — the Cube is now visible.

The application owns the ordinal lifecycle throughout, sealing each ordinal
with `advance_write_floor` before reading:

```text
        populate   animation frames   USD edit
ordinal 1          2 … 25             26
```

## What you'll see

```
populated prim types: Xform, Mesh, Mesh
final Torus xform translation (row [3][0:3]): [100.  25.   0.]
after USD edit, prim types: Xform, Mesh, Mesh, Cube
```

- Line 1 confirms the populate: `/World` (an Xform) plus the Plane and Torus
  meshes from `torus-plane.usda`.
- Line 2 is update path 1 — directly in the runtime table. The scene is Y-up
  with the Torus authored at `translate = (0, 25, 0)`, so it slid 100 units
  along +X, keeping y=25.
- Line 3 is update path 2 — through the USD source. `/World/EditCube` existed
  only in a USD reference edit; `apply_usd_changes` propagates it into the
  runtime table as a fourth prim.

## Build and run

The example depends on a published `ovstage` wheel (pinned in
`pyproject.toml`); [uv](https://docs.astral.sh/uv/) resolves, installs, and
runs it in one step. The wheel bundles the `ovstage` shared library, which
the bindings load automatically.

```bash
uv run main.py
```

> **Pre-release:** if `uv` cannot resolve the pinned `ovstage` wheel, no package
> index available to you carries it yet — check the repository releases page for
> current availability.


## Snippets

The `[snippet:name]` markers in `main.py` fence regions referenced by the
ovstage skills under `../../../skills/`; keep them intact when editing.

- `setup` — imports (`numpy`, `ovstage`, `population`)
- `populate` — `open_usd` + `advance_write_floor` (load file → runtime table)
- `read-populated` — read the reserved `usd-prim-type` metadata to confirm the populate
- `update-table` — animate `omni:xform` over 24 frames straight into the table (path 1)
- `update-usd` — `add_usd_reference_from_string` + `apply_usd_changes` (path 2)

## Notes

- `open_usd(...)` takes any `.usda`/`.usdc` path; `torus-plane.usda` is the
  ovrtx minimal example scene, copied next to `main.py` so the example stays
  self-contained.
- The example supplies `omni:xform` in its canonical form: **one 16-lane
  element per prim** (`dtype.lanes = 16`, `shape = [1]`, via
  `make_dltensor`). A convenience input shaped `(N, 4, 4)` with `lanes = 1`
  is also accepted, but its trailing shape is not preserved: raw reads/maps
  return `(N,)` with 16 lanes and Python DLPack export produces `(N, 16)`.
- The inline `add_usd_reference` USDA must be multi-line; a single-line
  layer-metadata + prim body does not parse through the anonymous-layer path.

