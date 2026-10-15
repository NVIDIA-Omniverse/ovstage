# ovstage queries (C/C++)

A standalone C++ program that finds prims with **filter queries** — predicates
instead of explicit path lists — through the core **C API**: select prims by
type, path, parent, applied schema, or attribute presence, inspect what a
query matched, and map **scene-graph-instancing** structure. It is the C
sibling of `../../python/queries/main.py`, which mirrors the same workflows.

## At a glance

1. Load a small USD scene (meshes, a cube, an applied schema, two instanced references) at ordinal 1.
2. Write a user attribute (example:count) to two prims so presence filters have a target.
3. Run one filter query per supported predicate — by type, path prefix, parent, children, applied schema, and attribute presence — printing what each matches.
4. Ask a query what it found: match count, reported attributes, and its reusable handle.
5. Map the instancing structure — prototypes to instances and back.

An **ordinal** is a point on the stage's write timeline; every write lands at
one. "Seal it" advances the **write floor** (seals everything up to that
ordinal so readers can trust it).

## What you'll see

```text
populated prims (usd-prim-type):
  /World = Xform
  /World/Anchor = Cube
  /World/Group = Xform
  /World/Group/Left = Mesh
  /World/Group/Right = Mesh
  /World/InstanceA = XformInstance
  /World/InstanceB = XformInstance
  /World/Prototype = Xform
  /World/Prototype/Box = Cube
/World/Anchor exists: True
/World/Anchor has example:count: True
Meshes below /World/Group: 2
Direct children of /World/Group: 2
Prims with points and faceVertexCounts: 2
Matched paths: /World/Group/Left /World/Group/Right
Prims with ShadowAPI: 1
APIs on /World/Group/Right: MaterialBindingAPI MeshSubdivision ShadowAPI
usd-prim-type IN {Mesh} -> 2 matched: /World/Group/Left /World/Group/Right
usd-path PREFIX {/World/Group} -> 3 matched: /World/Group /World/Group/Left /World/Group/Right
usd-parent IN {/World/Group} -> 2 matched: /World/Group/Left /World/Group/Right
usd-children CONTAINS {/World/Group/Left} -> 1 matched: /World/Group
usd-schemas CONTAINS {ShadowAPI} -> 1 matched: /World/Group/Right
HAS example:count -> 2 matched: /World/Anchor /World/Group/Left
query introspection (HAS example:count, scoped to two attributes):
  total_prim_count: 2
  reported attributes: example:count usd-prim-type
  all_handle == query handle: yes
  example:count via all_handle: /World/Anchor=5 /World/Group/Left=3
prototype roots: 1
instance roots of the prototype: /World/InstanceA /World/InstanceB
/World/InstanceA maps back to the same prototype root: yes
/World/OuterA/NestedLeaf
/World/OuterSource/NestedLeaf
same leaf prototype: yes
not an instance root
```

- The scene is `queries.usda`, copied next to the binary at build time;
  `/World/Prototype` is referenced by two instanceable references, which is
  why `InstanceA`/`B` list as `XformInstance`.
- Each `-> N matched:` line is one filter query — no path list goes in; the
  stage finds the prims. Matched paths are sorted before printing, so repeated
  runs print the same bytes.
- `all_handle` is the same query handle echoed back into the result, so code
  holding only the `ovstage_query_result_t` can still read from the matched
  set — the example reads `example:count` through it.
- The final lines map scene-graph instances to their prototypes, then load
  `nested.usda` to show nested instance paths and a non-instance lookup.
  Generated prototype names are discovered through the API.

## Supported predicates

`ovstage_query` takes a filter — a list of predicates that AND together;
values are always strings. The current implementation accepts exactly these
pairings and rejects everything else at enqueue with
`OVSTAGE_ERROR_NOT_SUPPORTED` (the header describes more operators; this table
is what works today):

| Attribute | Operators | Selects |
|---|---|---|
| any attribute | `HAS` | prims where the attribute exists (no value test) |
| `usd-path` | `IN`, `PREFIX` | exact path(s) / subtree (byte-prefix; trailing `/` scopes) |
| `usd-parent` | `IN` | direct children of the named parent(s) |
| `usd-children` | `CONTAINS` | prims having the named child |
| `usd-prim-type` | `IN` | prims of the named type(s) |
| `usd-schemas` | `CONTAINS` | applied-schema membership |

> **`usd-active` is not supported.** It appears in the header contract for
> stability, but a live prim is always active, so the attribute carries no
> information; any read or predicate naming it is rejected with
> `NOT_SUPPORTED`. Subject to removal in a future release.

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
(cd build && ./queries)
```

```powershell
# Windows
cmake -B build
cmake --build build --config Release
# ovstage discovers its bundled plugins relative to where ovstage.dll loads
# from, so keep the package bin/ intact and put it on PATH (do not copy the
# DLL next to the exe):
$env:PATH = "<ovstage-package>\bin;$env:PATH"
cd build\Release; .\queries.exe
```

On Linux the build sets an rpath onto the package `bin/`, so no environment
setup is needed. The program loads `./queries.usda` (copied next to the binary
at build time); run it from a directory containing the scene, or pass a scene
path as `argv[1]`. Keep `nested.usda` in the same directory as that scene. To build every C example at once, configure from the parent
directory (`../CMakeLists.txt` aggregates them).


## Snippets

The `[snippet:name]` markers in `main.cpp` fence regions referenceable from
the ovstage skills under `../../../skills/`; keep them intact when editing.

- `resolve-matched-prims`
- `filter-query`
- `query-prim-exists-c`
- `query-attribute-exists-c`
- `query-type-subtree-c`
- `query-direct-children-c`
- `query-required-attributes-c`
- `query-matched-paths-c`
- `query-applied-schema-c`
- `query-list-schemas-c`
- `filter-predicates`
- `query-introspection`
- `instancing-queries-c`
- `instancing-timing-c`
- `instancing-nested-c`
- `instancing-not-found-c`

## Notes

- Population applies schemas of its own (e.g. `MaterialBindingAPI` lands on
  every gprim), so filter on a schema that is selective in your scene.
- A filter can match prims inside prototype subtrees, whose
  `/__Prototype_<id>` paths change between runs; this example's predicates are
  chosen so none match there.
- Path lists returned by the instancing calls carry a fresh reference the
  caller must release; read-group prim lists are borrows and must not be.
- The examples fail fast: any unexpected API failure prints and exits (helpers
  in `../common/ovstage_example_utils.h`). A real application would propagate
  errors instead.


## Checking existence

The following checks match `/World/Anchor` exactly, then test whether it has
`example:count`. A missing prim or attribute gives a count of zero.

These snippets call the C API from C++. `waitOp()` waits and checks operation
errors; `check()` checks synchronous status codes. Both helpers are in `main.cpp`.

### Prim exists

```cpp
// Match the exact path, not a prefix that could include other prims.
const ovx_string_t path = literal_to_ovx_string("/World/Anchor");
ovstage_predicate_t predicate{};
predicate.attribute.string = literal_to_ovx_string("usd-path");
predicate.op = OVSTAGE_FILTER_OP_IN;
predicate.values = &path;
predicate.value_count = 1;
ovstage_filter_t filter{};
filter.predicates = &predicate;
filter.count = 1;

// Wait for the query before inspecting its result.
ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query prim existence");
ovstage_query_result_t result{};
check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
      "fetch_query_result");
const bool primExists = result.total_prim_count != 0;
std::printf("/World/Anchor exists: %s\n", primExists ? "True" : "False");
// /World/Anchor exists: True

// The result and query handle have separate lifetimes; release both.
check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
waitOp(stage, ovstage_release_query(stage, query), "release_query");
```

### Attribute exists on that prim

```cpp
// Both conditions must match: this path AND presence of this attribute.
const ovx_string_t path = literal_to_ovx_string("/World/Anchor");
ovstage_predicate_t predicates[2]{};
predicates[0].attribute.string = literal_to_ovx_string("usd-path");
predicates[0].op = OVSTAGE_FILTER_OP_IN;
predicates[0].values = &path;
predicates[0].value_count = 1;
predicates[1].attribute.string = literal_to_ovx_string("example:count");
predicates[1].op = OVSTAGE_FILTER_OP_HAS; // No comparison value is needed.
ovstage_filter_t filter{};
filter.predicates = predicates;
filter.count = 2;

ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query attribute existence");
ovstage_query_result_t result{};
check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
      "fetch_query_result");
const bool attributeExists = result.total_prim_count != 0;
std::printf("/World/Anchor has example:count: %s\n", attributeExists ? "True" : "False");
// /World/Anchor has example:count: True

// Release the result and query even when no prim matched.
check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
waitOp(stage, ovstage_release_query(stage, query), "release_query");
```

## Common query patterns

These examples use the same stage. The first three report match counts; the
last reads `usd-path` and resolves the identifiers to path strings.
`dict` is the stage dictionary; `resolvePath()` converts a prim-path identifier
to a string using its component tokens. Other helpers are described above.

### Meshes below /World/Group

```cpp
// Match Mesh prims below /World/Group; the trailing slash excludes the root.
const ovx_string_t type = literal_to_ovx_string("Mesh");
const ovx_string_t prefix = literal_to_ovx_string("/World/Group/");
ovstage_predicate_t predicates[2]{};
predicates[0].attribute.string = literal_to_ovx_string("usd-prim-type");
predicates[0].op = OVSTAGE_FILTER_OP_IN;
predicates[0].values = &type;
predicates[0].value_count = 1;
predicates[1].attribute.string = literal_to_ovx_string("usd-path");
predicates[1].op = OVSTAGE_FILTER_OP_PREFIX;
predicates[1].values = &prefix;
predicates[1].value_count = 1;
ovstage_filter_t filter{};
filter.predicates = predicates;
filter.count = 2;

// Wait, inspect the match count, and release the result and query.
ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query");
ovstage_query_result_t result{};
check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
      "fetch_query_result");
std::printf("Meshes below /World/Group: %llu\n", static_cast<unsigned long long>(result.total_prim_count));
// Meshes below /World/Group: 2
check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
waitOp(stage, ovstage_release_query(stage, query), "release_query");
```

### Direct children of /World/Group

```cpp
// Match the parent exactly; grandchildren have a different parent.
const ovx_string_t parent = literal_to_ovx_string("/World/Group");
ovstage_predicate_t predicate{};
predicate.attribute.string = literal_to_ovx_string("usd-parent");
predicate.op = OVSTAGE_FILTER_OP_IN;
predicate.values = &parent;
predicate.value_count = 1;
ovstage_filter_t filter{};
filter.predicates = &predicate;
filter.count = 1;

// Wait, inspect the match count, and release the result and query.
ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query");
ovstage_query_result_t result{};
check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
      "fetch_query_result");
std::printf("Direct children of /World/Group: %llu\n", static_cast<unsigned long long>(result.total_prim_count));
// Direct children of /World/Group: 2
check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
waitOp(stage, ovstage_release_query(stage, query), "release_query");
```

### Prims with points and faceVertexCounts

```cpp
// Separate HAS predicates require both attributes, regardless of their values.
ovstage_predicate_t predicates[2]{};
predicates[0].attribute.string = literal_to_ovx_string("points");
predicates[0].op = OVSTAGE_FILTER_OP_HAS;
predicates[1].attribute.string = literal_to_ovx_string("faceVertexCounts");
predicates[1].op = OVSTAGE_FILTER_OP_HAS;
ovstage_filter_t filter{};
filter.predicates = predicates;
filter.count = 2;

// Wait, inspect the match count, and release the result and query.
ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query");
ovstage_query_result_t result{};
check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
      "fetch_query_result");
std::printf("Prims with points and faceVertexCounts: %llu\n", static_cast<unsigned long long>(result.total_prim_count));
// Prims with points and faceVertexCounts: 2
check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
waitOp(stage, ovstage_release_query(stage, query), "release_query");
```

### Matched paths

```cpp
// Query the direct children of /World/Group.
const ovx_string_t parent = literal_to_ovx_string("/World/Group");
ovstage_predicate_t predicate{};
predicate.attribute.string = literal_to_ovx_string("usd-parent");
predicate.op = OVSTAGE_FILTER_OP_IN;
predicate.values = &parent;
predicate.value_count = 1;
ovstage_filter_t filter{};
filter.predicates = &predicate;
filter.count = 1;
ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query paths");

// usd-path contains prim-path identifiers resolved through the stage dictionary.
const ovx_string_t name = literal_to_ovx_string("usd-path");
ovx_token_t pathAttribute = OVX_INVALID_TOKEN;
checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &pathAttribute),
         "intern usd-path");
ovstage_ordinal_range_t range{};
range.end_ordinal = 2; // No start ordinal: read current paths.
ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
waitOp(stage, ovstage_read_attributes(stage, query, &pathAttribute, 1, range, &read), "read paths");

std::vector<std::string> matchedPaths;
for (;;)
{
    ovstage_read_group_t group{};
    const ovstage_api_status_t fetched =
        ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
    if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
        break;
    check(stage, fetched, "fetch_read_next");

    const DLTensor& tensor = group.data.tensors[0];
    const auto* pathIds = reinterpret_cast<const uint64_t*>(
        static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
    for (uint32_t i = 0; i < group.prims.count; ++i)
    {
        const uint32_t row = group.data.index_map ? group.data.index_map[i] : i;
        matchedPaths.push_back(resolvePath(dict, pathIds[row]));
    }
    check(stage, ovstage_release_group(stage, &group), "release_group");
}
waitOp(stage, ovstage_release_read(stage, read), "release_read");
waitOp(stage, ovstage_release_query(stage, query), "release_query");

std::sort(matchedPaths.begin(), matchedPaths.end());
std::printf("Matched paths:");
for (const std::string& path : matchedPaths)
    std::printf(" %s", path.c_str());
std::printf("\n");
// Matched paths: /World/Group/Left /World/Group/Right
```

### Applied API schemas

Select prims with `ShadowAPI`, then list the schemas on `/World/Group/Right`.

```cpp
// Select prims carrying this applied API schema.
const ovx_string_t schema = literal_to_ovx_string("ShadowAPI");
ovstage_predicate_t predicate{};
predicate.attribute.string = literal_to_ovx_string("usd-schemas");
predicate.op = OVSTAGE_FILTER_OP_CONTAINS;
predicate.values = &schema;
predicate.value_count = 1;
ovstage_filter_t filter{};
filter.predicates = &predicate;
filter.count = 1;

// Wait for the query before inspecting its result.
ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query applied schema");
ovstage_query_result_t result{};
check(stage, ovstage_fetch_query_result(stage, query, OVSTAGE_TIMEOUT_INFINITE, &result),
      "fetch_query_result");
std::printf("Prims with ShadowAPI: %llu\n",
            static_cast<unsigned long long>(result.total_prim_count));
            // Prims with ShadowAPI: 1

// The result and query handle have separate lifetimes; release both.
check(stage, ovstage_release_query_result(stage, &result), "release_query_result");
waitOp(stage, ovstage_release_query(stage, query), "release_query");
```

```cpp
// Select the prim whose applied schemas we want to inspect.
const ovx_string_t primPath = literal_to_ovx_string("/World/Group/Right");
ovstage_predicate_t predicate{};
predicate.attribute.string = literal_to_ovx_string("usd-path");
predicate.op = OVSTAGE_FILTER_OP_IN;
predicate.values = &primPath;
predicate.value_count = 1;
ovstage_filter_t filter{};
filter.predicates = &predicate;
filter.count = 1;
ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
waitOp(stage, ovstage_query(stage, &filter, nullptr, 0, &query), "query prim schemas");

// usd-schemas contains an array of token identifiers for each prim.
const ovx_string_t name = literal_to_ovx_string("usd-schemas");
ovx_token_t schemaAttribute = OVX_INVALID_TOKEN;
checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &schemaAttribute),
         "intern usd-schemas");
ovstage_ordinal_range_t range{};
range.end_ordinal = 2; // No start ordinal: read current schemas.
ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
waitOp(stage, ovstage_read_attributes(stage, query, &schemaAttribute, 1, range, &read), "read schemas");

std::vector<std::string> schemaNames;
for (;;)
{
    ovstage_read_group_t group{};
    const ovstage_api_status_t fetched =
        ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
    if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
        break;
    check(stage, fetched, "fetch_read_next");

    // Array attributes return a separate tensor for each prim.
    for (uint32_t i = 0; i < group.data.tensor_count; ++i)
    {
        const DLTensor& tensor = group.data.tensors[i];
        const auto* tokens = reinterpret_cast<const ovx_token_t*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        for (int64_t j = 0; j < tensor.shape[0]; ++j)
            schemaNames.push_back(tokenName(dict, tokens[j]));
    }
    check(stage, ovstage_release_group(stage, &group), "release_group");
}
waitOp(stage, ovstage_release_read(stage, read), "release_read");
waitOp(stage, ovstage_release_query(stage, query), "release_query");

std::sort(schemaNames.begin(), schemaNames.end());
std::printf("APIs on /World/Group/Right:");
for (const std::string& schemaName : schemaNames)
    std::printf(" %s", schemaName.c_str());
std::printf("\n");
// APIs on /World/Group/Right: MaterialBindingAPI MeshSubdivision ShadowAPI
```
