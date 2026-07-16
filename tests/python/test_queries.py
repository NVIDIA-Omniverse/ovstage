# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage query/filter test: assert built-in metadata filters discover
# client-authored prims. CPU-only. The queries example is the workflow tour; this
# file holds the compact asserted snippets (per-predicate match counts + query
# introspection) that the stage-queries skill sources.

import math

import numpy as np
import pytest

from ovstage import ErrorCode, Filter, FilterOp, PathDictionary, PopulationDomain, Predicate, population

# A small scene mirroring examples/python/queries/queries.usda: two meshes under
# a Group (Right carries an applied ShadowAPI schema), a Cube, and a prototype
# referenced by two instanceable references (which land as XformInstance prims).
QUERY_USDA = """#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Xform "Group"
    {
        def Mesh "Left"
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 0, 1)]
            uniform token subdivisionScheme = "none"
        }

        def Mesh "Right" (
            prepend apiSchemas = ["ShadowAPI"]
        )
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0, 0, 0), (-1, 0, 0), (0, 0, -1)]
            uniform token subdivisionScheme = "none"
        }
    }

    def Cube "Anchor"
    {
        double size = 1.0
    }

    def Xform "Prototype" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Looks/BoxMaterial>

        def Cube "Box"
        {
            double size = 1.0
        }
    }

    def Scope "Looks"
    {
        def Material "BoxMaterial"
        {
            token outputs:surface.connect = </World/Looks/BoxMaterial/Shader.outputs:surface>

            def Shader "Shader"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.6, 0.6, 0.6)
                token outputs:surface
            }
        }
    }

    def Xform "InstanceA" (
        instanceable = true
        prepend references = </World/Prototype>
    )
    {
    }

    def Xform "InstanceB" (
        instanceable = true
        prepend references = </World/Prototype>
    )
    {
    }
}

def Xform "Worldwide"
{
}
"""


def _prim_count(stage, filter_) -> int:
    with stage.query(filter=filter_) as query:
        query.wait()
        return query.result().total_prim_count


def _populate_query_scene(stage, paths):
    """Populate QUERY_USDA and write example:count to two prims. Returns the
    (usd-prim-type, example:count) tokens the filter/introspection cases use."""
    population.open_usd_from_string(
        stage, QUERY_USDA, ordinal=1, time_code=math.nan, domains=PopulationDomain.ALL
    )
    stage.advance_write_floor(ordinal=1).wait()

    prim_type = paths.intern_token("usd-prim-type")
    count = paths.intern_token("example:count")
    targets = paths.create_path_list_from_strings(["/World/Anchor", "/World/Group/Left"])
    count_query = stage.query_from_path_list(targets)
    try:
        stage.write_attribute(
            count_query, count, ordinal=2, tensors=np.array([5, 3], np.int32), is_array=False
        ).wait()
        stage.advance_write_floor(ordinal=2).wait()
    finally:
        stage.release_query(count_query).wait()
        paths.destroy_path_list(targets)
    return prim_type, count


def test_query_by_usd_path_and_has_attribute(stage):
    """Write-only (no population): usd-path IN and HAS on client-authored prims."""
    with PathDictionary(stage) as paths:
        attr = paths.intern_token("temperature")
        prim_paths = paths.create_path_list_from_strings(
            ["/World/MeshA", "/World/MeshB", "/World/Camera"]
        )
        query = stage.query_from_path_list(prim_paths)
        try:
            stage.write_attribute(
                query,
                attr,
                ordinal=1,
                tensors=np.array([1.0, 2.0, 3.0], np.float32),
                is_array=False,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()

            # [snippet:query-by-usd-path]
            path_filter = Filter([Predicate("usd-path", FilterOp.IN, ["/World/MeshB"])])
            with stage.query(filter=path_filter) as meshes:
                meshes.wait()
                result = meshes.result()
                assert result.total_prim_count == 1
            # [/snippet:query-by-usd-path]

            # [snippet:query-has-attribute]
            has_filter = Filter([Predicate(attr, FilterOp.HAS)])
            # An unscoped query reports the attributes discovered on the matched prims.
            # The three prims here carry only `temperature`, so discovery must return
            # exactly that one token; this also guards against spurious/extra attributes
            # being reported (scoping with attrs= would make the check tautological).
            with stage.query(filter=has_filter) as with_attr:
                with_attr.wait()
                result = with_attr.result()
                assert result.total_prim_count == 3
                assert result.attributes == [attr]
            # [/snippet:query-has-attribute]
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)


def test_query_predicate_matrix(stage):
    """Each supported predicate selects the documented prim set (see the queries
    example). Requires the population bridge to author the metadata built-ins."""
    if not population.available():
        pytest.skip("libovstage was built without the ovstage population bridge")

    with PathDictionary(stage) as paths:
        _, count_tok = _populate_query_scene(stage, paths)

        # [snippet:query-predicate-matrix]
        # The support matrix is narrow: HAS works on any attribute; the value
        # operators pair only with the reserved metadata built-ins. Predicates in
        # one Filter AND together; values are always strings. Queries resolve
        # against the latest committed state, so no path list goes in.
        def matched(attribute, op, *values):
            return _prim_count(stage, Filter([Predicate(attribute, op, list(values))]))

        assert matched("usd-prim-type", FilterOp.IN, "Mesh") == 2  # Left, Right
        assert matched("usd-path", FilterOp.PREFIX, "/World/Group") == 3  # Group + 2 meshes
        # PREFIX is byte-prefix: "/World" also matches "/Worldwide"; trailing "/" scopes.
        assert matched("usd-path", FilterOp.PREFIX, "/Worldwide") == 1
        assert matched("usd-path", FilterOp.PREFIX, "/World") > matched(
            "usd-path", FilterOp.PREFIX, "/World/"
        )
        assert matched("usd-parent", FilterOp.IN, "/World/Group") == 2  # direct children
        assert matched("usd-children", FilterOp.CONTAINS, "/World/Group/Left") == 1  # Group
        assert matched("usd-schemas", FilterOp.CONTAINS, "ShadowAPI") == 1  # Right
        # HAS takes the interned token for a user attribute (no value test).
        assert matched(count_tok, FilterOp.HAS) == 2  # Anchor, Group/Left
        # [/snippet:query-predicate-matrix]

        # usd-active appears in the header contract but is not supported (a
        # live prim is always active, so it carries no information): any
        # predicate naming it is rejected at enqueue. Subject to removal in a
        # future release.
        rejected = stage.query(filter=Filter([Predicate("usd-active", FilterOp.IN, ["true"])]))
        assert rejected.op.status == ErrorCode.NOT_SUPPORTED


def test_query_result_introspection(stage):
    """Query.result() reports the match count, the scoped attribute list, and the
    reusable all_handle echoed back into the result."""
    if not population.available():
        pytest.skip("libovstage was built without the ovstage population bridge")

    with PathDictionary(stage) as paths:
        prim_type, count = _populate_query_scene(stage, paths)

        # [snippet:query-result-introspection]
        # Scoping attrs to named tokens keeps the reported attribute list
        # deterministic; all_handle is the same query handle echoed into the
        # result, so a consumer handed only the QueryResult can still read the set.
        with stage.query(filter=Filter([Predicate(count, FilterOp.HAS)]),
                         attrs=[count, prim_type]) as query:
            query.wait()
            result = query.result()
            assert result.total_prim_count == 2
            assert set(result.attributes) == {count, prim_type}
            assert result.all_handle == query.handle
        # [/snippet:query-result-introspection]
