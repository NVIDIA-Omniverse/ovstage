# ovstage queries (Python)

A headless Python program that finds prims with **filter queries** —
predicates instead of explicit path lists — through the ovstage Python
bindings: select prims by type, path, parent, applied schema, or attribute
presence, inspect what a query matched, and map scene-graph-instancing
structure. It is the Python sibling of `../../c/queries/main.cpp`.

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
instance roots: /World/InstanceA /World/InstanceB
/World/OuterA/NestedLeaf
/World/OuterSource/NestedLeaf
True
not an instance root
```

- The scene is `queries.usda`, shipped next to this file; `/World/Prototype`
  is referenced by two instanceable references, which is why `InstanceA`/`B`
  list as `XformInstance`.
- Each `-> N matched:` line is one filter query — no path list goes in; the
  stage finds the prims. Matched paths are sorted before printing, so repeated
  runs print the same bytes.
- `all_handle` is the same query handle echoed back into the result, so code
  holding only the `QueryResult` can still read from the matched set — the
  example reads `example:count` through it.
- The final lines map scene-graph instances to their prototypes, then load
  `nested.usda` to show nested instance paths and a non-instance lookup.
  Generated prototype names are discovered through the API.

## Supported predicates

`Stage.query` takes a `Filter` — a list of `Predicate`s that AND together;
values are always strings. The current implementation accepts exactly these
pairings and rejects everything else at enqueue with `NOT_SUPPORTED` (the C
header describes more operators; this table is what works today):

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

The example is a [uv](https://docs.astral.sh/uv/) project pinning the released
`ovstage` wheel (see `pyproject.toml`); the wheel bundles the `ovstage`
shared library at `<package>/bin`, which the bindings load automatically:

```bash
uv run main.py
```

> **Pre-release:** if `uv` cannot resolve the pinned `ovstage` wheel, no package
> index available to you carries it yet — check the repository releases page for
> current availability.


## Snippets

The `[snippet:name]` markers in `main.py` fence regions referenceable from the
ovstage skills under `../../../skills/`; keep them intact when editing.

- `setup`
- `query-filter-imports`
- `instancing-imports`
- `query-prim-exists`
- `query-attribute-exists`
- `query-type-subtree`
- `query-direct-children`
- `query-required-attributes`
- `query-matched-paths`
- `query-applied-schema`
- `query-list-schemas`
- `filter-predicates`
- `query-introspection`
- `instancing-queries`
- `instancing-timing`
- `instancing-nested`
- `instancing-not-found`
- `filter-query`
- `resolve-matched-prims`

## Notes

- Population applies schemas of its own (e.g. `MaterialBindingAPI` lands on
  every gprim), so filter on a schema that is selective in your scene.
- A filter can match prims inside prototype subtrees, whose
  `/__Prototype_<id>` paths change between runs; this example's predicates are
  chosen so none match there.
- Read-group prim lists (`group.prim_list`) are borrows owned by the read —
  resolve them, never `destroy_path_list` them.
- The example fails fast: an unexpected `OvstageError` propagates to the
  top-level handler, which prints one line and exits nonzero. A real
  application would handle errors in place.


## Checking existence

The following checks match `/World/Anchor` exactly, then test whether it has
`example:count`. A missing prim or attribute gives a count of zero.

The imports are `Filter`, `FilterOp`, and `Predicate` from `ovstage`.

### Prim exists

```python
# Match the exact path, not a prefix that could include other prims.
prim_filter = Filter([Predicate("usd-path", FilterOp.IN, ["/World/Anchor"])])
with stage.query(filter=prim_filter) as query:
    query.wait()
    prim_exists = query.result().total_prim_count != 0
print("/World/Anchor exists:", prim_exists)
# /World/Anchor exists: True
```

### Attribute exists on that prim

```python
# Both conditions must match: this path AND presence of this attribute.
attribute_filter = Filter([
    Predicate("usd-path", FilterOp.IN, ["/World/Anchor"]),
    Predicate("example:count", FilterOp.HAS),
])
with stage.query(filter=attribute_filter) as query:
    query.wait()
    attribute_exists = query.result().total_prim_count != 0
print("/World/Anchor has example:count:", attribute_exists)
# /World/Anchor has example:count: True
```

## Common query patterns

These examples use the same stage. The first three report match counts; the
last reads `usd-path` and resolves the identifiers to path strings.
`paths` is the stage's `PathDictionary`; import `OrdinalRange` from `ovstage`.

### Meshes below /World/Group

```python
from ovstage import Filter, FilterOp, Predicate

# Separate predicates must all match.
mesh_filter = Filter([
    Predicate("usd-prim-type", FilterOp.IN, ["Mesh"]),
    Predicate("usd-path", FilterOp.PREFIX, ["/World/Group/"]),
])
with stage.query(filter=mesh_filter) as query:
    query.wait()
    mesh_count = query.result().total_prim_count
print("Meshes below /World/Group:", mesh_count)
# Meshes below /World/Group: 2
```

### Direct children of /World/Group

```python
# Match the parent exactly; grandchildren have a different parent.
children_filter = Filter([
    Predicate("usd-parent", FilterOp.IN, ["/World/Group"]),
])
with stage.query(filter=children_filter) as query:
    query.wait()
    child_count = query.result().total_prim_count
print("Direct children of /World/Group:", child_count)
# Direct children of /World/Group: 2
```

### Prims with points and faceVertexCounts

```python
# Require both attributes, without comparing their values.
geometry_filter = Filter([
    Predicate("points", FilterOp.HAS),
    Predicate("faceVertexCounts", FilterOp.HAS),
])
with stage.query(filter=geometry_filter) as query:
    query.wait()
    geometry_count = query.result().total_prim_count
print("Prims with points and faceVertexCounts:", geometry_count)
# Prims with points and faceVertexCounts: 2
```

### Matched paths

```python
# Query the direct children, then read their current path identifiers.
children_filter = Filter([
    Predicate("usd-parent", FilterOp.IN, ["/World/Group"]),
])
path_attribute = paths.intern_token("usd-path")
matched_paths = []
with stage.query(filter=children_filter) as query:
    query.wait()
    with stage.read_attributes(query, [path_attribute], OrdinalRange.latest(2)) as read:
        read.wait()
        for group in read.groups():
            with group:  # Release every group after resolving its identifiers.
                path_ids = group.array(0)
                for i in range(group.prim_count):
                    row = group.data_row_index(i) if group.has_data_index_map else i
                    matched_paths.append(paths.path_to_string(int(path_ids[row])))
print("Matched paths:", " ".join(sorted(matched_paths)))
# Matched paths: /World/Group/Left /World/Group/Right
```

### Applied API schemas

Select prims with `ShadowAPI`, then list the schemas on `/World/Group/Right`.

```python
# Select prims carrying this applied API schema.
schema_filter = Filter([Predicate("usd-schemas", FilterOp.CONTAINS, ["ShadowAPI"])])
with stage.query(filter=schema_filter) as query:
    query.wait()
    schema_count = query.result().total_prim_count
print("Prims with ShadowAPI:", schema_count)
# Prims with ShadowAPI: 1
```

```python
# Select one prim, then read its array of applied-schema token identifiers.
prim_filter = Filter([Predicate("usd-path", FilterOp.IN, ["/World/Group/Right"])])
schema_attribute = paths.intern_token("usd-schemas")
schema_names = []
with stage.query(filter=prim_filter) as query:
    query.wait()
    with stage.read_attributes(query, [schema_attribute], OrdinalRange.latest(2)) as read:
        read.wait()
        for group in read.groups():
            with group:
                # Array attributes return a separate tensor for each prim.
                for i in range(group.prim_count):
                    for token in group.array(i):
                        schema_names.append(paths.token_to_string(int(token)))
print("APIs on /World/Group/Right:", " ".join(sorted(schema_names)))
# APIs on /World/Group/Right: MaterialBindingAPI MeshSubdivision ShadowAPI
```
