# Runtime to USD export (C)

This example populates ovstage from ordinary USD, then exports `/World/Props` as
typed USD through the public identifier-based C ABI. Its descriptor derives
prim types from recorded metadata, applies each prim's recorded Physics APIs,
and projects supported schema-declared attributes without rule tables. The
one-shot API creates an empty destination, exports once, saves it, and closes it
without exposing OpenUSD C++ ABI types.

## Build and run

```bash
cmake -S . -B build
cmake --build build --config Release
./build/usd-export
```

On a multi-config generator, run `build/Release/usd-export` instead. Pass a USD
file path as the first argument to replace `destination.usda`.

Expected output:

```text
saved /World/Props as typed USD to destination.usda
report: 7 prims, 4 attributes, 2 applied schemas
```

## Snippet

`main.cpp` marks `typed-hierarchy-c-example`, the public C descriptor-helper
recipe referenced by the USD-export skill. Keep the marker pair intact.
