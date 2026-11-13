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
# Public ovstage population description test: the selector predicates that look at
# other prims (TARGETS, HAS_DESCENDANT, IS_INSTANCE, IS_TARGETED_BY) and derived
# properties. These are the tested source for the population guide's snippets. The C
# sibling is tests/c/test_population_descriptions.cpp. CPU-only.

import pytest

from ovstage import Filter, FilterOp, OrdinalRange, PathDictionary, Predicate, PrimPath, Token, population
from ovstage.population import (DerivedProperty, DeriveWhen, Expression, PrimPredicate, PropertyPredicate,
                                Selector)

pytestmark = pytest.mark.skipif(
    not population.available(),
    reason="libovstage was built without the ovstage population bridge",
)

# A scene in centimeters for every example: a Cube that names a proxy Mesh, an instance
# and a PointInstancer whose prototype holds a light, a sibling of each holding none,
# a material bound for the `preview` purpose, a `lights` collection and a drive.
SCENE_USDA = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
)
def Xform "World" (
    prepend apiSchemas = ["MaterialBindingAPI", "CollectionAPI:lights"]
)
{
    rel material:binding:preview = </World/Looks/Preview>
    rel collection:lights:includes = </World/Lights>
    def Scope "Looks"
    {
        def Material "Preview"
        {
        }
    }
    def Cube "Render"
    {
        rel proxyPrim = </World/Proxy>
    }
    def Cube "Sized"
    {
        double size = 3
    }
    def Mesh "Proxy"
    {
        point3f[] points = [(0, 0, 0)]
    }
    def Mesh "Unused"
    {
        point3f[] points = [(0, 0, 0)]
    }
    def Xform "Lamp" (
        instanceable = true
        prepend references = </LampPrototype>
    )
    {
    }
    def Xform "Crate" (
        instanceable = true
        prepend references = </CratePrototype>
    )
    {
    }
    def PointInstancer "Lights"
    {
        rel prototypes = </World/Lights/Prototype>
        def Xform "Prototype"
        {
            def Sphere "Bulb" (
                prepend apiSchemas = ["LightAPI"]
            )
            {
            }
        }
    }
    def PointInstancer "Boxes"
    {
        rel prototypes = </World/Boxes/Prototype>
        def Xform "Prototype"
        {
            def Cube "Box"
            {
            }
        }
    }
    def Xform "Joint" (
        prepend apiSchemas = ["PhysicsDriveAPI:rotX"]
    )
    {
    }
    def Xform "Free"
    {
    }
}
class Xform "LampPrototype"
{
    def Sphere "Bulb" (
        prepend apiSchemas = ["LightAPI"]
    )
    {
    }
}
class Xform "CratePrototype"
{
    def Cube "Box"
    {
    }
}
"""


def _populate(stage, selector):
    population.open_usd_from_string_with_desc(
        stage, SCENE_USDA, ordinal=1, time=population.DEFAULT_TIME, descs=[population.Desc(selectors=[selector])]
    )
    stage.advance_write_floor(1).wait()


def _exists(stage, path: str) -> bool:
    query = stage.query(filter=Filter([Predicate("usd-path", FilterOp.IN, [path])]))
    query.wait()
    count = stage.fetch_query_result(query).total_prim_count
    stage.release_query(query).wait()
    return count == 1


def _publishes(stage, paths, path: str, name: str) -> bool:
    """Whether the prim at `path` carries the column `name`."""
    query = stage.query(filter=Filter([Predicate("usd-path", FilterOp.IN, [path])]))
    query.wait()
    found = paths.intern_token(name) in set(stage.fetch_query_result(query).attributes)
    stage.release_query(query).wait()
    return found


def _read(stage, paths, path: str, name: str):
    """The values of the column `name` on the prim at `path`, as a flat list."""
    with paths.create_path_list_from_strings([path]) as path_list:
        with stage.query_from_path_list(path_list) as query:
            with stage.read_attributes(query, [paths.intern_token(name)], OrdinalRange.latest(1)) as read:
                read.wait()
                values = []
                for group in read.groups():
                    with group:
                        values.extend(group.columns[0].array(0).reshape(-1).tolist())
                return values


def test_select_by_relationship_target(stage):
    # [snippet:select-by-relationship-target]
    # Every prim whose `proxyPrim` relationship names a Mesh, with that relationship.
    proxies_mesh = PropertyPredicate.and_(
        PropertyPredicate.has_name("proxyPrim"),
        PropertyPredicate.targets(PrimPredicate.has_type("Mesh")),
    )
    proxied = Selector(
        prim_predicate=PrimPredicate.has_property(proxies_mesh),
        property_predicate=PropertyPredicate.has_name("proxyPrim"),
    )
    # [/snippet:select-by-relationship-target]
    _populate(stage, proxied)

    assert _exists(stage, "/World/Render")
    assert not _exists(stage, "/World/Sized")


def test_select_by_descendant_and_instances(stage):
    # [snippet:select-by-descendant]
    # Every prim whose `prototypes` relationship names a prim that holds a light, with that relationship.
    holds_light = PrimPredicate.has_descendant(PrimPredicate.has_applied_schema("LightAPI"))
    protos_hold_light = PropertyPredicate.and_(
        PropertyPredicate.has_name("prototypes"),
        PropertyPredicate.targets(holds_light),
    )
    instancers = Selector(
        prim_predicate=PrimPredicate.has_property(protos_hold_light),
        property_predicate=PropertyPredicate.has_name("prototypes"),
    )
    # [/snippet:select-by-descendant]
    # [snippet:select-instances]
    # Every instance whose prototype holds a light.
    light_instances = PrimPredicate.and_(PrimPredicate.is_instance(), holds_light)
    # [/snippet:select-instances]
    _populate(stage, Selector(prim_predicate=PrimPredicate.or_(instancers.prim_predicate, light_instances)))

    assert _exists(stage, "/World/Lights")
    assert not _exists(stage, "/World/Boxes")
    assert _exists(stage, "/World/Lamp")
    assert not _exists(stage, "/World/Crate")


def test_select_targeted_prims(stage):
    # [snippet:select-targeted-prims]
    # Every Mesh a `proxyPrim` relationship of an Imageable names, with its points.
    referrer = PrimPredicate.is_a_type("Imageable")
    via_proxy = PropertyPredicate.has_name("proxyPrim")
    proxies = Selector(
        prim_predicate=PrimPredicate.and_(
            PrimPredicate.has_type("Mesh"),
            PrimPredicate.is_targeted_by(referrer, via_proxy),
        ),
        property_predicate=PropertyPredicate.has_name("points"),
    )
    # [/snippet:select-targeted-prims]
    _populate(stage, proxies)

    assert _exists(stage, "/World/Proxy")
    assert not _exists(stage, "/World/Unused")


def test_derive_default_in_stage_units(stage):
    with PathDictionary(stage) as paths:
        # [snippet:derive-default-in-stage-units]
        one_meter = Expression.divide(Expression.number(1.0), Expression.stage_metadata("metersPerUnit"))

        is_cube = PrimPredicate.has_type("Cube")
        rules = [
            # `size` is declared by Cube, so the column keeps its declared double type.
            DerivedProperty("size", one_meter, prim_predicate=is_cube, when=DeriveWhen.WHEN_UNAUTHORED),
        ]
        selector = Selector(
            prim_predicate=is_cube, property_predicate=PropertyPredicate.has_name("size"), derived_properties=rules
        )
        # [/snippet:derive-default-in-stage-units]
        _populate(stage, selector)

        # One meter in centimeters where nothing is authored; the authored size as read.
        assert _read(stage, paths, "/World/Render", "size") == [pytest.approx(100.0)]
        assert _read(stage, paths, "/World/Sized", "size") == [pytest.approx(3.0)]


def test_derive_bound_material(stage):
    with PathDictionary(stage) as paths:
        # [snippet:derive-bound-material]
        bound = Expression.resolved_bound_material("preview")
        binding = DerivedProperty("resolvedMaterial:preview", bound)
        # [/snippet:derive-bound-material]
        _populate(stage, Selector(prim_predicate=PrimPredicate.has_type("Mesh"), derived_properties=[binding]))

        # Inherited from /World, which binds it for the `preview` purpose.
        targets = _read(stage, paths, "/World/Proxy", "resolvedMaterial:preview")
        assert [paths.path_to_string(PrimPath(int(v))) for v in targets] == ["/World/Looks/Preview"]


def test_derive_collection_members(stage):
    with PathDictionary(stage) as paths:
        # [snippet:derive-collection-members]
        is_light = PrimPredicate.has_applied_schema("LightAPI")
        lights = Expression.resolved_collection_members("lights", is_light, PropertyPredicate.none())
        # [/snippet:derive-collection-members]
        _populate(stage, Selector(prim_predicate=PrimPredicate.has_path("/World"),
                                  derived_properties=[DerivedProperty("lights", lights)]))

        # The collection expands below /World/Lights; only the light is kept.
        members = _read(stage, paths, "/World", "lights")
        assert [paths.path_to_string(PrimPath(int(v))) for v in members] == ["/World/Lights/Prototype/Bulb"]


def test_derive_binding_strength(stage):
    with PathDictionary(stage) as paths:
        # [snippet:derive-binding-strength]
        strength = Expression.resolved_material_binding_strength("preview")
        binding_strength = DerivedProperty("resolvedMaterial:preview:winning-binding-strength", strength)
        # [/snippet:derive-binding-strength]
        _populate(stage, Selector(prim_predicate=PrimPredicate.has_type("Mesh"),
                                  derived_properties=[binding_strength]))

        # The binding authors no strength, so it takes the fallback.
        values = _read(stage, paths, "/World/Proxy", "resolvedMaterial:preview:winning-binding-strength")
        assert [paths.token_to_string(Token(int(v))) for v in values] == ["weakerThanDescendants"]


def test_derive_for_one_instance(stage):
    with PathDictionary(stage) as paths:
        # [snippet:derive-for-one-instance]
        value = Expression.number(1000.0)
        applies_rot_x = PrimPredicate.has_applied_schema("PhysicsDriveAPI:rotX")
        rot_x_rule = DerivedProperty(
            "drive:rotX:physics:maxForce", value,
            prim_predicate=applies_rot_x, when=DeriveWhen.WHEN_UNAUTHORED,
        )
        # [/snippet:derive-for-one-instance]
        _populate(stage, Selector(prim_predicate=PrimPredicate.has_type("Xform"), derived_properties=[rot_x_rule]))

        assert _read(stage, paths, "/World/Joint", "drive:rotX:physics:maxForce") == [pytest.approx(1000.0)]
        assert not _publishes(stage, paths, "/World/Free", "drive:rotX:physics:maxForce")
