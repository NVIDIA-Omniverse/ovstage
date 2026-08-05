# ovstage authoring hierarchy (Python)

A standalone Python program that shows pure client-side authoring and the
hierarchy APIs: it builds a multi-environment world with **zero USD** — prims
come into existence via attribute writes to nested paths — then shows that
derived world transforms are **pull-computed**: nothing fills them in until a
hierarchy computation model runs. It is the Python sibling of
`../../c/authoring-hierarchy/main.cpp` (same sections, byte-identical output).

## At a glance

1. Create prims by writing attributes to nested paths — no USD anywhere; the hierarchy comes from the paths.
2. Stamp prim types (`usd-prim-type`) and applied schemas (`usd-schemas`) client-side.
3. Write each prim's local transform (`omni:xform`) and clone the prototype into three environments.
4. Show the staleness rule: derived world matrices don't exist until you compute them; run the GPU compute and read correct world translations.
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
stale placeholder world translation /World/Env_0/Body/Tip: 0.0 0.0 0.0
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

- The first two lines prove the prims exist with zero USD: attribute writes to
  nested paths created them, and the reserved `usd-prim-type` / `usd-schemas`
  columns read back exactly what was stamped.
- The `absent` and `stale placeholder` lines are the heart of the example: the
  derived `omni:fabric:worldMatrix` column does not exist until the client
  seeds it, and the seeded zeros stay stale until `compute_hierarchy` runs.
- The `moved` / `recomputed` pair shows that a later edit needs another
  compute: the ordinal-4 write lands in the derived rows only after the second
  `compute_hierarchy` — the tip's z becomes 47.0.
- The last relation lines come from `get_hierarchy` lookups against the
  write-created prims.

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

The `[snippet:name]` markers in `main.py` fence regions referenced by the
ovstage skills under `../../../skills/`; keep them intact when editing.

- `setup` — imports (`numpy`, `ovstage` types)
- `insert-author-subtree` — create prims via an INSERT (create-only) write to nested paths
- `author-prim-types` — stamp `usd-prim-type` (scalar uint64 token ids)
- `author-applied-schemas` — stamp `usd-schemas` (token-id array; whole-set rows)
- `author-local-transforms` — `omni:xform` as ONE 16-lane float64 element per prim
- `clone-prototype-envs` — clone one prototype to N environments in one call
- `hierarchy-computation-models` — enumerate the runtime's computation models
- `world-matrix-staleness` — absent → seeded placeholder (stale) → compute → correct
- `recompute-after-edit` — a later local edit needs another compute to land
- `hierarchy-lookups` — parent/children/siblings via `get_hierarchy`

## Notes

- **A CUDA-capable GPU is required.** In this ovstage revision the GPU models
  recompute world transforms for client-authored prims; `cpu-incremental` only
  derives them during USD population flows. Without a GPU the program prints a
  clear error and exits 1.
- Reading the derived column between an edit and the recompute is not a
  reliable staleness probe: the read itself can pull an incremental refresh.
  Only an explicit compute guarantees rows consistent with the new input
  ordinal, so the program reads only after it.
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

