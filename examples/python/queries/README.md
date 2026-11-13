# ovstage queries (Python)

Discover prims with filters, inspect the matched attributes, and map scene-graph
instances to their prototypes. [queries.usda](queries.usda) supplies two meshes,
a Cube, an applied schema, and two instances; [nested.usda](nested.usda) supplies
the separate nested-instancing case.

The [C/C++ example](../../c/queries/README.md) covers the same workflow.

## At a glance

A filter selects prims by conditions rather than by a supplied path list. Its
predicates are combined with AND; a presence test checks that an attribute exists,
not that its value equals something.

Pseudocode (workflow only):

```text
populate the scene at ordinal 1
write example:count on Anchor and Left at ordinal 2
finish writes through 2
query by path, type, attribute presence, and schema
inspect the match count and read values from the matched set
map instance roots to their discovered prototype
load the nested scene and handle a non-instance lookup
```

### Selecting meshes below a path

The two predicates below require both the `Mesh` type and a path below
`/World/Group/`. The trailing slash excludes the group itself. The surrounding
source submits this filter, waits, and reads the match count.

Excerpt from [`query-type-subtree`](main.py) in the runnable example;
this is the filter construction, not a standalone program.

```python
mesh_filter = Filter([
    Predicate("usd-prim-type", FilterOp.IN, ["Mesh"]),
    Predicate("usd-path", FilterOp.PREFIX, ["/World/Group/"]),
])
```

## What you'll see

These are selected results, not the full console transcript:

| Query | Result |
|---|---|
| Anchor exists / has `example:count` | Both true. |
| Meshes below `/World/Group/` | Two: `Left` and `Right`. |
| Direct children of `/World/Group` | Two; grandchildren would not match. |
| Has both `points` and `faceVertexCounts` | Two prims. |
| Meshes without `example:count` | One: `Right`. |
| Has applied `ShadowAPI` | One: `Right`. |
| Introspection of `HAS example:count` | Two prims; reads Anchor = 5 and Left = 3. |
| Instances of the discovered prototype | `/World/InstanceA` and `/World/InstanceB`. |

The nested case finds `/World/OuterA/NestedLeaf` and
`/World/OuterSource/NestedLeaf` sharing a prototype. Looking up the ordinary
`/World/LeafSource` prim prints `not an instance root` as an expected result.
Generated prototype names are discovered at runtime, not hard-coded.

## How to read the code

Start in `main()` in [main.py](main.py). The names below are searchable
`[snippet:name]` markers in that file; each row explains why that section is there.

| Source markers | What to learn |
|---|---|
| `query-prim-exists`, `query-attribute-exists` | Start with an exact path, then add HAS to require an attribute on that same prim. |
| `query-type-subtree`, `query-direct-children` | Compare type plus path prefix with an exact parent predicate. A trailing slash on the prefix scopes descendants. |
| `query-required-attributes`, `query-excluded-attribute` | Combine presence and absence tests without reading values. |
| `query-matched-paths`, `query-applied-schema`, `query-list-schemas` | Resolve returned identifiers to readable paths or schema names; schema membership and listing schemas are separate operations. |
| `filter-predicates`, `filter-query` | Compare the supported predicate examples. The shared helper submits a query, waits, reads its matches, and sorts paths for display. |
| `query-introspection`, `resolve-matched-prims` | Inspect count and reported attributes, then read through the same query handle. Resolve the group's prim indices and each column's data indices instead of assuming query order. |
| `instancing-queries`, `instancing-timing`, `instancing-nested`, `instancing-not-found` | After population completes and its floor advances, map prototypes and instances and distinguish NOT_FOUND from unexpected errors. |

## Build and run

Use a package with population and instancing-query support. See the [shared setup](../../README.md) for package prerequisites.
Run these commands from this example's directory.

```bash
uv run main.py
```


## Notes

- `usd-path PREFIX` is a byte-prefix test. `/World/Group/` selects descendants;
  the separate `filter-predicates` demonstration uses `/World/Group` and also matches
  the group itself. See the query guide for supported operator/attribute pairings.
- `all_handle` echoes the query handle; it does not create a new owned query.
  Keep that query alive while reading through the handle.
- Read-group prim lists are borrowed. The high-level instancing helpers return
  Python path strings; they do not expose those borrowed read-group lists.
- Population can add schemas besides those authored in the fixture. Use a schema
  selective in your scene rather than assuming every populated schema is selective.

## Further reading

- [Filter queries and supported predicates](../../../docs/scene/queries.rst)
- [Scene-graph instancing](../../../docs/scene/instancing.rst)
- [Path and token identifiers](../../../docs/scene/path_dictionary.rst)
