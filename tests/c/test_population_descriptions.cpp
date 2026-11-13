// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
// property and proprietary rights in and to this material, related
// documentation and any modifications thereto. Any use, reproduction,
// disclosure or distribution of this material and related documentation
// without an express license agreement from NVIDIA CORPORATION or
// its affiliates is strictly prohibited.
//
// Public ovstage population description test: the selector predicates that look at
// other prims (TARGETS, HAS_DESCENDANT, IS_INSTANCE, IS_TARGETED_BY) and derived
// properties. Tested source for the population guide's C snippets; the Python sibling
// is tests/python/test_population_descriptions.py. CPU-only.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{

// The scene of tests/python/test_population_descriptions.py: a Cube that names a proxy
// Mesh, an instance and a PointInstancer whose prototype holds a light, a sibling of each
// holding none, a material bound for the `preview` purpose, a `lights` collection and a drive.
const char kSceneUsda[] = R"usda(#usda 1.0
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
)usda";

ovx_string_t str(const char* s)
{
    return ovx_string_t{ s, std::strlen(s) };
}

bool waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq)
{
    if (enq.status != OVSTAGE_OK)
        return false;
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t status = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    ovstage_release_op(stage, enq.op_index);
    return status == OVSTAGE_OK && wait.error_op_id_count == 0;
}

class PopulationDescriptionsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.population_descriptions";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        ASSERT_NE(stage_, nullptr);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);
    }

    void TearDown() override
    {
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    // Populate the scene with one description holding `selector` alone.
    void populate(const ovstage_population_selector_t& selector)
    {
        ovstage_population_desc_t desc = {};
        desc.selectors = &selector;
        desc.selector_count = 1;
        const ovstage_population_enqueue_result_t pop = ovstage_population_open_usd_from_string_with_desc(
            stage_, str(kSceneUsda), /*ordinal=*/1, /*time=*/0.0, &desc, 1);
        ASSERT_EQ(pop.status, OVSTAGE_OK);
        ovstage_population_op_wait_result_t wait{};
        ASSERT_EQ(ovstage_population_wait_op(stage_, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait), OVSTAGE_OK);
        ASSERT_EQ(wait.error_op_id_count, 0u);
    }

    // The columns of the prim at `path`, or none when the prim is not populated.
    std::vector<std::string> columns(const char* path)
    {
        ovx_string_t value = str(path);
        ovstage_predicate_t pred{};
        pred.attribute.string = str("usd-path");
        pred.op = OVSTAGE_FILTER_OP_IN;
        pred.values = &value;
        pred.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &pred;
        filter.count = 1;

        std::vector<std::string> names;
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        if (waitOp(stage_, ovstage_query(stage_, &filter, nullptr, 0, &query)))
        {
            ovstage_query_result_t result{};
            if (ovstage_fetch_query_result(stage_, query, OVSTAGE_TIMEOUT_INFINITE, &result) == OVSTAGE_OK)
            {
                if (result.total_prim_count == 1)
                {
                    for (size_t i = 0; i < result.attribute_count; ++i)
                        names.push_back(name(result.attributes[i]));
                    // An empty name marks a populated prim that publishes no column.
                    names.push_back("");
                }
                ovstage_release_query_result(stage_, &result);
            }
        }
        if (query != OVSTAGE_INVALID_QUERY_HANDLE)
            waitOp(stage_, ovstage_release_query(stage_, query));
        return names;
    }

    bool exists(const char* path) { return !columns(path).empty(); }

    bool publishes(const char* path, const char* column)
    {
        const std::vector<std::string> names = columns(path);
        return std::find(names.begin(), names.end(), column) != names.end();
    }

    std::string name(ovx_token_t token)
    {
        ovx_string_t s{};
        EXPECT_EQ(path_dictionary_get_strings_from_tokens(dict_, &token, 1, &s).status, OVX_API_SUCCESS);
        return std::string(s.ptr ? s.ptr : "", s.ptr ? s.length : 0);
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
};

} // namespace

TEST_F(PopulationDescriptionsTest, SelectByRelationshipTarget)
{
    // [snippet:select-by-relationship-target-c]
    // Every prim whose `proxyPrim` relationship names a Mesh, with that relationship.
    ovx_string_t proxy_prim[] = { literal_to_ovx_string("proxyPrim") };
    ovx_string_t mesh[] = { literal_to_ovx_string("Mesh") };
    ovstage_population_prim_predicate_t is_mesh = ovstage_population_prim_predicate_has_type(mesh, 1);
    ovstage_population_property_predicate_t proxy_terms[2] = {
        ovstage_population_property_predicate_has_name(proxy_prim, 1),
        ovstage_population_property_predicate_targets(&is_mesh) };
    ovstage_population_property_predicate_t proxies_mesh = ovstage_population_property_predicate_and(proxy_terms, 2);

    ovstage_population_selector_t proxied = {};
    proxied.prim_predicate = ovstage_population_prim_predicate_has_property(&proxies_mesh);
    proxied.property_predicate = ovstage_population_property_predicate_has_name(proxy_prim, 1);
    // [/snippet:select-by-relationship-target-c]
    populate(proxied);

    EXPECT_TRUE(publishes("/World/Render", "proxyPrim"));
    EXPECT_FALSE(exists("/World/Sized"));
}

TEST_F(PopulationDescriptionsTest, SelectByDescendantAndInstances)
{
    // [snippet:select-by-descendant-c]
    // Every prim whose `prototypes` relationship names a prim that holds a light, with that relationship.
    ovx_string_t prototypes[] = { literal_to_ovx_string("prototypes") };
    ovx_string_t light_api[] = { literal_to_ovx_string("LightAPI") };
    ovstage_population_prim_predicate_t is_light = ovstage_population_prim_predicate_has_applied_schema(light_api, 1);
    ovstage_population_prim_predicate_t holds_light = ovstage_population_prim_predicate_has_descendant(&is_light);
    ovstage_population_property_predicate_t proto_terms[2] = {
        ovstage_population_property_predicate_has_name(prototypes, 1),
        ovstage_population_property_predicate_targets(&holds_light) };
    ovstage_population_property_predicate_t protos_hold_light =
        ovstage_population_property_predicate_and(proto_terms, 2);

    ovstage_population_selector_t instancers = {};
    instancers.prim_predicate = ovstage_population_prim_predicate_has_property(&protos_hold_light);
    instancers.property_predicate = ovstage_population_property_predicate_has_name(prototypes, 1);
    // [/snippet:select-by-descendant-c]
    // [snippet:select-instances-c]
    // Every instance whose prototype holds a light.
    ovstage_population_prim_predicate_t instance_terms[2] = {
        ovstage_population_prim_predicate_is_instance(),
        ovstage_population_prim_predicate_has_descendant(&is_light) };
    ovstage_population_prim_predicate_t light_instances = ovstage_population_prim_predicate_and(instance_terms, 2);
    // [/snippet:select-instances-c]
    ovstage_population_prim_predicate_t either[2] = { instancers.prim_predicate, light_instances };
    ovstage_population_selector_t both = {};
    both.prim_predicate = ovstage_population_prim_predicate_or(either, 2);
    populate(both);

    EXPECT_TRUE(exists("/World/Lights"));
    EXPECT_FALSE(exists("/World/Boxes"));
    EXPECT_TRUE(exists("/World/Lamp"));
    EXPECT_FALSE(exists("/World/Crate"));
}

TEST_F(PopulationDescriptionsTest, SelectTargetedPrims)
{
    // [snippet:select-targeted-prims-c]
    // Every Mesh a `proxyPrim` relationship of an Imageable names, with its points.
    ovx_string_t imageable[] = { literal_to_ovx_string("Imageable") };
    ovx_string_t proxy_prim[] = { literal_to_ovx_string("proxyPrim") };
    ovx_string_t mesh[] = { literal_to_ovx_string("Mesh") };
    ovx_string_t points[] = { literal_to_ovx_string("points") };
    ovstage_population_prim_predicate_t referrer = ovstage_population_prim_predicate_is_a_type(imageable, 1);
    ovstage_population_property_predicate_t via_proxy = ovstage_population_property_predicate_has_name(proxy_prim, 1);
    ovstage_population_prim_predicate_t proxied_terms[2] = {
        ovstage_population_prim_predicate_has_type(mesh, 1),
        ovstage_population_prim_predicate_is_targeted_by(&referrer, &via_proxy) };

    ovstage_population_selector_t proxies = {};
    proxies.prim_predicate = ovstage_population_prim_predicate_and(proxied_terms, 2);
    proxies.property_predicate = ovstage_population_property_predicate_has_name(points, 1);
    // [/snippet:select-targeted-prims-c]
    populate(proxies);

    EXPECT_TRUE(publishes("/World/Proxy", "points"));
    EXPECT_FALSE(exists("/World/Unused"));
}

TEST_F(PopulationDescriptionsTest, DeriveDefaultInStageUnits)
{
    // [snippet:derive-default-in-stage-units-c]
    ovstage_population_expression_t one = ovstage_population_expression_number(1.0);
    ovstage_population_expression_t mpu =
        ovstage_population_expression_stage_metadata(literal_to_ovx_string("metersPerUnit"));
    ovstage_population_expression_t one_meter = ovstage_population_expression_divide(&one, &mpu);

    ovx_string_t cube[] = { literal_to_ovx_string("Cube") };
    ovx_string_t size_name[] = { literal_to_ovx_string("size") };
    ovstage_population_prim_predicate_t is_cube = ovstage_population_prim_predicate_has_type(cube, 1);
    // `size` is declared by Cube, so the column keeps its declared double type.
    ovstage_population_derived_property_t size = {};
    size.name = literal_to_ovx_string("size");
    size.prim_predicate = &is_cube;
    size.when = OVSTAGE_POPULATION_DERIVE_WHEN_UNAUTHORED;
    size.value = &one_meter;

    ovstage_population_selector_t selector = {};
    selector.prim_predicate = is_cube;
    selector.property_predicate = ovstage_population_property_predicate_has_name(size_name, 1);
    selector.derived_properties = &size;
    selector.derived_property_count = 1;
    // [/snippet:derive-default-in-stage-units-c]
    populate(selector);

    // Derived where nothing is authored, read as authored elsewhere: both prims carry it.
    EXPECT_TRUE(publishes("/World/Render", "size"));
    EXPECT_TRUE(publishes("/World/Sized", "size"));
}

TEST_F(PopulationDescriptionsTest, DeriveBoundMaterial)
{
    // [snippet:derive-bound-material-c]
    ovstage_population_expression_t bound =
        ovstage_population_expression_resolved_bound_material(literal_to_ovx_string("preview"), NULL);
    ovstage_population_derived_property_t binding = {};
    binding.name = literal_to_ovx_string("resolvedMaterial:preview");
    binding.value = &bound;
    // [/snippet:derive-bound-material-c]
    ovx_string_t mesh[] = { literal_to_ovx_string("Mesh") };
    ovstage_population_selector_t selector = {};
    selector.prim_predicate = ovstage_population_prim_predicate_has_type(mesh, 1);
    selector.derived_properties = &binding;
    selector.derived_property_count = 1;
    populate(selector);

    EXPECT_TRUE(publishes("/World/Proxy", "resolvedMaterial:preview"));
}

TEST_F(PopulationDescriptionsTest, DeriveCollectionMembers)
{
    // [snippet:derive-collection-members-c]
    ovx_string_t light_api[] = { literal_to_ovx_string("LightAPI") };
    ovstage_population_prim_predicate_t is_light = ovstage_population_prim_predicate_has_applied_schema(light_api, 1);
    ovstage_population_property_predicate_t no_property = ovstage_population_property_predicate_none();
    ovstage_population_expression_t lights = ovstage_population_expression_resolved_collection_members(
        literal_to_ovx_string("lights"), &is_light, &no_property);
    // [/snippet:derive-collection-members-c]
    ovstage_population_derived_property_t rule = {};
    rule.name = literal_to_ovx_string("lights");
    rule.value = &lights;
    ovx_string_t world[] = { literal_to_ovx_string("/World") };
    ovstage_population_selector_t selector = {};
    selector.prim_predicate = ovstage_population_prim_predicate_has_path(world, 1);
    selector.derived_properties = &rule;
    selector.derived_property_count = 1;
    populate(selector);

    EXPECT_TRUE(publishes("/World", "lights"));
}

TEST_F(PopulationDescriptionsTest, DeriveBindingStrength)
{
    // [snippet:derive-binding-strength-c]
    ovstage_population_expression_t strength =
        ovstage_population_expression_resolved_material_binding_strength(literal_to_ovx_string("preview"), NULL);
    ovstage_population_derived_property_t binding_strength = {};
    binding_strength.name = literal_to_ovx_string("resolvedMaterial:preview:winning-binding-strength");
    binding_strength.value = &strength;
    // [/snippet:derive-binding-strength-c]
    ovx_string_t mesh[] = { literal_to_ovx_string("Mesh") };
    ovstage_population_selector_t selector = {};
    selector.prim_predicate = ovstage_population_prim_predicate_has_type(mesh, 1);
    selector.derived_properties = &binding_strength;
    selector.derived_property_count = 1;
    populate(selector);

    EXPECT_TRUE(publishes("/World/Proxy", "resolvedMaterial:preview:winning-binding-strength"));
}

TEST_F(PopulationDescriptionsTest, DeriveForOneInstance)
{
    // [snippet:derive-for-one-instance-c]
    ovstage_population_expression_t value = ovstage_population_expression_number(1000.0);
    ovx_string_t rot_x[] = { literal_to_ovx_string("PhysicsDriveAPI:rotX") };
    ovstage_population_prim_predicate_t applies_rot_x = ovstage_population_prim_predicate_has_applied_schema(rot_x, 1);
    ovstage_population_derived_property_t rot_x_rule = {};
    rot_x_rule.name = literal_to_ovx_string("drive:rotX:physics:maxForce");
    rot_x_rule.prim_predicate = &applies_rot_x;
    rot_x_rule.when = OVSTAGE_POPULATION_DERIVE_WHEN_UNAUTHORED;
    rot_x_rule.value = &value;
    // [/snippet:derive-for-one-instance-c]
    ovx_string_t xform[] = { literal_to_ovx_string("Xform") };
    ovstage_population_selector_t selector = {};
    selector.prim_predicate = ovstage_population_prim_predicate_has_type(xform, 1);
    selector.derived_properties = &rot_x_rule;
    selector.derived_property_count = 1;
    populate(selector);

    EXPECT_TRUE(publishes("/World/Joint", "drive:rotX:physics:maxForce"));
    EXPECT_FALSE(publishes("/World/Free", "drive:rotX:physics:maxForce"));
}
