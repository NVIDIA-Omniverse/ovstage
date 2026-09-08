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
// Public ovstage runtime-to-USD export contract. CPU-only.

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_population.h>
#include <ovstage/ovstage_population_export.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

template <std::size_t N>
constexpr ovx_string_t str(const char (&value)[N])
{
    return { value, N - 1 };
}

ovx_string_t str(std::string_view value)
{
    return { value.data(), value.size() };
}

bool waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enqueue, const char* what)
{
    if (enqueue.status != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " enqueue rejected (code " << enqueue.status << ")";
        return false;
    }
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t status =
        ovstage_wait_op(stage, enqueue.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    ovstage_release_op(stage, enqueue.op_index);
    if (status != OVSTAGE_OK || wait.error_op_id_count != 0)
    {
        ADD_FAILURE() << what << " failed (code " << status << ")";
        return false;
    }
    return true;
}

bool waitPopulation(ovstage_instance_t* stage, ovstage_population_enqueue_result_t enqueue, const char* what)
{
    if (enqueue.status != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " enqueue rejected (code " << enqueue.status << ")";
        return false;
    }
    const ovstage_api_status_t status =
        ovstage_population_wait_op(stage, enqueue.op_index, OVSTAGE_TIMEOUT_INFINITE, nullptr);
    if (status != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " failed (code " << status << ")";
        return false;
    }
    return true;
}

class StageOwner
{
public:
    explicit StageOwner(const char* name)
    {
        ovstage_instance_desc_t desc{};
        desc.name = name;
        EXPECT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
    }

    ~StageOwner()
    {
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    StageOwner(const StageOwner&) = delete;
    StageOwner& operator=(const StageOwner&) = delete;

    ovstage_instance_t* get() const
    {
        return stage_;
    }

private:
    ovstage_instance_t* stage_ = nullptr;
};

class TemporaryLayer
{
public:
    TemporaryLayer()
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ovstage-public-export-" + std::to_string(nonce) + ".usda");
        std::ofstream stream(path_, std::ios::binary);
        stream << "#usda 1.0\n";
        EXPECT_TRUE(stream.good());
        identifier_ = path_.string();
    }

    ~TemporaryLayer()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::string& identifier() const
    {
        return identifier_;
    }

private:
    std::filesystem::path path_;
    std::string identifier_;
};

struct RuntimeAttribute
{
    path_dictionary_instance_t* dictionary = nullptr;
    ovx_primpath_list_t paths = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ovx_token_t attribute = OVX_INVALID_TOKEN;
};

RuntimeAttribute writeFloat(
    ovstage_instance_t* stage,
    std::string_view primPath,
    std::string_view attributeName,
    float value,
    ovstage_ordinal_t ordinal)
{
    RuntimeAttribute out{};
    out.dictionary = ovstage_get_path_dictionary(stage);
    EXPECT_NE(out.dictionary, nullptr);

    const ovx_string_t path = str(primPath);
    EXPECT_EQ(path_dictionary_create_path_list_from_strings(out.dictionary, &path, 1, &out.paths).status,
              OVX_API_SUCCESS);
    EXPECT_EQ(ovstage_query_from_path_list(stage, out.paths, &out.query), OVSTAGE_OK);

    const ovx_string_t name = str(attributeName);
    EXPECT_EQ(path_dictionary_create_tokens_from_strings(out.dictionary, &name, 1, &out.attribute).status,
              OVX_API_SUCCESS);

    int64_t shape[] = { 1 };
    DLTensor tensor{};
    tensor.data = &value;
    tensor.device = { kDLCPU, 0 };
    tensor.ndim = 1;
    tensor.dtype = { kDLFloat, 32, 1 };
    tensor.shape = shape;

    ovstage_write_data_t write{};
    write.tensors = &tensor;
    write.tensor_count = 1;
    EXPECT_TRUE(waitOp(stage,
        ovstage_write_attribute(stage, out.query, { out.attribute, {} }, ordinal, write, OVSTAGE_PRIM_MODE_UPSERT),
        "write_attribute"));

    ovstage_write_floor_desc_t floor{};
    floor.ordinal = ordinal;
    floor.scope = OVSTAGE_SCOPE_ALL;
    EXPECT_TRUE(waitOp(stage, ovstage_advance_write_floor(stage, &floor), "advance_write_floor"));
    return out;
}

void releaseRuntimeAttribute(ovstage_instance_t* stage, RuntimeAttribute& attribute)
{
    if (attribute.query != OVSTAGE_INVALID_QUERY_HANDLE)
        EXPECT_TRUE(waitOp(stage, ovstage_release_query(stage, attribute.query), "release_query"));
    if (attribute.dictionary && attribute.paths != OVX_INVALID_PRIMPATH_LIST)
        EXPECT_EQ(path_dictionary_release_path_list_reference(attribute.dictionary, attribute.paths).status,
                  OVX_API_SUCCESS);
}

TEST(PopulationExportPublic, ExportTypedHierarchyToUsdFile)
{
    StageOwner runtime("test.ovstage.population-export.typed-source");
    ASSERT_NE(runtime.get(), nullptr);
    TemporaryLayer destination;

    // [snippet:export-typed-hierarchy-to-usd-c]
    constexpr char source[] = R"usd(#usda 1.0
def Xform "World"
{
    def Xform "Props"
    {
        def Sphere "Ball" (prepend apiSchemas = ["PhysicsCollisionAPI"])
        {
            double radius = 1.5
            bool physics:collisionEnabled = true
        }
        def Cube "Crate" (prepend apiSchemas = ["PhysicsRigidBodyAPI"])
        {
            double size = 2
            bool physics:rigidBodyEnabled = true
        }
    }
}
)usd";
    ASSERT_TRUE(waitPopulation(runtime.get(),
        ovstage_population_open_usd_from_string(
            runtime.get(), str(source), 1, NAN, OVSTAGE_POPULATION_DOMAIN_ALL),
        "open_usd_from_string"));
    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 1;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(runtime.get(), ovstage_advance_write_floor(runtime.get(), &floor), "advance_write_floor"));

    const ovx_string_t root = str("/World/Props");
    ovstage_population_export_desc_t desc{};
    ASSERT_EQ(ovstage_population_export_desc_init_typed_hierarchy(&desc, 1, &root), OVSTAGE_OK);

    ovstage_population_export_report_t report{};
    ASSERT_EQ(ovstage_population_export_to_usd_file(
                  runtime.get(), str(destination.identifier()), &desc, &report),
              OVSTAGE_OK);
    // [/snippet:export-typed-hierarchy-to-usd-c]

    EXPECT_GE(report.prims_exported, 3u);
    EXPECT_EQ(report.api_schemas_applied, 2u);
    EXPECT_GE(report.attributes_exported, 4u);
    std::ifstream stream(destination.identifier(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("def Sphere \"Ball\""), std::string::npos);
    EXPECT_NE(text.find("def Cube \"Crate\""), std::string::npos);
    EXPECT_NE(text.find("PhysicsCollisionAPI"), std::string::npos);
    EXPECT_NE(text.find("PhysicsRigidBodyAPI"), std::string::npos);
}

TEST(PopulationExportPublic, ExportSelectedAttributeToUsdFile)
{
    StageOwner runtime("test.ovstage.population-export.source");
    ASSERT_NE(runtime.get(), nullptr);
    RuntimeAttribute radius = writeFloat(runtime.get(), "/World/Ball", "radius", 2.5f, 1);

    TemporaryLayer destination;

    // The one-shot API creates an empty destination, exports, saves, and closes.
    const ovx_string_t includePath = str("/World/Ball");
    const ovx_string_t attributeName = str("radius");
    const ovstage_population_prim_predicate_t primPredicate =
        ovstage_population_prim_predicate_has_path(&includePath, 1);
    const ovstage_population_property_predicate_t propertyPredicate =
        ovstage_population_property_predicate_has_name(&attributeName, 1);

    ovstage_population_export_attribute_rule_t rule{};
    rule.path = includePath;
    rule.path_match = OVSTAGE_POPULATION_EXPORT_PATH_MATCH_EXACT;
    rule.source_attribute_name = attributeName;
    rule.source_attribute_name_match = OVSTAGE_POPULATION_EXPORT_NAME_MATCH_EXACT;
    rule.usd_type_name = str("float");
    rule.flags = OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED;

    ovstage_population_export_desc_t desc{};
    ASSERT_EQ(ovstage_population_export_desc_init_snapshot(&desc, 1), OVSTAGE_OK);
    desc.prim_predicate = primPredicate;
    desc.property_predicate = propertyPredicate;
    desc.attribute_rules = &rule;
    desc.attribute_rule_count = 1;

    ovstage_population_export_report_t report{};
    ASSERT_EQ(ovstage_population_export_to_usd_file(
                  runtime.get(), str(destination.identifier()), &desc, &report),
              OVSTAGE_OK);

    EXPECT_EQ(report.prims_exported, 1u);
    EXPECT_EQ(report.attributes_exported, 1u);
    EXPECT_GT(std::filesystem::file_size(destination.identifier()), 0u);
    releaseRuntimeAttribute(runtime.get(), radius);
}

TEST(PopulationExportPublic, AsyncExportCompletesWithReport)
{
    StageOwner runtime("test.ovstage.population-export.async-source");
    ASSERT_NE(runtime.get(), nullptr);
    RuntimeAttribute height = writeFloat(runtime.get(), "/World/Cube", "height", 4.0f, 1);

    TemporaryLayer destination;

    ovstage_population_export_attribute_rule_t rule{};
    rule.source_attribute_name = str("height");
    rule.usd_type_name = str("float");
    rule.flags = OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED;
    ovstage_population_export_desc_t desc{};
    ASSERT_EQ(ovstage_population_export_desc_init_snapshot(&desc, 1), OVSTAGE_OK);
    desc.attribute_rules = &rule;
    desc.attribute_rule_count = 1;

    // [snippet:export-runtime-to-usd-async-c]
    const ovstage_population_enqueue_result_t enqueue =
        ovstage_population_export_enqueue_to_usd_file(
            runtime.get(), str(destination.identifier()), &desc);
    ASSERT_EQ(enqueue.status, OVSTAGE_OK);

    ovstage_population_export_report_t report{};
    ASSERT_EQ(ovstage_population_export_wait_op(
                  runtime.get(), enqueue.op_index, OVSTAGE_TIMEOUT_INFINITE, &report),
              OVSTAGE_OK);
    // [/snippet:export-runtime-to-usd-async-c]

    EXPECT_EQ(report.attributes_exported, 1u);
    EXPECT_GT(std::filesystem::file_size(destination.identifier()), 0u);
    releaseRuntimeAttribute(runtime.get(), height);
}

} // namespace
