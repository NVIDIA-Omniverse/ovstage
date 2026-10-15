# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.
#
# Find prims with filter queries instead of explicit path lists, inspect what a
# query found, and map scene-graph-instancing structure.
#
# Run + expected output: see README.md. Snippet markers are referenced by the
# skills under ../../../skills/ -- keep them intact.

"""ovstage queries: filter predicates, result introspection, and instancing."""

# [snippet:setup]
import pathlib

import numpy as np

import ovstage
# [snippet:query-filter-imports]
from ovstage import Filter, FilterOp, Predicate
# [/snippet:query-filter-imports]
from ovstage import OrdinalRange
# [snippet:instancing-imports]
from ovstage import ErrorCode, OvstageError, PopulationDomain, instancing, population
# [/snippet:instancing-imports]
# [/snippet:setup]

# A small scene shipped next to this file: /World/Group with two meshes (the
# right one carries an applied ShadowAPI schema), a Cube, and a prototype
# subtree referenced by two instanceable references.
SCENE = pathlib.Path(__file__).resolve().parent / "queries.usda"
SCENE_PRIMS = [
    "/World",
    "/World/Group",
    "/World/Group/Left",
    "/World/Group/Right",
    "/World/Anchor",
    "/World/Prototype",
    "/World/Prototype/Box",
    "/World/InstanceA",
    "/World/InstanceB",
]
COUNT_TARGETS = ["/World/Anchor", "/World/Group/Left"]  # receive example:count (for HAS)
END_ORDINAL = 2  # populate lands at ordinal 1, the example:count write at 2


def main() -> int:
    if not population.available():
        print("ovstage was built without the population bridge.")
        return 1
    if not instancing.available():
        print("ovstage was built without the instancing query API.")
        return 1

    # A Stage owns the ovstage instance; its path dictionary is instance-owned.
    with ovstage.Stage("example.queries") as stage, ovstage.PathDictionary(stage) as paths:
        prim_type = paths.intern_token("usd-prim-type")
        count_tok = paths.intern_token("example:count")

        # ---- 1. populate the scene and list the populated prims ----
        # Populate everything (domain ALL) at ordinal 1 and seal it; the two
        # instanceable references land as XformInstance prims.
        population.open_usd(stage, str(SCENE), ordinal=1, domains=PopulationDomain.ALL)
        stage.advance_write_floor(ordinal=1).wait()

        scene_list = paths.create_path_list_from_strings(SCENE_PRIMS)
        with stage.query_from_path_list(scene_list) as scene_query:
            scene_rows = list(_rows(stage, paths, scene_query, prim_type, 1))

        # Example plumbing: the usd-prim-type values are tokens -- resolve each
        # to its name and print "path = Type", sorted for deterministic output.
        scene_lines = sorted(f"{path} = {paths.token_to_string(int(token))}"
                             for path, token in scene_rows)
        print("populated prims (usd-prim-type):")
        for line in scene_lines:
            print(f"  {line}")

        # ---- 2. write example:count (filtered on in sections 3 and 4) ----
        # No query here yet: the HAS filters in sections 3 and 4 match the prims this write touches.
        target_list = paths.create_path_list_from_strings(COUNT_TARGETS)
        with stage.query_from_path_list(target_list) as target_query:
            stage.write_attribute(target_query, count_tok, ordinal=2,
                                  tensors=np.array([5, 3], dtype=np.int32), is_array=False).wait()
        stage.advance_write_floor(ordinal=2).wait()

        # [snippet:query-prim-exists]
        # Match the exact path, not a prefix that could include other prims.
        prim_filter = Filter([Predicate("usd-path", FilterOp.IN, ["/World/Anchor"])])
        with stage.query(filter=prim_filter) as query:
            query.wait()
            prim_exists = query.result().total_prim_count != 0
        print("/World/Anchor exists:", prim_exists)
        # /World/Anchor exists: True
        # [/snippet:query-prim-exists]

        # [snippet:query-attribute-exists]
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
        # [/snippet:query-attribute-exists]

        # [snippet:query-type-subtree]
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
        # [/snippet:query-type-subtree]

        # [snippet:query-direct-children]
        # Match the parent exactly; grandchildren have a different parent.
        children_filter = Filter([
            Predicate("usd-parent", FilterOp.IN, ["/World/Group"]),
        ])
        with stage.query(filter=children_filter) as query:
            query.wait()
            child_count = query.result().total_prim_count
        print("Direct children of /World/Group:", child_count)
        # Direct children of /World/Group: 2
        # [/snippet:query-direct-children]

        # [snippet:query-required-attributes]
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
        # [/snippet:query-required-attributes]

        # [snippet:query-matched-paths]
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
        # [/snippet:query-matched-paths]

        # [snippet:query-applied-schema]
        # Select prims carrying this applied API schema.
        schema_filter = Filter([Predicate("usd-schemas", FilterOp.CONTAINS, ["ShadowAPI"])])
        with stage.query(filter=schema_filter) as query:
            query.wait()
            schema_count = query.result().total_prim_count
        print("Prims with ShadowAPI:", schema_count)
        # Prims with ShadowAPI: 1
        # [/snippet:query-applied-schema]

        # [snippet:query-list-schemas]
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
        # [/snippet:query-list-schemas]

        # ---- 3. one filter query per supported predicate ----
        # [snippet:filter-predicates]
        # The support matrix is narrow: HAS works on any attribute; the value
        # operators only pair with the reserved metadata built-ins (usd-path
        # IN/PREFIX, usd-parent IN, usd-children CONTAINS, usd-prim-type IN,
        # usd-schemas CONTAINS). Anything else is rejected at enqueue -- the
        # bindings describe more operators than the current implementation
        # accepts. Values are always strings; predicates in one Filter AND
        # together.
        _run_filter_query(stage, paths, Predicate("usd-prim-type", FilterOp.IN, ["Mesh"]),
                          prim_type, END_ORDINAL, "usd-prim-type IN {Mesh}")
        _run_filter_query(stage, paths, Predicate("usd-path", FilterOp.PREFIX, ["/World/Group"]),
                          prim_type, END_ORDINAL, "usd-path PREFIX {/World/Group}")  # subtree
        _run_filter_query(stage, paths, Predicate("usd-parent", FilterOp.IN, ["/World/Group"]),
                          prim_type, END_ORDINAL, "usd-parent IN {/World/Group}")  # direct children
        _run_filter_query(stage, paths,
                          Predicate("usd-children", FilterOp.CONTAINS, ["/World/Group/Left"]),
                          prim_type, END_ORDINAL, "usd-children CONTAINS {/World/Group/Left}")
        # ShadowAPI is authored on /World/Group/Right only. Population applies
        # schemas of its own (e.g. MaterialBindingAPI lands on every gprim),
        # so filter on a schema that is selective in your scene.
        _run_filter_query(stage, paths, Predicate("usd-schemas", FilterOp.CONTAINS, ["ShadowAPI"]),
                          prim_type, END_ORDINAL, "usd-schemas CONTAINS {ShadowAPI}")
        _run_filter_query(stage, paths, Predicate(count_tok, FilterOp.HAS),
                          prim_type, END_ORDINAL, "HAS example:count")  # presence; no values
        # [/snippet:filter-predicates]

        # ---- 4. query introspection ----
        # [snippet:query-introspection]
        # Query.result() reports what a query found. The attrs argument scopes
        # the reported attribute list to the named tokens (deterministic);
        # without it the result reports every column discovered on the matched
        # prims. all_handle is the same query handle echoed back into the
        # result, so a consumer handed only the QueryResult can still read
        # from the matched set.
        with stage.query(filter=Filter([Predicate(count_tok, FilterOp.HAS)]),
                         attrs=[count_tok, prim_type]) as query:
            query.wait()
            result = query.result()
            reported = sorted(paths.token_to_string(tok) for tok in result.attributes)
            print("query introspection (HAS example:count, scoped to two attributes):")
            print("  total_prim_count:", result.total_prim_count)
            print("  reported attributes:", " ".join(reported))
            print("  all_handle == query handle:", "yes" if result.all_handle == query.handle else "no")
            rows = _rows(stage, paths, result.all_handle, count_tok, END_ORDINAL)
            # Example plumbing: the example:count values are int32 -- print
            # "path=value", sorted for deterministic output.
            print("  example:count via all_handle:",
                  " ".join(sorted(f"{path}={int(value)}" for path, value in rows)))
        # [/snippet:query-introspection]

        paths.destroy_path_list(scene_list)
        paths.destroy_path_list(target_list)

        # ---- 5. map scene-graph instancing ----
        # [snippet:instancing-queries]
        prototype_roots = instancing.get_prototype_roots(stage)
        print("prototype roots:", len(prototype_roots))
        # prototype roots: 1

        # Discover the generated prototype path from a known instance.
        prototype_root = instancing.get_prototype_root(stage, "/World/InstanceA")
        instance_roots = sorted(instancing.get_instance_roots(stage, prototype_root))
        print("instance roots:", " ".join(instance_roots))
        # instance roots: /World/InstanceA /World/InstanceB
        # [/snippet:instancing-queries]

    # Use a separate scene to demonstrate instances inside other instances.
    with ovstage.Stage("example.nested-instancing") as stage:
        # [snippet:instancing-timing]
        population.open_usd(stage, str(SCENE.with_name("nested.usda")),
                            ordinal=1, domains=PopulationDomain.PHYSICS)
        # open_usd() waits by default. Make its changes available for reads.
        stage.advance_write_floor(ordinal=1).wait()
        # [/snippet:instancing-timing]

        # [snippet:instancing-nested]
        leaf = instancing.get_prototype_root(stage, "/World/OuterSource/NestedLeaf")
        print("\n".join(sorted(instancing.get_instance_roots(stage, leaf))))
        # /World/OuterA/NestedLeaf
        # /World/OuterSource/NestedLeaf
        print(instancing.get_prototype_root(stage, "/World/OuterA/NestedLeaf") == leaf)
        # True
        # [/snippet:instancing-nested]

        # [snippet:instancing-not-found]
        try:
            instancing.get_prototype_root(stage, "/World/LeafSource")
        except OvstageError as error:
            # LeafSource is an ordinary prim, even though instances reference it.
            if error.code != ErrorCode.NOT_FOUND:
                raise
            print("not an instance root")
            # not an instance root
        # [/snippet:instancing-not-found]

    return 0


# [snippet:filter-query]
def _run_filter_query(stage, paths, predicate, prim_type, end_ordinal, label):
    """Run one filter query end to end and report its matches.

    No path list is supplied -- the stage finds the prims, evaluating against
    the latest committed state at execution time, and the matched prims are
    enumerated by reading the reserved usd-prim-type column over the query
    (the typed prims in this example carry it). The `with` block releases the query
    handle.
    """
    with stage.query(filter=Filter([predicate])) as query:
        query.wait()
        rows = list(_rows(stage, paths, query, prim_type, end_ordinal))

    # Example plumbing: report the matched paths, sorted for deterministic output.
    matched = sorted(path for path, _ in rows)
    print(f"{label} -> {len(matched)} matched:", " ".join(matched))
# [/snippet:filter-query]


# [snippet:resolve-matched-prims]
def _rows(stage, paths, query, attribute, end_ordinal):
    """Yield (path string, tensor row value) for every prim a query matched.

    Each read group names its covered prims as a path-list handle (a borrow
    owned by the read -- do not destroy it) plus per-group indices; the path
    dictionary resolves the handles back to strings. The tensor row for a
    local prim honors data.index_map when present (gather / reorder / dedup)
    and is the identity otherwise.
    """
    with stage.read_attributes(query, [attribute], OrdinalRange.latest(end_ordinal)) as read:
        read.wait()
        for group in read.groups():
            handles = paths.get_paths(group.prim_list)
            values = group.array(0)
            for local in range(group.prim_count):
                path = paths.path_to_string(handles[group.prim_index(local)])
                row = group.data_row_index(local) if group.has_data_index_map else local
                yield path, values[row]
            stage.release_group(group)
# [/snippet:resolve-matched-prims]


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OvstageError as err:
        print(f"ovstage error (code {int(err.code)}): {err.message}")
        raise SystemExit(1)
