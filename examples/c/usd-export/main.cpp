// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
// property and proprietary rights in and to this material, related
// documentation and any modifications thereto. Any use, reproduction,
// disclosure or distribution of this material and related documentation
// without an express license agreement from NVIDIA CORPORATION or
// its affiliates is strictly prohibited.

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_population.h>
#include <ovstage/ovstage_population_export.h>
#include <ovx/types.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "../common/ovstage_example_utils.h"

// Create the source stage used by the export walkthrough.
static ovstage_instance_t* createStage(const char* name)
{
    ovstage_instance_desc_t desc{};
    desc.name = name;
    ovstage_instance_t* stage = nullptr;
    check(nullptr, ovstage_create_instance(&desc, &stage), "create_instance");
    return stage;
}


// Demonstrate rule-based export on a single runtime-created prim.
static void exportRuntimeData(const std::string& prefix)
{
    // [snippet:export-example-setup-c]
    ovstage_instance_t* stage = createStage("example.usd-export.runtime");
    auto* dict = getPathDictionary(stage);
    // [snippet:export-write-runtime-c]
    const ovx_string_t selected[] = {literal_to_ovx_string("/World/Ball")};
    ovx_primpath_list_t paths{};
    checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &paths), "paths");
    ovstage_query_handle_t query{};
    check(stage, ovstage_query_from_path_list(stage, paths, &query), "query");

    const ovx_string_t names[] = {literal_to_ovx_string("omni:xform"),
        literal_to_ovx_string("userProperties:temperature"), literal_to_ovx_string("solver:collisionRadius"),
        literal_to_ovx_string("usd-prim-type"), literal_to_ovx_string("Sphere")};
    ovx_token_t tokens[5]{};
    checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, names, 5, tokens), "tokens");
    int64_t shape[] = {1};
    double matrix[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1};
    float temperature = 20;
    double radius = 2.5;
    uint64_t sphereType = tokens[4];
    DLTensor tensors[4]{};
    void* values[] = {matrix, &temperature, &radius, &sphereType};
    const DLDataType types[] = {{kDLFloat, 64, 16}, {kDLFloat, 32, 1},
                                {kDLFloat, 64, 1}, {kDLUInt, 64, 1}};
    // Four fixed-size attributes on one prim. Record the type for later DEF exports.
    for (int i = 0; i < 4; ++i)
    {
        tensors[i].data = values[i];
        tensors[i].device = {kDLCPU, 0};
        tensors[i].ndim = 1;
        tensors[i].shape = shape;
        tensors[i].dtype = types[i];
        auto write = writeData(&tensors[i], i == 0 ? OVSTAGE_SEMANTIC_MATRIX : OVSTAGE_SEMANTIC_NONE);
        waitOp(stage, ovstage_write_attribute(stage, query, {tokens[i], {}}, 1, write,
                                              OVSTAGE_PRIM_MODE_UPSERT), "write");
    }
    ovstage_write_floor_desc_t watermark{};
    watermark.ordinal = 1;
    watermark.scope = OVSTAGE_SCOPE_ALL;
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
    // [/snippet:export-write-runtime-c]
    // Keep this source stage, query, and dictionary alive for the remaining examples.
    // [/snippet:export-example-setup-c]

    // [snippet:export-file-c]
    ovstage_population_export_attribute_rule_t transformRule{};
    transformRule.source_attribute_name = literal_to_ovx_string("omni:xform");
    ovstage_population_export_desc_t transformDesc{};
    check(stage, ovstage_population_export_desc_init_snapshot(&transformDesc, 1), "init export");
    transformDesc.attribute_rules = &transformRule;
    transformDesc.attribute_rule_count = 1;
    const std::string transformFile = prefix + "transforms.usda";
    ovstage_population_export_report_t report{};
    // One-shot export creates, saves, and closes the overlay layer.
    check(stage, ovstage_population_export_to_usd_file(stage,
        {transformFile.c_str(), transformFile.size()}, &transformDesc, &report), "export transforms");
    std::printf("Transforms exported: %zu\n", report.transforms_exported);
    // Transforms exported: 1
    // [/snippet:export-file-c]

    // [snippet:export-lifetime-c]
    const std::string discardedFile = prefix + "discarded.usda";
    ovstage_population_export_destination_handle_t discarded{};
    check(stage, ovstage_population_export_destination_create(stage,
        {discardedFile.c_str(), discardedFile.size()}, OVSTAGE_POPULATION_EXPORT_DESTINATION_EMPTY,
        &discarded), "create destination");
    check(stage, ovstage_population_export_to_destination(stage, discarded, &transformDesc, nullptr), "export");
    // No save: destroy discards this export. Do this before destroying the source stage.
    check(stage, ovstage_population_export_destination_destroy(stage, discarded), "close destination");
    // [/snippet:export-lifetime-c]

    // [snippet:export-reuse-c]
    const std::string combinedFile = prefix + "combined.usda";
    ovstage_population_export_destination_handle_t usd{};
    check(stage, ovstage_population_export_destination_create(stage,
        {combinedFile.c_str(), combinedFile.size()}, OVSTAGE_POPULATION_EXPORT_DESTINATION_EMPTY,
        &usd), "create destination");
    check(stage, ovstage_population_export_to_destination(stage, usd, &transformDesc, nullptr), "export transform");
    ovstage_population_export_attribute_rule_t temperatureRule{};
    temperatureRule.source_attribute_name = literal_to_ovx_string("userProperties:temperature");
    temperatureRule.usd_type_name = literal_to_ovx_string("float");
    ovstage_population_export_desc_t temperatureDesc{};
    check(stage, ovstage_population_export_desc_init_snapshot(&temperatureDesc, 1), "init export");
    temperatureDesc.attribute_rules = &temperatureRule;
    temperatureDesc.attribute_rule_count = 1;
    check(stage, ovstage_population_export_to_destination(stage, usd, &temperatureDesc, nullptr), "export temperature");
    // Persist both exports, then release the destination. Save itself leaves it open.
    check(stage, ovstage_population_export_destination_save(stage, usd), "save");
    check(stage, ovstage_population_export_destination_destroy(stage, usd), "close destination");
    // [/snippet:export-reuse-c]

    // [snippet:export-custom-c]
    ovstage_population_export_attribute_rule_t customRule{};
    customRule.source_attribute_name = literal_to_ovx_string("userProperties:*");
    customRule.source_attribute_name_match = OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB;
    // An empty destination name preserves each matched source name.
    customRule.flags = OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE | OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED;
    ovstage_population_export_desc_t customDesc{};
    check(stage, ovstage_population_export_desc_init_snapshot(&customDesc, 1), "init export");
    customDesc.attribute_rules = &customRule;
    customDesc.attribute_rule_count = 1;
    const std::string customFile = prefix + "custom.usda";
    check(stage, ovstage_population_export_to_usd_file(stage,
        {customFile.c_str(), customFile.size()}, &customDesc, nullptr), "export custom attributes");
    // [/snippet:export-custom-c]

    // [snippet:export-rule-c]
    ovstage_population_export_attribute_rule_t radiusRule{};
    radiusRule.source_attribute_name = literal_to_ovx_string("solver:collisionRadius");
    radiusRule.destination_attribute_name = literal_to_ovx_string("radius");
    radiusRule.usd_type_name = literal_to_ovx_string("double");
    radiusRule.flags = OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED;
    // [/snippet:export-rule-c]

    // [snippet:export-rename-c]
    ovstage_population_export_desc_t radiusDesc{};
    check(stage, ovstage_population_export_desc_init_snapshot(&radiusDesc, 1), "init export");
    radiusDesc.layer_mode = OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF;
    radiusDesc.attribute_rules = &radiusRule;
    radiusDesc.attribute_rule_count = 1;
    const std::string sphereFile = prefix + "sphere.usda";
    check(stage, ovstage_population_export_to_usd_file(stage,
        {sphereFile.c_str(), sphereFile.size()}, &radiusDesc, nullptr), "export Sphere radius");
    // The recorded usd-prim-type creates a Sphere with radius 2.5.
    // [/snippet:export-rename-c]

    // [snippet:export-selection-c]
    auto selectedDesc = temperatureDesc; // Explicit rule: temperature becomes USD float.
    const ovx_string_t selectedPath[] = {literal_to_ovx_string("/World/Ball")};
    const ovx_string_t namespaces[] = {literal_to_ovx_string("userProperties")};
    selectedDesc.prim_predicate = ovstage_population_prim_predicate_has_path(selectedPath, 1);
    selectedDesc.property_predicate = ovstage_population_property_predicate_in_namespace(namespaces, 1);
    const std::string selectedFile = prefix + "selected.usda";
    check(stage, ovstage_population_export_to_usd_file(stage,
        {selectedFile.c_str(), selectedFile.size()}, &selectedDesc, nullptr), "export selected data");
    // [/snippet:export-selection-c]

    // [snippet:export-matrix-c]
    auto matrixDesc = transformDesc; // Selects omni:xform at ordinal 1.
    matrixDesc.layer_mode = OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF;
    matrixDesc.transform_policy = OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_MATRIX_OP;
    const std::string matrixFile = prefix + "matrix.usda";
    check(stage, ovstage_population_export_to_usd_file(stage,
        {matrixFile.c_str(), matrixFile.size()}, &matrixDesc, nullptr), "export matrix");
    // Creates a Sphere with a matrix xform operation instead of decomposed operations.
    // [/snippet:export-matrix-c]

    // [snippet:export-report-c]
    // report and transformDesc come from the earlier transform export.
    std::printf("Attributes exported: %zu\n", report.attributes_exported);
    // Attributes exported: 1
    auto staleDesc = transformDesc;
    staleDesc.ordinal = 0; // Current watermark is 1: this cannot select an older snapshot.
    const std::string staleFile = prefix + "stale.usda";
    ovstage_population_export_report_t failureReport{};
    const auto status = ovstage_population_export_to_usd_file(stage,
        {staleFile.c_str(), staleFile.size()}, &staleDesc, &failureReport);
    if (status == OVSTAGE_OK || failureReport.source_states_unavailable == 0)
        std::exit(EXIT_FAILURE);
    std::printf("Stale export rejected\n");
    // Stale export rejected
    // [/snippet:export-report-c]

    // [snippet:export-async-c]
    const std::string asyncFile = prefix + "async.usda";
    const auto operation = ovstage_population_export_enqueue_to_usd_file(stage,
        {asyncFile.c_str(), asyncFile.size()}, &transformDesc);
    check(stage, operation.status, "enqueue export");
    ovstage_population_export_report_t asyncReport{};
    // Use the export-specific wait; completion includes saving the file.
    check(stage, ovstage_population_export_wait_op(stage, operation.op_index,
        OVSTAGE_TIMEOUT_INFINITE, &asyncReport), "wait export");
    std::printf("Async transforms exported: %zu\n", asyncReport.transforms_exported);
    // Async transforms exported: 1
    // [/snippet:export-async-c]

    // [snippet:export-changed-c]
    // custom.usda already holds the baseline temperature 20 at ordinal 1.
    // tensors[1] points to temperature; tokens[1] names userProperties:temperature.
    // query still selects the single prim /World/Ball.
    temperature = 24.5f;
    waitOp(stage, ovstage_write_attribute(stage, query, {tokens[1], {}}, 2,
        writeData(&tensors[1]), OVSTAGE_PRIM_MODE_UPSERT), "update temperature");
    watermark.ordinal = 2;
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
    auto changedDesc = temperatureDesc;
    changedDesc.ordinal = 2;
    changedDesc.since_ordinal = 1;
    changedDesc.selection = OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL;
    const std::string changedFile = prefix + "changed.usda";
    ovstage_population_export_report_t changed{};
    check(stage, ovstage_population_export_to_usd_file(stage,
        {changedFile.c_str(), changedFile.size()}, &changedDesc, &changed), "export changes");
    std::printf("Changed attributes exported: %zu\n", changed.attributes_exported);
    // Changed attributes exported: 1
    // [/snippet:export-changed-c]

    waitOp(stage, ovstage_release_query(stage, query), "release query");
    checkOvx(dict, path_dictionary_release_path_list_reference(dict, paths), "release paths");
    check(nullptr, ovstage_destroy_instance(stage), "destroy instance");
}

// Export the populated hierarchy, then run the direct-write examples.
int main(int argc, char** argv)
{
    const char* destination = argc > 1 ? argv[1] : "destination.usda";
    ovstage_instance_t* runtime = createStage("example.usd-export.source");

    const char* source = R"usd(#usda 1.0
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
    def Xform "SimulatedMeshes"
    {
        def Mesh "Floor" (prepend apiSchemas = ["PhysicsCollisionAPI"])
        {
            point3f[] points = [(0, 0, 0), (2, 0, 0), (0, 2, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
        }
        def Mesh "Dynamic" (prepend apiSchemas = ["PhysicsCollisionAPI", "PhysicsRigidBodyAPI"])
        {
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
        }
    }
    def Sphere "OutsideSelection"
    {
        double radius = 10
    }
}
)usd";
    waitPop(runtime,
            ovstage_population_open_usd_from_string(
                runtime, { source, std::strlen(source) }, 1, NAN, OVSTAGE_POPULATION_DOMAIN_ALL),
            "open_usd_from_string");

    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 1;
    floor.scope = OVSTAGE_SCOPE_ALL;
    waitOp(runtime, ovstage_advance_write_floor(runtime, &floor), "advance_write_floor");

    // [snippet:typed-hierarchy-c-example]
    const ovx_string_t root = literal_to_ovx_string("/World/Props");
    ovstage_population_export_desc_t exportDesc{};
    check(runtime,
          ovstage_population_export_desc_init_typed_hierarchy(&exportDesc, 1, &root),
          "export_desc_init_typed_hierarchy");

    ovstage_population_export_report_t report{};
    const ovx_string_t destinationId{ destination, std::strlen(destination) };
    check(runtime,
          ovstage_population_export_to_usd_file(runtime, destinationId, &exportDesc, &report),
          "export_to_usd_file");

    std::ifstream output(destination, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(output)), std::istreambuf_iterator<char>());
    if (text.find("def Sphere \"Ball\"") == std::string::npos ||
        text.find("def Cube \"Crate\"") == std::string::npos ||
        text.find("OutsideSelection") != std::string::npos)
    {
        std::fprintf(stderr, "the exported USD file does not contain the selected typed hierarchy\n");
        std::exit(EXIT_FAILURE);
    }
    // [/snippet:typed-hierarchy-c-example]

    // [snippet:export-mesh-c]
    const ovx_string_t meshRoot = literal_to_ovx_string("/World/SimulatedMeshes");
    ovstage_population_export_desc_t meshDesc{};
    check(runtime, ovstage_population_export_desc_init_typed_hierarchy(&meshDesc, 1, &meshRoot), "init mesh export");
    const std::string meshFile = std::string(destination) + ".meshes.usda";
    ovstage_population_export_report_t meshReport{};
    check(runtime, ovstage_population_export_to_usd_file(runtime,
        {meshFile.c_str(), meshFile.size()}, &meshDesc, &meshReport), "export meshes");
    std::printf("Mesh APIs exported: %zu\n", meshReport.api_schemas_applied);
    // Mesh APIs exported: 5
    // [/snippet:export-mesh-c]

    std::printf("saved /World/Props as typed USD to %s\n", destination);
    // saved /World/Props as typed USD to <destination path>
    std::printf("report: %llu prims, %llu attributes, %llu applied schemas\n",
                static_cast<unsigned long long>(report.prims_exported),
                static_cast<unsigned long long>(report.attributes_exported),
                static_cast<unsigned long long>(report.api_schemas_applied));
    // report: 7 prims, 4 attributes, 4 applied schemas
    ovstage_destroy_instance(runtime);
    exportRuntimeData(std::string(destination) + ".");
    return 0;
}
