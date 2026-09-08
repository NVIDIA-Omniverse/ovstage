# AGENTS.md — ovstage public tests (tested-doc contract)

This directory is the **public-contract test layer** for ovstage: tests that run
against the *produced* ovstage package (headers + wheel), double as the single
source of truth for doc/skill snippets, and are included in the public source.

> **Status: active.** The public C + Python suites (write→read round-trip and USD
> population) run in CI against the produced package/wheel, and their snippets feed
> the ovstage skills (see the inventory below). Coverage grows over time.


## Why this exists

Hand-written code blocks in docs and skills go stale: wrong signatures, missing
setup, patterns that don't compile. By requiring every public code example to
live in a **tested** file with snippet markers, the docs/skills break when the API
changes — which is the point. These tests validate the *published surface* (the
shipped headers, the wheel, the examples, the skills), not the shared-library
implementation behind the package.


## CPU-only

Every public ovstage test is **CPU-only**. ovstage's public surface is USD-stage
read/write (path dictionary, ordinal-keyed attribute columns, clone) exercised
with `kDLCPU` DLPack tensors — no GPU is required to prove the contract. This
keeps the runner matrix simple (no GPU tags, no driver requirements).

**Expanding to GPU later is additive**, not a restructure: add GPU-marked cases to
the existing suites (or a third suite) with the appropriate runner tag and
dependency set. Nothing in this layout precludes it. Until then, do not author
public tests that require a GPU.

## Architecture

### Subprojects

| Directory | Deps | Purpose |
|-----------|------|---------|
| `c/`      | CMake, GoogleTest, ovstage C API | Test the C API snippets (create instance → path dictionary → write/advance floor/read → clone) |
| `python/` | `pytest`, `numpy`, `ovstage` wheel | Test the Python API snippets (same workflows via the ctypes bindings) |

The C suite builds via CMake + GoogleTest and runs via `ctest`. The Python suite
runs via `pytest`. Both build and run against the produced package, not the build
tree.

There is no separate `usd/` suite. Runtime-to-USD export is covered through both
public-contract suites. Both use the saved-file and reusable-destination APIs and
inspect the persisted USD through public test/report behavior and the saved text,
without coupling to OpenUSD C++ ABI types. ovstage owns every export
destination, so no public route authors into a caller-owned layer or stage and
no public test may assume the caller shares ovstage's USD runtime.

### Data flow: test → snippet → skill → doc

```
test file (tests/c/*.cpp, tests/python/*.py)
    contains snippet markers: // [snippet:name] ... // [/snippet:name]  (C)
                              # [snippet:name] ... # [/snippet:name]    (Python)
        |
        v
skill (../skills/*/SKILL.md)  — references snippets via > **Source:** directives
        |
        v
RST doc (../docs/**/*.rst) — literalinclude with :start-after:/:end-before:
```

Skills and RST docs are both live consumers. When you add or change a snippet,
update every skill and doc that references it.

## Rules

These rules are mandatory.

1. **Snippets are the source of truth.** Never write inline API code blocks in a
   skill or RST doc. Substantial examples live in a test file with snippet markers
   and are pulled in by reference. Small one-liners may stay inline.
   A runnable shipped example may also carry a paired marker for a skill's
   operational recipe, but it supplements rather than replaces asserted test
   coverage; list the asserted test snippet in the inventory below.
2. **Test against the produced package.** Suites build/run against the shipped
   headers and wheel, so a signature drift fails the test.
3. **Snippet naming.** kebab-case, unique across the whole `tests/` tree. The C
   equivalent of a Python snippet appends `-c` (e.g. `minimal-write-read` /
   `minimal-write-read-c`).
4. **Preserve markers.** Moving/restructuring marked code moves the markers with
   it. Removing a marker requires removing/updating every `skills/`/`docs/`
   reference to it in the same change.
5. **Publish-safe.** This tree ships publicly. Keep it self-contained: do not
   include internal-only material or links to private source/docs.



## ovstage specifics to respect in tests

- **Async, ordinal-keyed model.** State-mutating/data-producing calls enqueue and
  return an `op_index`; drive them to completion (`wait_op` in C, `Operation.wait()`
  in Python) and check per-op errors — enqueue success ≠ op success.
- **Latest-snapshot payloads.** This build does not retain historical payloads.
  Bounded dirty metadata may test older change membership at or above
  `ovstage_get_oldest_preserved_ordinal`.
- **Reads target sealed data** at/below the write floor; advance the write floor
  before reading a just-written column.

## Snippet inventory

**Agents MUST keep this table current** — update it in the same change that adds,
renames, moves, or removes a snippet. Each entry is an asserted tested snippet
consumed by an ovstage skill and/or public documentation.

| Snippet | Source file | Used in |
|---------|-------------|---------|
| `write-read-roundtrip-c` | `tests/c/test_minimal.cpp` | `skills/application-flow` |
| `write-read-roundtrip` | `tests/python/test_minimal.py` | `skills/application-flow` |
| `populate-and-query-c` | `tests/c/test_population.cpp` | `skills/loading-usd` |
| `populate-and-query` | `tests/python/test_population.py` | `skills/loading-usd` |
| `usd-reference-c` | `tests/c/test_population.cpp` | `skills/loading-usd` |
| `usd-reference` | `tests/python/test_population.py` | `skills/loading-usd` |
| `reset-usd-c` | `tests/c/test_population.cpp` | `skills/loading-usd` |
| `reset-usd` | `tests/python/test_population.py` | `skills/loading-usd` |
| `open-missing-file-c` | `tests/c/test_population.cpp` | `skills/loading-usd` |
| `open-missing-file` | `tests/python/test_population.py` | `skills/loading-usd` |
| `populate-by-schema-c` | `tests/c/test_population.cpp` | `skills/loading-usd` |
| `populate-by-schema` | `tests/python/test_population.py` | `skills/loading-usd` |
| `populate-narrowed-c` | `tests/c/test_population.cpp` | `skills/loading-usd` |
| `populate-narrowed` | `tests/python/test_population.py` | `skills/loading-usd` |
| `populate-stage-metadata` | `tests/python/test_population.py` | `skills/loading-usd` |
| `populate-several-descs` | `tests/python/test_population.py` | `skills/loading-usd` |
| `register-usd-schemas-c` | `tests/c/test_population.cpp` | `skills/loading-usd` |
| `export-typed-hierarchy-to-usd-c` | `tests/c/test_population_export.cpp` | `skills/exporting-to-usd`, `docs/scene/exporting_to_usd.rst` |
| `export-typed-hierarchy-to-usd-file` | `tests/python/test_population_export.py` | `skills/exporting-to-usd`, `docs/scene/exporting_to_usd.rst` |
| `export-runtime-to-usd` | `tests/python/test_population_export.py` | `skills/exporting-to-usd`, `docs/scene/exporting_to_usd.rst` |
| `export-reusable-destination` | `tests/python/test_population_export.py` | `skills/exporting-to-usd`, `docs/scene/exporting_to_usd.rst` |
| `export-custom-attributes-to-usd` | `tests/python/test_population_export.py` | `docs/scene/exporting_to_usd.rst` |
| `export-schema-attribute-to-usd` | `tests/python/test_population_export.py` | `docs/scene/exporting_to_usd.rst` |
| `export-homogeneous-mesh-subtree` | `tests/python/test_population_export.py` | `skills/exporting-to-usd`, `docs/scene/exporting_to_usd.rst` |
| `export-changed-properties-to-usd` | `tests/python/test_population_export.py` | `docs/scene/exporting_to_usd.rst` |
| `export-runtime-to-usd-async-c` | `tests/c/test_population_export.cpp` | `skills/exporting-to-usd`, `docs/scene/exporting_to_usd.rst` |
| `export-runtime-to-usd-async` | `tests/python/test_population_export.py` | `skills/exporting-to-usd`, `docs/scene/exporting_to_usd.rst` |
| `clone-and-verify-c` | `tests/c/test_clone.cpp` | `skills/clone-subtree-multienv` |
| `clone-and-verify` | `tests/python/test_clone.py` | `skills/clone-subtree-multienv` |
| `clone-target-exists-error-c` | `tests/c/test_error_handling.cpp` | `skills/error-handling` |
| `clone-target-exists-error` | `tests/python/test_error_handling.py` | `skills/error-handling` |
| `query-by-usd-path-c` | `tests/c/test_queries.cpp` | `skills/stage-queries` |
| `query-by-usd-path` | `tests/python/test_queries.py` | `skills/stage-queries` |
| `query-has-attribute-c` | `tests/c/test_queries.cpp` | `skills/stage-queries` |
| `query-has-attribute` | `tests/python/test_queries.py` | `skills/stage-queries` |
| `query-predicate-matrix-c` | `tests/c/test_queries.cpp` | `skills/stage-queries` |
| `query-predicate-matrix` | `tests/python/test_queries.py` | `skills/stage-queries` |
| `query-result-introspection-c` | `tests/c/test_queries.cpp` | `skills/stage-queries` |
| `query-result-introspection` | `tests/python/test_queries.py` | `skills/stage-queries` |
| `canonical-fixed-shapes-c` | `tests/c/test_minimal.cpp` | `skills/dlpack-tensor-exchange` |
| `canonical-fixed-shapes` | `tests/python/test_minimal.py` | `skills/dlpack-tensor-exchange` |
| `attribute-shapes-fixed-c` | `tests/c/test_attribute_shapes.cpp` | `skills/dlpack-tensor-exchange` |
| `attribute-shapes-fixed` | `tests/python/test_attribute_shapes.py` | `skills/dlpack-tensor-exchange` |
| `attribute-shapes-ragged-c` | `tests/c/test_attribute_shapes.cpp` | `skills/dlpack-tensor-exchange` |
| `attribute-shapes-ragged` | `tests/python/test_attribute_shapes.py` | `skills/dlpack-tensor-exchange` |
| `semantic-roles-c` | `tests/c/test_attributes.cpp` | `skills/dlpack-tensor-exchange` |
| `semantic-roles` | `tests/python/test_attributes.py` | `skills/dlpack-tensor-exchange` |
| `path-list-context-manager` | `tests/python/test_path_lists.py` | `skills/path-dictionary` |
| `path-list-loop-reuse` | `tests/python/test_path_lists.py` | `skills/path-dictionary` |
| `read-group-context-manager` | `tests/python/test_read_groups.py` | `skills/dlpack-tensor-exchange` |
| `map-unmap-cpu-c` | `tests/c/test_map_attribute.cpp` | `skills/dlpack-tensor-exchange` |
| `map-unmap-cpu` | `tests/python/test_map_attribute.py` | `skills/dlpack-tensor-exchange` |
| `map-dlpack-readonly` | `tests/python/test_map_attribute.py` | `skills/dlpack-tensor-exchange` |
| `upsert-vs-insert-c` | `tests/c/test_write_modes.cpp` | `skills/error-handling` |
| `upsert-vs-insert` | `tests/python/test_write_modes.py` | `skills/error-handling` |
| `sparse-index-map-and-mask-c` | `tests/c/test_sparse_writes.cpp` | _(test only)_ |
| `sparse-index-map-and-mask` | `tests/python/test_sparse_writes.py` | _(test only)_ |
| `delete-attribute-then-prim-c` | `tests/c/test_delete.cpp` | _(test only)_ |
| `delete-attribute-then-prim` | `tests/python/test_delete.py` | _(test only)_ |
| `log-callback-filter-c` | `tests/c/test_logging.cpp` | `skills/logging` |
| `log-callback-filter` | `tests/python/test_logging.py` | `skills/logging` |
| `version-and-error-c` | `tests/c/test_support_api.cpp` | `skills/error-handling` |
| `dlpack-round-trip` | `tests/python/test_support_api.py` | `skills/dlpack-tensor-exchange` |
| `configure-transform-updates` | `tests/python/test_config.py` | `docs/guides/project_setup_python.rst` |

These are asserted tested snippets (they verify behavior, not just print it). Most
feed an ovstage skill; a few are `(test only)` until a matching skill exists. New
tested snippets are added here as public-surface coverage grows.

## Running

`run_public_tests.py` is the cross-platform runner (Linux/Windows) used by CI. It
consumes a produced package the way a consumer would: the C tests
(`find_package(ovstage)` + GoogleTest → ctest), then **every** shipped C example
(one aggregate configure+build, each run and asserted exit 0) and, with
`--wheel`, the Python tests (pytest in a venv, the wheel force-reinstalled so
reruns always test the given artifact) plus **every** shipped Python example.
GPU-requiring examples run only when a CUDA device is detected
(`OVSTAGE_PUBLIC_TESTS_REQUIRE_GPU=1` turns that skip into a failure so GPU
runners cannot silently lose the coverage); on a GPU box the runner also
installs write-flavors' warp dependency so its GPU-ingest section runs. Each
CMake build starts from a fresh build dir and the runner asserts that configure
resolved `ovstage_DIR` inside the package under test, so a rerun can never
silently validate a cached or downloaded ovstage. An example present on disk
but missing from the runner's registry is a hard error:

```bash
# Everything, against a produced package zip + wheel
python3 run_public_tests.py --package ovstage@X.zip --wheel ovstage-X.whl

# C only, against an already-unpacked package dir
python3 run_public_tests.py --package /path/to/unpacked-ovstage --skip-python
```

Or drive a single suite by hand:

```bash
cd c && cmake -B build -DCMAKE_PREFIX_PATH=/path/to/ovstage && cmake --build build
ctest --test-dir build --output-on-failure
cd ../python && python -m pytest -v      # ovstage on PYTHONPATH + libovstage on the loader path
```
