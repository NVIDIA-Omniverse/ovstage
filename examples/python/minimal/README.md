# ovstage minimal (Python)

A small, standalone Python program that shows the core ovstage **Python
bindings** in one pass: create a stage, intern paths/tokens via the path
dictionary, write an attribute column, advance the write floor, and read it
back. It is the Python sibling of `../../c/minimal/main.cpp`.

## At a glance

1. Create a stage and borrow the path dictionary its instance owns.
2. Intern the attribute name "temperature" into a stable integer token, resolve it back, and print both.
3. Build a prim-path list for three prims and open a query over them.
4. Write one float per prim at ordinal 1 — the first write also creates the prims.
5. Advance the write floor to 1 to seal the write, then read the column back and print it.
6. Write again, passing the attribute as a plain string instead of a token — both forms work.
7. Clone the subtree under one prim to two new targets in a single call.
8. Write and read one more round of values through DLPack — zero-copy exchange with numpy.

## What you'll see

```
attribute token <N> = temperature
read back ordinal 1 [1. 2. 3.]
cloned /World/A -> A_env0, A_env1
dlpack read back ordinal 4 [7. 8. 9.]
```

- The token line shows interning: the path dictionary maps `temperature` to a
  stable integer token (`<N>` — the value is not fixed), and resolving the
  token returns the same string.
- The read-back line is the whole write/seal/read cycle: an **ordinal** is a
  version number the application picks for each write, and advancing the
  **write floor** to 1 seals everything up to that ordinal so readers can
  trust it.
- `cloned /World/A` and `dlpack read back ordinal 4` show the multi-environment clone and the DLPack round trip.

## Build and run

The example depends on a published `ovstage` wheel (pinned in
`pyproject.toml`); [uv](https://docs.astral.sh/uv/) resolves, installs, and
runs it in one step. Requirements: Python 3.10–3.13 and `uv`.

```bash
uv run main.py
```

The `ovstage` wheel bundles the native library under `<package>/bin`, which
the bindings search automatically — so a published install needs **no**
loader-path setup. To run against a locally built `libovstage` instead, put
the build output dir (the one holding `ovstage-dynamic.dll` / `libovstage-dynamic.so`) on the
**loader path** — `PATH` on Windows, `LD_LIBRARY_PATH` on Linux — and run with
a plain interpreter:

```bash
pip install numpy
# Linux
export LD_LIBRARY_PATH=/path/to/ovstage/build/output:$LD_LIBRARY_PATH
# Windows (PowerShell)
#   $env:PATH = "C:\path\to\ovstage\build\output;$env:PATH"
python main.py
```

> Note: instead of `PATH` / `LD_LIBRARY_PATH`, you can point the loader at the build dir with
> `OVSTAGE_LIBRARY_PATH_HINT` — as an environment variable, or as the module-global in-process
> override (`ovstage._src.bindings.OVSTAGE_LIBRARY_PATH_HINT`, which takes precedence).


## Snippets

The `[snippet:name]` markers in [main.py](main.py) identify the source regions described below and referenced by the [ovstage skills](../../../skills/). Keep the markers intact when editing.

- `setup` — Imports NumPy, ovstage, and the tensor, ordinal-range, and error types used throughout the example. `main()` creates the stage and path dictionary before running the snippets.
- `intern-and-resolve` — Calls `intern_token("temperature")` to obtain an attribute identifier, then `token_to_string()` to recover its name. The printed identifier is assigned at runtime; the resolved name is `temperature`.
- `path-list-query` — Uses `create_path_list_from_strings()` and `query_from_path_list()` to select `/World/A`, `/World/B`, and `/World/C`. Close the query before destroying its path list; the surrounding context manager and cleanup handle this in `main()`.
- `minimal-write-read` — Writes temperatures `[1, 2, 3]` at ordinal 1, waits for completion, and advances the write floor. `read_attributes()` then reads the column, and `column.array(0)` exposes its values while the group is valid; the example releases the group after printing.
- `string-or-token-arg` — Passes `"temperature"` directly to `write_attribute()` instead of its interned token. It updates the same column to `[4, 5, 6]` at ordinal 2, demonstrating that either argument form identifies the attribute.
- `clone-subtree-multienv` — Calls `clone()` to copy `/World/A` and its subtree to `/World/A_env0` and `/World/A_env1` at ordinal 3. The snippet advances the write floor before later reads of the copied data.
- `dlpack-interchange` — Wraps NumPy values `[7, 8, 9]` with `DLTensor.from_dlpack()` for a write, then reads them with `np.from_dlpack(read_column.dlpack(0))`. It accounts for a data-row index map when present and finishes using the returned array before releasing the group.
- `error-handling` — The reference helper `write_checked()` waits for a write and returns `True` on success. If an `OvstageError` occurs, it prints the code and message and returns `False`; this helper is provided for reuse and is not called by `main()`.
- `nonblocking-poll` — The reference helper `poll_until_done()` repeatedly calls `wait_op(timeout=0)`, continuing on `TIMEOUT` and reporting failures. It releases the operation record after completion; this helper is also separate from the main demonstration.

## Notes

- The writes use UPSERT prim mode (create-or-update), so the first write at
  ordinal 1 creates the three prims — no USD is involved.
- The example fails fast: unexpected exceptions propagate and exit nonzero. A
  real application would catch `OvstageError` / `OvxError` (see the
  `error-handling` snippet).
- Everything runs on the CPU; no GPU is needed.

