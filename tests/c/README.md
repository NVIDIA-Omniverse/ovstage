# ovstage public C tests

CPU-only C API tests that double as the source of truth for the C snippets in the
ovstage skills. Built with CMake + GoogleTest and run via `ctest`, against the
*produced* ovstage C SDK (not the build tree).

- `CMakeLists.txt` — CMake + GoogleTest, `find_package(ovstage)` (point
  `CMAKE_PREFIX_PATH` at an unpacked package).
- `test_minimal.cpp` — asserts the write → advance-write-floor → read round-trip, the
  type-aware fixed-size raw-read layout, and the zero-timeout poll → release flow
  (`write-read-roundtrip-c`, `canonical-fixed-shapes-c`, `nonblocking-poll-c`).
- `test_population.cpp` — asserts USD population → query-back by usd-path, plus
  USD reference add/remove, reset, and missing-file failure (`populate-and-query-c`,
  `usd-reference-c`, `reset-usd-c`, `open-missing-file-c`).
- `test_logging.cpp` — asserts the log-callback lifecycle + channel-filter suppression
  (`log-callback-filter-c`).
- `test_support_api.cpp` — asserts `get_version`, `get_error_string`, process-config
  readback ownership, and unknown-key rejection (`version-and-error-c`).
- `test_clone.cpp` — asserts clone copies attribute values and reports the prims it
  created (`clone-and-verify-c`, `clone-result-c`).
- `test_queries.cpp` — asserts `usd-path` IN / HAS on client-written prims, plus
  the population-backed predicate matrix and query introspection
  (`query-by-usd-path-c`, `query-has-attribute-c`, `query-predicate-matrix-c`,
  `query-result-introspection-c`).
- `test_change_feed.cpp` — asserts what a per-tick consumer sees from ordinal-range
  reads on a retained whole-stage query: a target-only tick sealed ALL or INCLUDE
  yields an empty structural feed, an INCLUDE seal moves only the written column's
  floor, and a retained filter query keeps its membership while a fresh one sees a
  new prim (`empty-change-range-c`, `include-seal-change-range-c`,
  `frozen-vs-fresh-query-c`); also pins that an INCLUDE seal leaves a same-tick
  structural change unreadable until a covering seal lands.
- `test_error_handling.cpp` — asserts clone to existing target fails
  (`clone-target-exists-error-c`).
- `test_attribute_shapes.cpp` — asserts scalar / fixed-lane / ragged column shapes
  round-trip (`attribute-shapes-fixed-c`, `attribute-shapes-ragged-c`).
- `test_attributes.cpp` — asserts POINT / COLOR / MATRIX / TOKEN_ID semantic
  metadata round-trip (`semantic-roles-c`).
- `test_map_attribute.cpp` — asserts CPU map/unmap writes existing and new
  attributes (`map-unmap-cpu-c`).
- `test_write_modes.cpp` — asserts UPSERT vs INSERT admission
  (`upsert-vs-insert-c`).
- `test_sparse_writes.cpp` — asserts sparse `index_map` + `mask` writes
  (`sparse-index-map-and-mask-c`).
- `test_delete.cpp` — asserts attribute delete and whole-prim tombstones
  (`delete-attribute-then-prim-c`).

Build + run directly, or via the cross-platform `../run_public_tests.py`:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/unpacked-ovstage && cmake --build build
ctest --test-dir build --output-on-failure     # keep <package>/bin on the loader path
```

See `../AGENTS.md` for the tested-doc contract and snippet rules.

