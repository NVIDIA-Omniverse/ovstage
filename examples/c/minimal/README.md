# ovstage minimal (C/C++)

A standalone C++ program that shows the core ovstage **C API** in one pass:
create an instance, intern paths via the path dictionary, write an attribute
column, advance the write floor, read it back, and clone a subtree. It is the
C sibling of `../../python/minimal/main.py` and the source of the C snippets
referenced by the ovstage skills.

## At a glance

1. Create an ovstage instance and get its path dictionary (the instance owns it).
2. Intern the attribute name "temperature" into a stable integer token, resolve it back, and print both.
3. Build a prim-path list for three prims and open a query over them.
4. Write one float per prim at ordinal 1 — the first write also creates the prims.
5. Advance the write floor to 1 so readers can trust the write.
6. Read the column back at ordinal 1 and print the three values.
7. Clone the subtree under one prim to two new targets in a single call.
8. Release every handle and destroy the instance.

## What you'll see

```text
attribute token <N> = 'temperature'
read back ordinal 1: 1.0 2.0 3.0
cloned /World/A -> A_env0, A_env1
```

- The token value `<N>` is not fixed: interning maps the string `temperature`
  to a stable integer token, and resolving the token returns the same string.
- An **ordinal** is a version number the application picks for each write;
  advancing the **write floor** to 1 seals everything up to it, and the read
  at ordinal 1 returns exactly the three values written.
- The clone step clones at ordinal 2 and seals it, mirroring the Python
  sibling, then prints a confirmation.

## Build and run

The example builds standalone with CMake: `find_package(ovstage)` locates an
installed package, otherwise the build fetches the released package zip (see
`../cmake/ovstage.cmake`).

```bash
# Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/minimal
```

```powershell
# Windows
cmake -B build
cmake --build build --config Release
# ovstage discovers its bundled plugins relative to where ovstage.dll loads
# from, so keep the package bin/ intact and put it on PATH (do not copy the
# DLL next to the exe):
$env:PATH = "<ovstage-package>\bin;$env:PATH"
.\build\Release\minimal.exe
```

On Linux the build sets an rpath onto the package `bin/`, so the binary runs
from anywhere with no environment setup (no assets needed). To build every C
example at once, configure from the parent directory (`../CMakeLists.txt`
aggregates them).


## Snippets

The `[snippet:name]` markers in `main.cpp` fence regions referenced by the
ovstage skills under `../../../skills/`; keep them intact when editing.

- `check-sync-error` — fail-fast check of a synchronous `ovstage_api_status_t`
- `enqueue-wait-error` — drive an async enqueue to completion, report per-op errors, release the op
- `intern-and-resolve` — path-dictionary intern + resolve back to a string
- `string-view-from-ovx-string` — wrap `ovx_string_t` in `std::string_view`
- `path-list-query` — build a prim-path list and open a query
- `string-or-token-arg` — pass an attribute as `ovx_string_or_token_t` (token form)
- `minimal-write-read` — write → advance write floor → read back
- `clone-subtree-multienv` — clone one prim's subtree to several targets in one call

## Notes

- The write uses UPSERT prim mode (create-or-update), so the first write
  creates the three prims — no scene file or USD is involved.
- The example fails fast: any unexpected API failure prints and exits. Unlike
  its siblings it does not include `../common/ovstage_example_utils.h` — the
  checking helpers are themselves snippet sources here. A real application
  would propagate errors instead.
- `ovstage_destroy_instance` requires every op and handle released first —
  hence the releases at the end of `main`.
- Everything runs on the CPU; no GPU is needed.

