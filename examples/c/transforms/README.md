# ovstage transforms (C/C++)

Change a wheel's local transform and its inheritance from a parent, then compute
and read its world transform. The fixture [car.usda](car.usda) places
`/World/Car` at x = 10; the example writes the wheel's local x = 4.
The executable uses CPU incremental hierarchy computation.

The [Python example](../../python/transforms/README.md) covers the same workflow.

## At a glance

A local transform is input to hierarchy computation. The world matrix is derived
output: writing the local matrix alone does not refresh it. The reset flag controls
whether the wheel inherits the car's transform.

Pseudocode (workflow only):

```text
populate all Xforms at ordinal 1; finish writes through 1
at ordinal 2: write wheel local x = 4 and reset = false
compute hierarchy at 2; finish writes through 2
at ordinal 3: set reset = true, keeping the same local matrix
compute hierarchy at 3; finish writes through 3
read the wheel's derived world matrix at 3
```

## What you'll see

```text
Wheel world x: 4.0
```

With inheritance enabled, car x = 10 plus wheel x = 4 gives world x = 14.
The example then enables reset, so the wheel ignores the parent and ends at world
x = 4. Only this final value is printed. If the read produces no derived row,
the program prints `No derived world-matrix row is available.` instead.

## How to read the code

Start in `main()` in [main.cpp](main.cpp). The names below are searchable
`[snippet:name]` markers in that file; each row explains why that section is there.

| Source markers | What to learn |
|---|---|
| `transform-layout-c` | Prepare one row-major float64 4-by-4 matrix and one boolean per prim. Translation occupies the last row. |
| `transform-write-c` | Write the local matrix with MATRIX semantics and the reset flag at the same ordinal. |
| `transform-compute-c` | Check that CPU incremental computation is available, compute the hierarchy, then advance the floor. Computation also writes output, so it comes before closing the ordinal. |
| `transform-override-c` | Change only the runtime reset flag and recompute; the source USD and local matrix stay unchanged. |
| `transform-read-c` | Read the derived world-matrix attribute, honoring the read column's data index map when locating the wheel's tensor row. |

## Build and run

Use a package with population and CPU incremental hierarchy computation. See the [shared setup](../../README.md) for package prerequisites.
Run these commands from this example's directory.

Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build
./transforms
```

Windows (multi-configuration generator):

```powershell
cmake -S . -B build
cmake --build build --config Release
$env:PATH = "<ovstage-package>\bin;$env:PATH"
Set-Location build\Release
.\transforms.exe
```

Keep the package's `bin/` directory intact so the library can find its plugins.
Linux builds set an rpath to that directory. The project uses the shared
[cmake](../cmake/) and [common](../common/) directories; retain them if copying it.
CMake copies the USD fixtures beside the executable; run from that directory.

## Notes

- `omni:xform` and `omni:resetXformStack` are the writable inputs used here.
  `omni:fabric:worldMatrix` is the derived output; do not write it directly.
- Population includes every Xform to seed the hierarchy and its output rows.
- [gpu_write.cpp](gpu_write.cpp) is an optional companion for applications that already
  own CUDA buffers; the main program does not call it. It uses GPU global computation
  on a populated hierarchy with existing derived-output rows. Record a CUDA readiness
  event after filling both buffers; the writes wait on it. Keep the buffers and the
  event valid and unchanged until the helper returns.
  Configure with `-DOVSTAGE_EXAMPLE_CUDA=ON` to compile this helper;
  it is an object library, not a second runnable example.

## Further reading

- [Transform inputs and hierarchy computation](../../../docs/scene/transforms.rst)
- [Authoring and hierarchy example](../authoring-hierarchy/README.md)
