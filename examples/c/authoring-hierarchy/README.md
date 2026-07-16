# ovstage authoring hierarchy (C/C++)

A standalone C++ program that shows pure client-side authoring and the
hierarchy APIs through the core **C API**: it builds a multi-environment world
with **zero USD** — prims come into existence via attribute writes to nested
paths — then shows that derived world transforms are **pull-computed**: nothing
fills them in until a hierarchy computation model runs. It is the C sibling of
`../../python/authoring-hierarchy/main.py`.

## At a glance

1. Create prims by writing attributes to nested paths — no USD anywhere; the hierarchy comes from the paths.
2. Stamp prim types (`usd-prim-type`) and applied schemas (`usd-schemas`) client-side.
3. Write each prim's local transform (`omni:xform`) and clone the prototype into three environments.
4. Show the staleness rule: derived world matrices don't exist until you compute them; run the compute and read correct world translations.
5. Move one environment and recompute to fold the edit into the derived rows.
6. Look up parent, children, and siblings of a prim.

Every transform is a pure translation, so each Tip's world translation is the
sum of the locals down its chain:

```text
/World          local (0, 100, 0)
  Proto         local (10, 0, 0)   -- cloned to Env_0/Env_1/Env_2, locals (100/200/300, 0, 0)
    Arm         local (0, 0, 3)
    Body        local (0, 0, 5)
      Tip       local (0, 0, 2)    -- world (10, 100, 7); Env_1's Tip (200, 100, 7)
```

## What you'll see

```
prototype prim types: Xform Xform Cube Xform Cube
applied schemas on /World/Proto/Body: PhysicsMassAPI PhysicsRigidBodyAPI
cloned /World/Proto -> /World/Env_0 /World/Env_1 /World/Env_2
hierarchy computation models: cpu-incremental gpu-incremental gpu-global
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

- The first two lines read back the client-stamped `usd-prim-type` (one type
  token per prim) and `usd-schemas` (a token array per prim) — the prims exist
  with zero USD and no parenting call.
- The `absent` and `stale placeholder` lines are the heart of the example: the
  derived `omni:fabric:worldMatrix` column does not exist until the client
  seeds it, and the seeded rows keep their zeros until
  `ovstage_compute_hierarchy` runs.
- The `moved` / `recomputed` pair shows that a later edit needs another compute
  with the new input ordinal — the tip's z becomes 47.0.
- The last three relation lines come from `ovstage_get_hierarchy` lookups
  against the write-created prims.

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
./build/authoring-hierarchy
```

```powershell
# Windows
cmake -B build
cmake --build build --config Release
# ovstage discovers its bundled plugins relative to where ovstage.dll loads
# from, so keep the package bin/ intact and put it on PATH (do not copy the
# DLL next to the exe):
$env:PATH = "<ovstage-package>\bin;$env:PATH"
.\build\Release\authoring-hierarchy.exe
```

On Linux the build sets an rpath onto the package `bin/`, so the binary runs
from anywhere with no environment setup (no assets needed). To build every C
example at once, configure from the parent directory (`../CMakeLists.txt`
aggregates them).


## Snippets

The `[snippet:name]` markers in `main.cpp` fence regions referenced by the
ovstage skills under `../../../skills/`; keep them intact when editing.

- `insert-author-subtree` — create prims via an INSERT (create-only) write to nested paths
- `author-prim-types` — stamp `usd-prim-type` (scalar uint64 token ids)
- `author-applied-schemas` — stamp `usd-schemas` (token-id array; whole-set rows)
- `author-local-transforms` — `omni:xform` as ONE 16-lane float64 element per prim
- `clone-prototype-envs` — clone one prototype to N environments in one call
- `hierarchy-computation-models` — enumerate the runtime's computation models
- `world-matrix-staleness` — absent → seeded placeholder (stale) → compute → correct
- `recompute-after-edit` — a later local edit needs another compute to land
- `hierarchy-lookups` — parent/children/siblings via `ovstage_get_hierarchy`

## Notes

- **A CUDA-capable GPU is required.** In this ovstage revision the GPU models
  recompute world transforms for client-authored prims; `cpu-incremental` only
  derives them during USD population flows. Without a GPU the compute op fails
  and the program exits 1.
- Reading the derived column between an edit and the recompute is not a
  reliable staleness probe: the read itself can pull an incremental refresh.
  Only an explicit compute guarantees rows consistent with the new input
  ordinal, so the program reads only after it.
- `usd-schemas` rows are whole-set assignments: a write replaces the prim's
  full schema set, and an empty row clears it. Elements read back unordered
  (printed sorted here); `ovstage_get_hierarchy` results are unordered too.
- The derived built-ins `usd-path`, `usd-parent`, and `usd-children` are
  read/filter-only — never write them. (`usd-active` is not supported at all:
  reads/filters on it return `NOT_SUPPORTED`, and the name is subject to
  removal in a future release.)
- There is no move/re-parent API. Hierarchy derives from the path structure, so
  re-parent with data operations: clone the subtree to its new path
  (`ovstage_clone`), then delete the old one (`ovstage_delete_attributes` with
  an empty attribute list deletes whole prims), both at ordinals above the
  write floor.
- The examples fail fast: any unexpected API failure prints and exits (helpers
  in `../common/ovstage_example_utils.h`). A real application would propagate
  errors instead.

