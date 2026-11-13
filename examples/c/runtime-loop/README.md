# ovstage runtime loop (C/C++)

A standalone C++ program that shows the ovstage **runtime loop** via the core
**C API**: load a USD scene, populate it (copy it) into the ovstage runtime
table, read it back, update it two ways, and read again — with **no renderer
attached**. It is the C sibling of `../../python/runtime-loop/main.py`.

## At a glance

1. Populate `torus-plane.usda` into the ovstage runtime table at ordinal 1 —
   an ordinal is a version number the application picks — and seal it by
   advancing the write floor.
2. Read the reserved `usd-prim-type` metadata back to confirm the scene landed.
3. Update path 1 — write the Torus transform straight into the runtime table,
   one sealed ordinal per frame for 24 frames, with no USD round-trip.
4. Read the final transform back: the Torus slid 100 units along +X.
5. Update path 2 — reference an inline USDA layer onto `/World/EditCube`, then
   apply the USD changes so the new prim propagates into the runtime table.
6. Read the prim types one last time — the Cube is now visible — and release
   everything.

The application owns the ordinal lifecycle throughout, sealing each ordinal
with `ovstage_advance_write_floor` before reading:

```text
ordinal  1          2 .. 25            26
         populate   animation frames   USD edit applied
```

## What you'll see

```text
populated prim types: Xform Mesh Mesh
final Torus xform translation (row [3][0:3]): 100.0 25.0 0.0
after USD edit, prim types: Xform Mesh Mesh Cube
```

- `populated prim types` confirms the populate: `/World` (an Xform) plus the Plane and Torus meshes from `torus-plane.usda`.
- `final Torus xform translation` shows the direct runtime update. The scene is Y-up with the Torus authored at `translate = (0, 25, 0)`, so it slid 100 units along +X, keeping y=25.
- `after USD edit, prim types` shows the update through the USD source. `/World/EditCube` existed only in a USD reference edit; `ovstage_population_apply_usd_changes` propagates it into the runtime table as a fourth prim.

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
./build/runtime-loop
```

```powershell
# Windows
cmake -B build
cmake --build build --config Release
# ovstage discovers its bundled plugins relative to where ovstage.dll loads
# from, so keep the package bin/ intact and put it on PATH (do not copy the
# DLL next to the exe):
$env:PATH = "<ovstage-package>\bin;$env:PATH"
.\build\Release\runtime-loop.exe
```

The build copies `torus-plane.usda` next to the binary, so run from this
directory or from the build output dir (or pass a scene path as `argv[1]`).
On Linux the build sets an rpath onto the package `bin/`. To build every C
example at once, configure from the parent directory (`../CMakeLists.txt`
aggregates them).


## Snippets

The `[snippet:name]` markers in [main.cpp](main.cpp) identify the source regions described below and referenced by the [ovstage skills](../../../skills/). Keep the markers intact when editing.

- `setup` — Includes the ovstage, population, and path-dictionary interfaces plus the C++ utilities used throughout the example. The shared example helpers provide status checking, waits, and tensor descriptions.
- `populate` — Calls `ovstage_population_open_usd_from_file()` to load rendering data at time 0 and ordinal 1. After waiting for population, it waits for `ovstage_advance_write_floor()` so readers can access that state.
- `read-populated` — `readPrimTypes()` reads `usd-prim-type` for the selected paths and resolves the returned tokens to names. It returns those names after releasing read groups and the read handle; the caller prints them to confirm population.
- `update-table` — Calls `ovstage_write_attribute()` to move the Torus over 24 frames, assigning and sealing a new ordinal for each frame. It then reads the final `omni:xform` and prints translation `(100, 25, 0)` before releasing the read resources.
- `update-usd` — Calls `ovstage_population_add_usd_reference_from_string()` to add `/World/EditCube` to the USD source, then `ovstage_population_apply_usd_changes()` at a fresh ordinal. After waiting and advancing the write floor, a query over the expanded path list confirms the new prim's type.

## Notes

- The example uses the canonical `omni:xform` layout: **one 16-lane float64
  element per prim** (`ndim=1`, `dtype={kDLFloat, 64, 16}`,
  `OVSTAGE_SEMANTIC_MATRIX`). A convenience input shaped `[N, 4, 4]` with
  `lanes=1` is also accepted. Raw reads and maps return `[N, 4, 4]` with scalar lanes. The memory remains row-major 4×4 with
  translation in elements `[12..14]`.
- The inline USDA layer needs the prim body braces and each statement on their
  own lines; a single-line `def ... { ... }` is a parse error.
- The examples fail fast: any unexpected API failure prints and exits (helpers
  in `../common/ovstage_example_utils.h`). A real application would propagate
  errors instead.
- `torus-plane.usda` is the ovrtx minimal example scene, copied next to this
  file so the example stays self-contained.
- Everything runs on the CPU; no GPU is needed.

