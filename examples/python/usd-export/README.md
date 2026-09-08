# Runtime to USD export (Python)

This example populates ovstage from ordinary USD containing a Sphere, a Cube,
and different Physics APIs. It exports only `/World/Props` as typed USD with
`population.export_typed_hierarchy_to_usd_file`, preserving each prim's type,
recorded applied schemas, and supported schema-declared attributes without rule
tables. The one-shot API creates an empty destination, exports once, saves it,
and closes it; no `pxr` Python binding is required.

## Run

```bash
uv run main.py
```

Expected output (the absolute path is intentionally omitted here):

```text
saved /World/Props as typed USD to destination.usda
report: 7 prims, 4 attributes, 2 applied schemas
```

## Snippet

`main.py` marks `typed-hierarchy-python-example`, the public Python helper
recipe referenced by the USD-export skill. Keep the marker pair intact.
