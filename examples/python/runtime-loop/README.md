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

- `populated prim types` confirms the populate: `/World` (an Xform) plus the Plane and Torus meshes from `torus-plane.usda`.
- `final Torus xform translation` shows the direct runtime update. The scene is Y-up with the Torus authored at `translate = (0, 25, 0)`, so it slid 100 units along +X, keeping y=25.
- `after USD edit, prim types` shows the update through the USD source. `/World/EditCube` existed only in a USD reference edit; `apply_usd_changes` propagates it into the runtime table as a fourth prim.

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

The `[snippet:name]` markers in [main.py](main.py) identify the source regions described below and referenced by the [ovstage skills](../../../skills/). Keep the markers intact when editing.

- `setup` — Imports NumPy, ovstage, and the population and matrix types used by the loop. `main()` checks `population.available()` before opening the scene.
- `populate` — Calls `population.open_usd()` to load the scene's rendering data at time 0 and ordinal 1. Waiting for `advance_write_floor(ordinal=1)` makes that populated state available to readers.
- `read-populated` — Calls `_prim_types()` to read `usd-prim-type` for the selected scene paths and resolve its token identifiers to names. The printed types confirm that population supplied the expected prims.
- `update-table` — Uses `_translate_matrix()` and `write_attribute()` to move the Torus over 24 frames, assigning and sealing a new ordinal for each frame. It then reads the final `omni:xform` and prints the translation `[100, 25, 0]` before releasing the read group.
- `update-usd` — Calls `add_usd_reference_from_string()` to add `/World/EditCube` to the USD source, then `apply_usd_changes()` to update ovstage at a fresh ordinal. After advancing the write floor, it queries the expanded path list and prints the new prim's type alongside the original types.

## Notes

- `open_usd(...)` takes any `.usda`/`.usdc` path; `torus-plane.usda` is the
  ovrtx minimal example scene, copied next to `main.py` so the example stays
  self-contained.
- The example supplies `omni:xform` in its canonical form: **one 16-lane
  element per prim** (`dtype.lanes = 16`, `shape = [1]`, via
  `make_dltensor`). A convenience input shaped `(N, 4, 4)` with `lanes = 1`
  is also accepted. Its exact input shape is not stored: raw reads reconstruct
  `(N, 4, 4)` with scalar lanes for both reads and maps.
- The inline `add_usd_reference` USDA must be multi-line; a single-line
  layer-metadata + prim body does not parse through the anonymous-layer path.

