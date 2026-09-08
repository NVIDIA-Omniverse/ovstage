# ovstage public Python tests

CPU-only Python API tests that double as the source of truth for the Python
snippets in the ovstage skills. Run via `pytest` against the *produced* ovstage
wheel (not the build tree).

- `pyproject.toml` — `pytest` + `numpy`; wheel pinning is deferred until a public
  index is available.
- `conftest.py` — provides the `stage` fixture; skips individual tests if
  `ovstage` can't be imported/loaded, and fails an all-skipped run.
- `test_minimal.py` — asserts the write → advance-write-floor → read round-trip
  (snippet `write-read-roundtrip`).
- `test_population.py` — asserts USD population → query-back by usd-path, plus
  USD reference add/remove, reset, and missing-file failure (`populate-and-query`,
  `usd-reference`, `reset-usd`, `open-missing-file`).
- `test_instancing.py` — asserts prototype-root enumeration and
  prototype/instance-root mapping through the public wheel.
- `test_logging.py` — asserts the log-callback lifecycle + channel-filter suppression
  (`log-callback-filter`).
- `test_support_api.py` — asserts `library_version` + the DLPack round-trip
  (`dlpack-round-trip`).
- `test_config.py` — asserts process configuration selects the runtime-default
  hierarchy model (`configure-transform-updates`).
- `test_clone.py` — asserts clone copies attribute values (`clone-and-verify`).
- `test_queries.py` — asserts `usd-path` IN / HAS on client-written prims, plus
  the population-backed predicate matrix and query introspection
  (`query-by-usd-path`, `query-has-attribute`, `query-predicate-matrix`,
  `query-result-introspection`).
- `test_error_handling.py` — asserts clone to existing target fails
  (`clone-target-exists-error`).
- `test_attribute_shapes.py` — asserts scalar / fixed-lane / ragged column shapes
  round-trip (`attribute-shapes-fixed`, `attribute-shapes-ragged`).
- `test_attributes.py` — asserts POINT / COLOR / MATRIX / TOKEN_ID semantic
  metadata round-trip (`semantic-roles`).
- `test_map_attribute.py` — asserts CPU map/unmap writes existing and new
  attributes (`map-unmap-cpu`).
- `test_write_modes.py` — asserts UPSERT vs INSERT admission
  (`upsert-vs-insert`).
- `test_sparse_writes.py` — asserts sparse `index_map` + `mask` writes
  (`sparse-index-map-and-mask`).
- `test_delete.py` — asserts attribute delete and whole-prim tombstones
  (`delete-attribute-then-prim`).
- `test_asset_paths.py` — asserts the `ASSET_PATH_ID` contract: an
  `(authored, resolved)` token pair round-trips for native and USD-populated
  columns, an unresolved asset carries token id 0, and a byte-row payload is
  rejected.
- `test_read_groups.py` — asserts `ReadGroup` release ownership: `with` releases the pinned
  storage (including when the body raises), double release is rejected, a dropped group is
  reclaimed with a `ResourceWarning`, the release still happens when `ResourceWarning` is
  escalated to an error, and a dropped group no longer blocks later writes
  (`read-group-context-manager`).
- `test_path_lists.py` — asserts `PathList` reference ownership: the handle is still an
  `int`, `with`/`release` drop the create reference, double release is rejected, an added
  reference is tracked, and a dropped list is reclaimed with a `ResourceWarning`
  (`path-list-context-manager`, `path-list-loop-reuse`).

Run via the cross-platform `../run_public_tests.py --wheel ...`, or directly with
`ovstage` on `PYTHONPATH` and `libovstage` on the loader path:

```bash
python -m pytest -v
```

See `../AGENTS.md` for the tested-doc contract and snippet rules.

