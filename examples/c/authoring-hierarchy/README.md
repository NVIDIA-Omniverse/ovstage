# ovstage authoring hierarchy (C/C++)

A standalone C++ program that shows pure client-side authoring and the
hierarchy APIs through the core **C API**: it builds a multi-environment world
with **zero USD** — prims come into existence via attribute writes to nested
paths. It then shows that derived world transforms are **pull-computed**: the
rows start absent, the client seeds them, and a hierarchy computation model
fills or refreshes them. It is the C sibling of
`../../python/authoring-hierarchy/main.py`.

## At a glance

1. Create prims by writing attributes to nested paths — no USD anywhere; the hierarchy comes from the paths.
2. Stamp prim types (`usd-prim-type`) and applied schemas (`usd-schemas`) client-side.
3. Write each prim's local transform (`omni:xform`) and clone the prototype into three environments.
4. Seed the derived world-matrix rows, run the compute, seal the ordinal, and read correct world translations.
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

- `prototype prim types` and `applied schemas` read back the client-stamped `usd-prim-type` (one type
  token per prim) and `usd-schemas` (a token array per prim) — the prims exist
  with zero USD and no parenting call.
- The `absent` line shows that the derived `omni:fabric:worldMatrix` column
  does not exist until the client seeds it and `ovstage_compute_hierarchy`
  runs. The `world translation` lines are the computed compositions, read
  after the compute and the seal of ordinal 3.
- The `moved` / `recomputed` pair shows that a later edit needs another compute
  — the tip's z becomes 47.0. The computation uses the latest committed
  inputs. The current implementation ignores `input_ordinal`.
- `parent of`, `children of`, and `siblings of` come from `ovstage_get_hierarchy` lookups
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

The `[snippet:name]` markers in [main.cpp](main.cpp) identify the source regions described below and referenced by the [ovstage skills](../../../skills/). Keep the markers intact when editing.

- `insert-author-subtree` — Calls `ovstage_write_attribute()` with `OVSTAGE_PRIM_MODE_INSERT` to create the prototype prims at nested paths. Each receives a `proto:part` value at ordinal 1; INSERT requires that the selected prims do not already exist.
- `author-prim-types` — Interns type names such as `Xform` and `Cube`, then writes one `uint64` token per prim to `usd-prim-type`. Later reads resolve the tokens back to type names.
- `author-applied-schemas` — Writes the `PhysicsRigidBodyAPI` and `PhysicsMassAPI` tokens as an array on `/World/Proto/Body`. Each `usd-schemas` write replaces the prim's complete applied-schema set; an empty array clears it.
- `author-local-transforms` — Uses `writeMatrixRows()` to write one 16-lane float64 matrix per prim to `omni:xform` with the matrix semantic. It advances the write floor to 1 so readers can access the completed authoring step.
- `clone-prototype-envs` — Calls `ovstage_clone()` once to copy `/World/Proto` to three environment paths, then waits for completion. `writeLocalTranslation()` gives each new root a different X translation before the write floor advances to 2.
- `hierarchy-computation-models` — Calls `ovstage_get_hierarchy_computation_models()` and prints the available model names. This example selects GPU incremental computation for its client-authored hierarchy and requires a CUDA-capable GPU.
- `world-matrix-staleness` — Checks the example's initially absent world matrices, writes placeholders, then waits for `ovstage_compute_hierarchy()` at ordinal 3. Reading the computed translations gives `(100, 100, 7)` for `/World/Env_0/Body/Tip`.
- `recompute-after-edit` — Moves `/World/Env_1` at ordinal 4, waits for another hierarchy computation, and advances the write floor before reading. The updated tip translation is `(200, 100, 47)`.
- `hierarchy-lookups` — `lookupHierarchyPaths()` submits `ovstage_get_hierarchy()` for a parent, children, or siblings relation and copies the fetched path strings into its return value. It releases the result, lookup handle, and path list; the caller sorts the strings because relation results have no guaranteed order.

## Notes

- **A CUDA-capable GPU is required.** In this ovstage revision the GPU models
  recompute world transforms for client-authored prims; `cpu-incremental` only
  derives them during USD population flows. Without a GPU the compute op fails
  and the program exits 1.
- Reading the derived column between an edit and the recompute is not a
  reliable staleness probe: the read itself can pull an incremental refresh.
  Only an explicit compute guarantees rows consistent with the latest
  committed inputs, so the program reads only after it. The current
  implementation ignores `input_ordinal`.
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

