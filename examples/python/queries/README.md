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
prototype roots: 1 (all prefixed /__Prototype_: yes)
instance roots of the prototype: /World/InstanceA /World/InstanceB
/World/InstanceA maps back to the same prototype root: yes
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
- The last three lines map instancing: prototype roots, one prototype's
  instance roots, and one instance back to its prototype. The synthesized
  prototype name is checked by prefix and never printed.

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

- `setup` — imports (`numpy`, `ovstage`, `Filter`/`Predicate`, `instancing`, `population`)
- `filter-predicates` — one query per supported predicate
- `query-introspection` — `Query.result()`: reported attributes,
  `total_prim_count`, and reading through `all_handle`
- `instancing-queries` — prototype roots to instance roots using Python strings
- `filter-query` — run one filter query end to end
- `resolve-matched-prims` — resolve a read group's prim handles back to path
  strings

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

