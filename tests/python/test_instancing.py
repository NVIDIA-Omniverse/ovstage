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


def test_instancing_query_round_trip(stage):
    if not instancing.available():
        pytest.skip("libovstage was built without the instancing query API")
    if not population.available():
        pytest.skip("libovstage was built without the population bridge")

    population.open_usd_from_string(stage, INSTANCING_USDA, domains=PopulationDomain.RENDERING)
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
