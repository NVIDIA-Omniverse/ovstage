# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Public wheel smoke coverage for scene-graph-instancing queries."""

import pytest

from ovstage import PopulationDomain, instancing, population

INSTANCING_USDA = """#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World"
{
    def Xform "Source" (
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
        prepend references = </World/Source>
    )
    {
    }

    def Xform "InstanceB" (
        instanceable = true
        prepend references = </World/Source>
    )
    {
    }
}
"""


def test_resolve_logical_paths(stage):
    population.open_usd_from_string(stage, INSTANCING_USDA, ordinal=1, domains=PopulationDomain.RENDERING)
    # [snippet:instancing-resolve-paths]
    prototype = instancing.get_prototype_root(stage, "/World/InstanceA")
    results = instancing.resolve_paths(stage, [
        "/World/InstanceA/Box", "/World/Source/Box", "/World/InstanceA/Missing",
        "/World/InstanceA", "/World/InstanceA/Box",
    ])
    assert results[0] == instancing.PathResolution(
        instancing.PathKind.INSTANCE, "/World/InstanceA", prototype, prototype + "/Box")
    assert results[1] == instancing.PathResolution(
        instancing.PathKind.NON_INSTANCE, None, None, "/World/Source/Box")
    assert results[2] == instancing.PathResolution(instancing.PathKind.NOT_FOUND, None, None, None)
    assert results[3].backing_path == prototype
    assert results[4] == results[0]
    assert instancing.resolve_paths(stage, []) == []
    # [/snippet:instancing-resolve-paths]


def test_instancing_query_round_trip(stage):
    if not instancing.available():
        pytest.skip("libovstage was built without the instancing query API")
    if not population.available():
        pytest.skip("libovstage was built without the population bridge")

    population.open_usd_from_string(stage, INSTANCING_USDA, ordinal=1, domains=PopulationDomain.RENDERING)
    stage.advance_write_floor(1).wait()
    prototype_roots = instancing.get_prototype_roots(stage)
    assert len(prototype_roots) == 1
    prototype_root = prototype_roots[0]
    assert prototype_root.startswith("/__Prototype_")
    assert instancing.get_prototype_root(stage, "/World/InstanceA") == prototype_root
    assert sorted(instancing.get_instance_roots(stage, prototype_root)) == [
        "/World/InstanceA",
        "/World/InstanceB",
    ]


def test_instancing_batch_query_round_trip(stage):
    if not instancing.available():
        pytest.skip("libovstage was built without the instancing query API")
    if not population.available():
        pytest.skip("libovstage was built without the population bridge")

    population.open_usd_from_string(stage, INSTANCING_USDA, ordinal=1, domains=PopulationDomain.RENDERING)
    stage.advance_write_floor(1).wait()
    roots = instancing.get_prototype_roots(stage)
    assert len(roots) == 1
    requested = [roots[0], "/World/Unrelated", roots[0]]
    result = instancing.get_instance_roots_batch(stage, requested)
    assert result == [
        ["/World/InstanceA", "/World/InstanceB"],
        [],
        ["/World/InstanceA", "/World/InstanceB"],
    ]
    assert result == [instancing.get_instance_roots(stage, root) for root in requested]
    assert result[0] is not result[2]
    assert instancing.get_instance_roots_batch(stage, []) == []
