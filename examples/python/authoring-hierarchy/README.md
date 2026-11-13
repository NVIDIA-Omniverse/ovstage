# ovstage authoring hierarchy (Python)

A standalone Python program that shows pure client-side authoring and the
hierarchy APIs: it builds a multi-environment world with **zero USD** — prims
come into existence via attribute writes to nested paths. It then shows that
derived world transforms are **pull-computed**: the rows start absent, the
client seeds them, and a hierarchy computation model fills or refreshes them.
It is the Python sibling of
`../../c/authoring-hierarchy/main.cpp` (same sections, byte-identical output).

## At a glance

1. Create prims by writing attributes to nested paths — no USD anywhere; the hierarchy comes from the paths.
2. Stamp prim types (`usd-prim-type`) and applied schemas (`usd-schemas`) client-side.
3. Write each prim's local transform (`omni:xform`) and clone the prototype into three environments.
4. Seed the derived world-matrix rows, run the GPU compute, seal the ordinal, and read correct world translations.
5. Move one environment and recompute to fold the edit into the derived rows.
6. Look up parent, children, and siblings of a prim.

Every local transform is a pure translation, so each Tip's world translation
is the sum of the locals down its chain:

```text
/World (0,100,0)
+- Proto (10,0,0)   --clone-->  Env_0 (100,0,0)   Env_1 (200,0,0)   Env_2 (300,0,0)
   +- Arm  (0,0,3)              (each environment repeats Arm, Body, Body/Tip)
   +- Body (0,0,5)
      +- Tip (0,0,2)     e.g. Env_0 Tip world = (0,100,0)+(100,0,0)+(0,0,5)+(0,0,2) = (100,100,7)
```

## What you'll see

```
prototype prim types: Xform Xform Cube Xform Cube
applied schemas on /World/Proto/Body: PhysicsMassAPI PhysicsRigidBodyAPI
cloned /World/Proto -> /World/Env_0 /World/Env_1 /World/Env_2
hierarchy computation models: cpu-incremental gpu-incremental gpu-global runtime-default
world matrix rows before compute_hierarchy: absent (derived rows are pull-computed)
world translation /World/Proto/Body/Tip: 10.0 100.0 7.0
world translation /World/Env_0/Body/Tip: 100.0 100.0 7.0
world translation /World/Env_1/Body/Tip: 200.0 100.0 7.0
world translation /World/Env_2/Body/Tip: 300.0 100.0 7.0
moved /World/Env_1 at ordinal 4
recomputed world translation /World/Env_1/Body/Tip: 200.0 100.0 47.0
parent of /World/Proto/Body: /World/Proto
children of /World/Proto: /World/Proto/Arm /World/Proto/Body
siblings of /World/Proto/Body: /World/Proto/Arm
```

- `prototype prim types` and `applied schemas` prove the prims exist with zero USD: attribute writes to
  nested paths created them, and the reserved `usd-prim-type` / `usd-schemas`
  columns read back exactly what was stamped.
- The `absent` line shows that the derived `omni:fabric:worldMatrix` column
  does not exist until the client seeds it and `compute_hierarchy` runs. The
  `world translation` lines are the computed compositions, read after the
  compute and the seal of ordinal 3.
- The `moved` / `recomputed` pair shows that a later edit needs another
  compute: the ordinal-4 write lands in the derived rows only after the second
  `compute_hierarchy` — the tip's z becomes 47.0.
- `parent of`, `children of`, and `siblings of` come from `get_hierarchy` lookups against the write-created prims.

## Build and run

The example is a [uv](https://docs.astral.sh/uv/) project pinning the released
`ovstage` wheel (see `pyproject.toml`). The wheel bundles the `ovstage`
shared library at `<package>/bin`, which the bindings load automatically:

```bash
uv run main.py
```

> **Pre-release:** if `uv` cannot resolve the pinned `ovstage` wheel, no package
> index available to you carries it yet — check the repository releases page for
> current availability.


## Snippets

The `[snippet:name]` markers in [main.py](main.py) identify the source regions described below and referenced by the [ovstage skills](../../../skills/). Keep the markers intact when editing.

- `setup` — Imports NumPy and the ovstage types used to write attributes, read values, and select a hierarchy computation model. The program creates its stage and path dictionary in `main()`.
- `insert-author-subtree` — Calls `write_attribute()` with `PrimMode.INSERT` to create the prototype prims at nested paths. Each prim receives a `proto:part` value at ordinal 1; INSERT requires that the prims do not already exist.
- `author-prim-types` — Uses `intern_token()` to turn type names such as `Xform` and `Cube` into identifiers, then writes one `uint64` identifier per prim to `usd-prim-type`. Later reads resolve these identifiers back to type names.
- `author-applied-schemas` — Writes the `PhysicsRigidBodyAPI` and `PhysicsMassAPI` identifiers as an array on `/World/Proto/Body`. Each `usd-schemas` write replaces the prim's complete applied-schema set; an empty array clears it.
- `author-local-transforms` — Uses `_matrix_rows()` to prepare one 4×4 float64 matrix per prim and writes `omni:xform` with the `MATRIX` semantic. Waiting for `advance_write_floor(ordinal=1)` makes the completed authoring step available to readers.
- `clone-prototype-envs` — Calls `clone()` once to copy `/World/Proto` to three environment paths. `_write_local_translation()` gives each new root a different X translation, then the snippet advances the write floor to 2.
- `hierarchy-computation-models` — Calls `get_hierarchy_computation_models()` and prints the available model names. This example uses `GPU_INCREMENTAL` for its client-authored hierarchy and requires a CUDA-capable GPU.
- `world-matrix-staleness` — Reads the example's initially absent world matrices, writes placeholder matrices, then calls `compute_hierarchy()` at ordinal 3. `_read_world_translation()` reads the computed result for each tip; `/World/Env_0/Body/Tip` ends at `(100, 100, 7)`.
- `recompute-after-edit` — Moves `/World/Env_1` at ordinal 4, calls `compute_hierarchy()` again, and advances the write floor before reading. The updated tip translation is `(200, 100, 47)`.
- `hierarchy-lookups` — The `_lookup()` helper calls `get_hierarchy()` for a requested parent, children, or siblings relation and returns the resulting path strings. The caller sorts these strings for display because relation results have no guaranteed order.

## Notes

- **A CUDA-capable GPU is required.** In this ovstage revision the GPU models
  recompute world transforms for client-authored prims; `cpu-incremental` only
  derives them during USD population flows. Without a GPU the program prints a
  clear error and exits 1.
- Reading the derived column between an edit and the recompute is not a
  reliable staleness probe: the read itself can pull an incremental refresh.
  Only an explicit compute guarantees rows consistent with the latest
  committed inputs, so the program reads only after it. The current
  implementation ignores `input_ordinal`.
- `usd-schemas` rows are whole-set assignments: a write replaces the prim's
  full schema set, and an empty row clears it. Elements read back unordered, so
  the program prints them sorted; `get_hierarchy` results are unordered too.
- The derived built-ins `usd-path`, `usd-parent`, and `usd-children` are
  read/filter-only — never write them. (`usd-active` is not supported at all:
  reads/filters on it return `NOT_SUPPORTED`, and the name is subject to
  removal in a future release.)
- There is no move/re-parent API. Hierarchy derives from the path structure, so
  re-parent with data operations: clone the subtree to its new path
  (`stage.clone`), then delete the old one (`stage.delete_attributes(query, [],
  ordinal)` — an empty attribute list deletes whole prims), both at ordinals
  above the write floor.
- The example fails fast: unexpected errors raise and exit nonzero. A real
  application would handle them instead.

