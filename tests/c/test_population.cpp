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
// Public ovstage population test (USD -> ovstage): populate from inline USDA and
// query back; add/remove a USD reference; reset; and confirm a missing file
// fails. Tested source for the loading-usd skill's C snippets; the Python sibling
// is tests/python/test_population.py. CPU-only.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

ovx_string_t str(const char* s)
{
    return ovx_string_t{ s, std::strlen(s) };
}

// Drive an ordinary (non-population) enqueue to completion.
bool waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq)
{
    if (enq.status != OVSTAGE_OK)
        return false;
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t status = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    ovstage_release_op(stage, enq.op_index);
    return status == OVSTAGE_OK && wait.error_op_id_count == 0;
}

// Drive a population enqueue to completion; true iff it ran error-free.
bool waitPopOk(ovstage_instance_t* stage, ovstage_population_enqueue_result_t pop, const char* what)
{
    if (pop.status != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " enqueue rejected (code " << pop.status << ")";
        return false;
    }
    ovstage_population_op_wait_result_t wait{};
    const ovstage_api_status_t st = ovstage_population_wait_op(stage, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    if (st != OVSTAGE_OK || wait.error_op_id_count != 0)
    {
        ADD_FAILURE() << what << " op failed (code " << st << ")";
        return false;
    }
    return true;
}

// A population op expected to FAIL (rejected at enqueue, or failed at wait).
bool waitPopFails(ovstage_instance_t* stage, ovstage_population_enqueue_result_t pop)
{
    if (pop.status != OVSTAGE_OK)
        return true;
    ovstage_population_op_wait_result_t wait{};
    const ovstage_api_status_t st = ovstage_population_wait_op(stage, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    return st != OVSTAGE_OK || wait.error_op_id_count != 0;
}

// A minimal USD scene: one Cube under a World Xform.
const char kCubeUsda[] =
    "#usda 1.0\n"
    "(\n"
    "    defaultPrim = \"World\"\n"
    ")\n"
    "def Xform \"World\"\n"
    "{\n"
    "    def Cube \"Cube\"\n"
    "    {\n"
    "        double size = 1.0\n"
    "    }\n"
    "}\n";

// A self-contained, referenceable layer (defaultPrim set so the reference composes).
const char kRefUsda[] =
    "#usda 1.0\n"
    "(\n"
    "    defaultPrim = \"Ref\"\n"
    ")\n"
    "def Xform \"Ref\"\n"
    "{\n"
    "    def Cube \"Cube\"\n"
    "    {\n"
    "        double size = 1.0\n"
    "    }\n"
    "}\n";

// A scene where a schema selection is distinguishable from selecting everything.
const char kSchemaUsda[] =
    "#usda 1.0\n"
    "(\n"
    "    defaultPrim = \"World\"\n"
    ")\n"
    "def Xform \"World\"\n"
    "{\n"
    "    def Cube \"Body\" (prepend apiSchemas = [\"PhysicsRigidBodyAPI\"])\n"
    "    {\n"
    "        double size = 4.0\n"
    "    }\n"
    "    def Cube \"Prop\"\n"
    "    {\n"
    "        double size = 2.0\n"
    "    }\n"
    "    def Sphere \"Ball\"\n"
    "    {\n"
    "        double radius = 1.0\n"
    "    }\n"
    "}\n"
    "def Cube \"Outside\"\n"
    "{\n"
    "    double size = 9.0\n"
    "}\n";

class PopulationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.population";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        ASSERT_NE(stage_, nullptr);
    }

    void TearDown() override
    {
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    // Count prims whose usd-path starts with `prefix` (latest committed state).
    size_t prefixCount(const char* prefix)
    {
        ovx_string_t value = str(prefix);
        ovstage_predicate_t pred{};
        pred.attribute.string = str("usd-path");
        pred.op = OVSTAGE_FILTER_OP_PREFIX;
        pred.values = &value;
        pred.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &pred;
        filter.count = 1;

        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        if (!waitOp(stage_, ovstage_query(stage_, &filter, nullptr, 0, &query)))
        {
            if (query != OVSTAGE_INVALID_QUERY_HANDLE)
                waitOp(stage_, ovstage_release_query(stage_, query));
            return 0;
        }
        ovstage_query_result_t result{};
        size_t count = 0;
        if (ovstage_fetch_query_result(stage_, query, OVSTAGE_TIMEOUT_INFINITE, &result) == OVSTAGE_OK)
        {
            count = result.total_prim_count;
            ovstage_release_query_result(stage_, &result);
        }
        waitOp(stage_, ovstage_release_query(stage_, query));
        return count;
    }

    ovstage_instance_t* stage_ = nullptr;
};

TEST_F(PopulationTest, PopulateFromUsdaAndQuery)
{
    size_t matched = 0;
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;

    // [snippet:populate-and-query-c]
    // Populate the ovstage from an inline USDA string (the RENDERING domain
    // mirrors meshes/lights/materials/cameras). Population is asynchronous — wait
    // for the op. Then confirm the prim landed by querying it back by its
    // usd-path; queries resolve against the latest committed state.
    ovstage_population_enqueue_result_t pop = ovstage_population_open_usd_from_string(
        stage_, str(kCubeUsda), /*ordinal=*/1, /*time=*/0.0, OVSTAGE_POPULATION_DOMAIN_RENDERING);
    ASSERT_EQ(pop.status, OVSTAGE_OK);
    ASSERT_EQ(ovstage_population_wait_op(stage_, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, nullptr), OVSTAGE_OK);

    ovx_string_t pathValue = str("/World/Cube");
    ovstage_predicate_t predicate{};
    predicate.attribute.string = str("usd-path");
    predicate.op = OVSTAGE_FILTER_OP_IN;
    predicate.values = &pathValue;
    predicate.value_count = 1;

    ovstage_filter_t filter{};
    filter.predicates = &predicate;
    filter.count = 1;

    const bool queryOk = waitOp(stage_, ovstage_query(stage_, &filter, nullptr, 0, &query));
    if (!queryOk)
    {
        if (query != OVSTAGE_INVALID_QUERY_HANDLE)
            waitOp(stage_, ovstage_release_query(stage_, query));
        ASSERT_TRUE(queryOk);
    }

    ovstage_query_result_t result{};
    const ovstage_api_status_t fetched = ovstage_fetch_query_result(stage_, query, OVSTAGE_TIMEOUT_INFINITE, &result);
    if (fetched == OVSTAGE_OK)
    {
        matched = result.total_prim_count;
        ovstage_release_query_result(stage_, &result);
    }
    // [/snippet:populate-and-query-c]

    ASSERT_TRUE(waitOp(stage_, ovstage_release_query(stage_, query)));
    ASSERT_EQ(fetched, OVSTAGE_OK);

    EXPECT_EQ(matched, 1u);
}

TEST_F(PopulationTest, AddRemoveUsdReference)
{
    ASSERT_TRUE(waitPopOk(stage_,
        ovstage_population_open_usd_from_string(stage_, str(kCubeUsda), 1, NAN, OVSTAGE_POPULATION_DOMAIN_RENDERING),
        "open"));

    // [snippet:usd-reference-c]
    // add_usd_reference edits the USD source only; apply_usd_changes propagates it
    // into the stage (at the ordinal you pass). The add reserves a handle
    // synchronously for a later remove; removing again propagates the tombstone.
    ovstage_population_usd_reference_handle_t handle = OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE;
    ASSERT_TRUE(waitPopOk(stage_,
        ovstage_population_add_usd_reference_from_string(stage_, str(kRefUsda), str("/World/Props"), &handle),
        "add_reference"));
    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_apply_usd_changes(stage_, 2), "apply_usd_changes"));
    EXPECT_GT(prefixCount("/World/Props"), 0u);  // the referenced subtree materialized

    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_remove_usd_reference(stage_, handle), "remove_reference"));
    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_apply_usd_changes(stage_, 3), "apply_usd_changes"));
    EXPECT_EQ(prefixCount("/World/Props"), 0u);  // the referenced subtree was removed
    // [/snippet:usd-reference-c]

    // The handle is spent: removing it again fails.
    EXPECT_TRUE(waitPopFails(stage_, ovstage_population_remove_usd_reference(stage_, handle)));
}

TEST_F(PopulationTest, ResetUsdAndRepopulate)
{
    ASSERT_TRUE(waitPopOk(stage_,
        ovstage_population_open_usd_from_string(stage_, str(kCubeUsda), 1, NAN, OVSTAGE_POPULATION_DOMAIN_RENDERING),
        "open"));
    EXPECT_GT(prefixCount("/World"), 0u);

    // [snippet:reset-usd-c]
    // reset_usd clears the USD source; apply_usd_changes propagates the cleared
    // state. The stage stays usable afterwards — repopulating from USD still works.
    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_reset_usd(stage_), "reset_usd"));
    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_apply_usd_changes(stage_, 2), "apply_usd_changes"));
    EXPECT_EQ(prefixCount("/World"), 0u);  // cleared before repopulating
    ASSERT_TRUE(waitPopOk(stage_,
        ovstage_population_open_usd_from_string(stage_, str(kCubeUsda), 3, NAN, OVSTAGE_POPULATION_DOMAIN_RENDERING),
        "reopen"));
    // [/snippet:reset-usd-c]

    EXPECT_GT(prefixCount("/World"), 0u);
}

TEST_F(PopulationTest, OpenMissingFileFails)
{
    // [snippet:open-missing-file-c]
    // A missing/unreadable file fails the populate op: the enqueue is accepted but
    // the failure surfaces from ovstage_population_wait_op.
    EXPECT_TRUE(waitPopFails(stage_,
        ovstage_population_open_usd_from_file(
            stage_, str("/nonexistent/ovstage-does-not-exist.usda"), 1, NAN, OVSTAGE_POPULATION_DOMAIN_RENDERING)));
    // [/snippet:open-missing-file-c]
}

} // namespace

// Registration is process-global, irreversible, and only counts before USD first reads
// its schema definitions. gtest_discover_tests gives every case its own process, so this
// one registers into a fresh USD and leaves nothing behind for the others.
TEST_F(PopulationTest, RegisterUsdSchemas)
{
    // A single-family schema tree USD has never seen -- a core Usd* schema would resolve
    // whether or not the call below was made, and so would prove nothing. The family
    // declares one fallback, which is what the assertion at the end reads back.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "ovstage_public_schema_fixture";
    const std::filesystem::path resources = root / "resources";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(resources);

    std::ofstream descriptor((resources / "plugInfo.json").string(), std::ios::binary | std::ios::trunc);
    descriptor << R"({
    "Plugins": [
        {
            "Info": { "Types": {
                "OvstagePublicTestAPI": {
                    "alias": { "UsdSchemaBase": "OvstagePublicTestAPI" },
                    "bases": [ "UsdAPISchemaBase" ],
                    "schemaIdentifier": "OvstagePublicTestAPI",
                    "schemaKind": "singleApplyAPI"
                }
            } },
            "Name": "ovstagePublicTestSchema",
            "ResourcePath": "resources",
            "Root": "..",
            "Type": "resource"
        }
    ]
}
)";
    descriptor.close();
    ASSERT_TRUE(descriptor);

    std::ofstream generated((resources / "generatedSchema.usda").string(), std::ios::binary | std::ios::trunc);
    generated << "#usda 1.0\n(\n)\n\nclass \"OvstagePublicTestAPI\" (\n)\n{\n"
              << "    uniform float ovstageTest:strength = 1.5\n}\n";
    generated.close();
    ASSERT_TRUE(generated);

    // A prim applying that family and authoring nothing of it.
    static const char kUsda[] =
        "#usda 1.0\n"
        "(\n"
        "    defaultPrim = \"World\"\n"
        ")\n"
        "def Xform \"World\"\n"
        "{\n"
        "    def Cube \"Body\" (prepend apiSchemas = [\"OvstagePublicTestAPI\"])\n"
        "    {\n"
        "        double size = 4.0\n"
        "    }\n"
        "}\n";

    const std::string schemaDir = resources.string();

    // [snippet:register-usd-schemas-c]
    // ovstage registers only what its USD build provides (the core Usd* schemas).
    // Register any other family your stages use -- each path a plugInfo.json or a
    // directory holding one -- before the first ovstage call that reads schema
    // definitions, which population and export both do: USD reads them once, and a
    // family registered afterwards contributes nothing.
    ovx_string_t schemaPaths[] = { str(schemaDir.c_str()) };
    ASSERT_EQ(ovstage_population_register_usd_schemas(schemaPaths, 1), OVSTAGE_OK);

    // With the family registered, a description can select on it and publish what it
    // declares -- including properties the stage never authored, which resolve to their
    // schema fallbacks. Without the registration they would not exist to publish.
    ovx_string_t family[] = { str("OvstagePublicTestAPI") };
    ovstage_population_selector_t selector = {};
    selector.prim_predicate = ovstage_population_prim_predicate_has_applied_schema(family, 1);
    selector.property_predicate = ovstage_population_property_predicate_declared_by_schema(family, 1);
    ovstage_population_desc_t desc = {};
    desc.selectors = &selector;
    desc.selector_count = 1;

    ASSERT_TRUE(waitPopOk(stage_,
        ovstage_population_open_usd_from_string_with_desc(
            stage_, str(kUsda), /*ordinal=*/1, /*time=*/NAN, &desc, 1),
        "populate with a registered schema family"));
    // [/snippet:register-usd-schemas-c]

    // Read the unauthored attribute back. The stage carries no such value, so it
    // resolves only because the family's definitions were registered.
    ovstage_write_floor_desc_t writeFloor{};
    writeFloor.ordinal = 1;
    writeFloor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &writeFloor)));

    ovx_string_t primPath = str("/World/Body");
    ovstage_predicate_t pred{};
    pred.attribute.string = str("usd-path");
    pred.op = OVSTAGE_FILTER_OP_IN;
    pred.values = &primPath;
    pred.value_count = 1;
    ovstage_filter_t filter{};
    filter.predicates = &pred;
    filter.count = 1;
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ASSERT_TRUE(waitOp(stage_, ovstage_query(stage_, &filter, nullptr, 0, &query)));

    path_dictionary_instance_t* dict = ovstage_get_path_dictionary(stage_);
    ASSERT_NE(dict, nullptr);
    ovx_string_t attrName = str("ovstageTest:strength");
    ovx_token_t attr = OVX_INVALID_TOKEN;
    ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict, &attrName, 1, &attr).status,
              OVX_API_SUCCESS);

    ovstage_ordinal_range_t range{};
    range.end_ordinal = 1;
    range.has_start_ordinal = false;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query, &attr, 1, range, &read)));

    ovstage_read_group_t group{};
    ASSERT_EQ(ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group), OVSTAGE_OK);
    ASSERT_EQ(group.data.tensor_count, 1u);
    ASSERT_NE(group.data.tensors[0].data, nullptr);
    float strength = NAN;
    std::memcpy(&strength, group.data.tensors[0].data, sizeof(strength));
    EXPECT_FLOAT_EQ(strength, 1.5f);

    ovstage_release_group(stage_, &group);
    waitOp(stage_, ovstage_release_read(stage_, read));
    waitOp(stage_, ovstage_release_query(stage_, query));
    std::filesystem::remove_all(root);
}

// Populate exactly the prims carrying a schema, with the properties that schema declares.
TEST_F(PopulationTest, PopulateBySchema)
{
    // [snippet:populate-by-schema-c]
    // A description says what to populate. The prim predicate picks the prims -- here
    // anything conforming to PhysicsRigidBodyAPI, whether as its type or as an applied
    // API schema -- and the property predicate picks which of their properties are
    // published. The same schema list serves both halves.
    //
    // A predicate references its values rather than copying them, so `schemas` must
    // outlive the call; the description itself need not outlive it.
    ovx_string_t schemas[] = { str("PhysicsRigidBodyAPI") };

    ovstage_population_selector_t selector = {};
    selector.prim_predicate = ovstage_population_prim_predicate_has_schema(schemas, 1);
    selector.property_predicate = ovstage_population_property_predicate_declared_by_schema(schemas, 1);

    ovstage_population_desc_t desc = {};
    desc.domains = OVSTAGE_POPULATION_DOMAIN_NONE;
    desc.selectors = &selector;
    desc.selector_count = 1;

    ovstage_population_enqueue_result_t pop = ovstage_population_open_usd_from_string_with_desc(
        stage_, str(kSchemaUsda), /*ordinal=*/1, /*time=*/0.0, &desc, 1);
    ASSERT_EQ(pop.status, OVSTAGE_OK);
    ASSERT_EQ(ovstage_population_wait_op(stage_, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, nullptr), OVSTAGE_OK);
    // [/snippet:populate-by-schema-c]

    EXPECT_EQ(prefixCount("/World/Body"), 1u);
    EXPECT_EQ(prefixCount("/World/Prop"), 0u);
}

// Predicates compose, so one selector can narrow a selection to a branch of the stage.
TEST_F(PopulationTest, PopulateNarrowedToSubtree)
{
    // [snippet:populate-narrowed-c]
    // Several values of one kind already mean "any of these", so AND is how conditions
    // of *different* kinds combine. A nested predicate is referenced, not copied, so
    // every node has to outlive the call -- which is what the named locals are for.
    ovx_string_t types[] = { str("Cube") };
    ovx_string_t roots[] = { str("/World") };
    ovstage_population_prim_predicate_t parts[2] = {
        ovstage_population_prim_predicate_has_type(types, 1),
        ovstage_population_prim_predicate_is_under_path(roots, 1),
    };
    ovx_string_t names[] = { str("size") };

    ovstage_population_selector_t selector = {};
    selector.prim_predicate = ovstage_population_prim_predicate_and(parts, 2);
    selector.property_predicate = ovstage_population_property_predicate_has_name(names, 1);

    ovstage_population_desc_t desc = {};
    desc.selectors = &selector;
    desc.selector_count = 1;

    ovstage_population_enqueue_result_t pop = ovstage_population_open_usd_from_string_with_desc(
        stage_, str(kSchemaUsda), /*ordinal=*/1, /*time=*/0.0, &desc, 1);
    ASSERT_EQ(pop.status, OVSTAGE_OK);
    ASSERT_EQ(ovstage_population_wait_op(stage_, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, nullptr), OVSTAGE_OK);
    // [/snippet:populate-narrowed-c]

    EXPECT_EQ(prefixCount("/World/Body"), 1u);
    EXPECT_EQ(prefixCount("/World/Prop"), 1u);

    EXPECT_EQ(prefixCount("/World/Ball"), 0u);
    EXPECT_EQ(prefixCount("/Outside"), 0u);
}
