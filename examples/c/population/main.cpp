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
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include "../common/ovstage_example_utils.h"

// Run the population and readback sequence against scene.usda.
int main(int argc, char** argv)
{
    // [snippet:population-setup-c]
    const std::string scene = argc > 1 ? argv[1] : "scene.usda";
    const ovx_string_t scenePath{scene.c_str(), scene.size()};
    ovstage_instance_desc_t instanceDesc{};
    instanceDesc.name = "example.population";
    ovstage_instance_t* stage = nullptr;
    check(nullptr, ovstage_create_instance(&instanceDesc, &stage), "create_instance");
    auto* dict = getPathDictionary(stage);

    // Select physics data and wait for population before making it readable.
    waitPop(stage, ovstage_population_open_usd_from_file(
        stage, scenePath, 1, OVSTAGE_POPULATION_DEFAULT_TIME,
        OVSTAGE_POPULATION_DOMAIN_PHYSICS), "open_usd");
    ovstage_write_floor_desc_t watermark{};
    watermark.scope = OVSTAGE_SCOPE_ALL;
    watermark.ordinal = 1;
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
    // Keep stage and dict alive until all examples finish.
    // [/snippet:population-setup-c]

    // [snippet:population-paths-c]
    const ovx_string_t bodyPath[] = {literal_to_ovx_string("/World/Body")};
    // Match this exact prim; is_under_path would select a subtree instead.
    auto bodyCondition = ovstage_population_prim_predicate_has_path(bodyPath, 1);
    // [/snippet:population-paths-c]

    // [snippet:population-property-metadata-c]
    const ovx_string_t primMetadata[] = {literal_to_ovx_string("customData:label")};
    const ovx_string_t propertyMetadata[] = {literal_to_ovx_string("customData:unit")};
    ovstage_population_selector_t selector{};
    selector.prim_predicate = bodyCondition;
    const ovx_string_t properties[] = {
        literal_to_ovx_string("size"), literal_to_ovx_string("temperature"), literal_to_ovx_string("state"),
        literal_to_ovx_string("points"), literal_to_ovx_string("emptyName"),
        literal_to_ovx_string("emptyArray"), literal_to_ovx_string("extent")};
    selector.property_predicate = ovstage_population_property_predicate_has_name(properties, 7);
    selector.prim_metadata_paths = primMetadata;
    selector.prim_metadata_path_count = 1;
    selector.property_metadata_paths = propertyMetadata;
    selector.property_metadata_path_count = 1;
    // [/snippet:population-property-metadata-c]

    // [snippet:population-stage-metadata-c]
    const ovx_string_t stageMetadata[] = {literal_to_ovx_string("timeCodesPerSecond")};
    ovstage_population_desc_t desc{};
    desc.domains = OVSTAGE_POPULATION_DOMAIN_NONE;
    desc.selectors = &selector;
    desc.selector_count = 1;
    desc.stage_metadata_paths = stageMetadata;
    desc.stage_metadata_path_count = 1;
    // Population copies the description and the referenced arrays during submission.
    waitPop(stage, ovstage_population_open_usd_from_file_with_desc(
        stage, scenePath, 2, OVSTAGE_POPULATION_DEFAULT_TIME, &desc, 1), "open with description");
    watermark.ordinal = 2;
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
    // [/snippet:population-stage-metadata-c]

    // [snippet:population-default-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("size");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const double*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        std::printf("%.1f\n", static_cast<double>(values[0]));
        // 2.0
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-default-c]

    // [snippet:population-numeric-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("temperature");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const float*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        std::printf("%.1f\n", static_cast<double>(values[0]));
        // 20.0
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-numeric-c]

    // [snippet:population-strings-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("state");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const uint64_t*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        ovx_token_t token = values[0];
        ovx_string_t text{};
        checkOvx(dict, path_dictionary_get_strings_from_tokens(dict, &token, 1, &text), "resolve token");
        std::printf("%.*s\n", static_cast<int>(text.length), text.ptr);
        // ready
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-strings-c]

    // [snippet:population-arrays-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("points");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        // The tensor has shape (2, 3): two points with scalar float32 lanes.
        std::printf("points: %lld, lanes: %u\n", static_cast<long long>(tensor.shape[0]), tensor.dtype.lanes);
        // points: 2, lanes: 1
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-arrays-c]

    // [snippet:population-empty-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("emptyName");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const uint64_t*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        std::printf("%llu\n", static_cast<unsigned long long>(values[0]));
        // 0
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-empty-c]

    // [snippet:population-empty-array-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("emptyArray");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        // The group is present even though its array has no elements.
        std::printf("elements: %lld\n", static_cast<long long>(tensor.shape[0]));
        // elements: 0
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-empty-array-c]

    // [snippet:population-extent-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("extent");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        // extent is one fixed-size value containing six float64 components.
        std::printf("bits: %u, lanes: %u, array: %d\n", tensor.dtype.bits, tensor.dtype.lanes, group.is_array);
        // bits: 64, lanes: 1, array: 0
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-extent-c]

    // [snippet:population-stage-metadata-read-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("usd-metadata:timeCodesPerSecond");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const double*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        std::printf("%.1f\n", values[0]);
        // 24.0
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-stage-metadata-read-c]

    // [snippet:population-property-metadata-read-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("temperature:usd-metadata:customData:unit");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const uint64_t*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        ovx_token_t token = values[0];
        ovx_string_t text{};
        checkOvx(dict, path_dictionary_get_strings_from_tokens(dict, &token, 1, &text), "resolve metadata");
        std::printf("%.*s\n", static_cast<int>(text.length), text.ptr);
        // C
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-property-metadata-read-c]

    // [snippet:population-prim-metadata-read-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("usd-metadata:customData:label");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const uint64_t*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        ovx_token_t token = values[0];
        ovx_string_t text{};
        checkOvx(dict, path_dictionary_get_strings_from_tokens(dict, &token, 1, &text), "resolve metadata");
        std::printf("%.*s\n", static_cast<int>(text.length), text.ptr);
        // robot
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-prim-metadata-read-c]

    // [snippet:population-reserved-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("usd-prim-type");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const uint64_t*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        ovx_token_t token = values[0];
        ovx_string_t text{};
        checkOvx(dict, path_dictionary_get_strings_from_tokens(dict, &token, 1, &text), "resolve type");
        std::printf("%.*s\n", static_cast<int>(text.length), text.ptr);
        // Cube
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-reserved-c]

    // [snippet:population-schemas-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("usd-schemas");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const uint64_t*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        // One token per applied API schema, stored in a USD array.
        for (int64_t i = 0; i < tensor.shape[0]; ++i)
        {
            ovx_token_t token = values[i];
            ovx_string_t text{};
            checkOvx(dict, path_dictionary_get_strings_from_tokens(dict, &token, 1, &text), "resolve type");
            std::printf("%.*s\n", static_cast<int>(text.length), text.ptr);
            // PhysicsRigidBodyAPI
        }
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-schemas-c]

    // [snippet:population-ancestor-c]
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("omni:xform");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 2;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const double*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        // Row-major matrices store translation in the last row.
        std::printf("World translation: %.1f %.1f %.1f\n", values[12], values[13], values[14]);
        // World translation: 10.0 0.0 0.0
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-ancestor-c]

    // [snippet:population-time-c]
    // Seconds are converted using timeCodesPerSecond=24, independently of ordinal 3.
    waitPop(stage, ovstage_population_apply_usd_time(stage, 3, 1.0), "sample one second");
    watermark.ordinal = 3;
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
    {
        const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
        ovx_primpath_list_t pathList{};
        checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
        ovstage_query_handle_t query{};
        check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
        const ovx_string_t name = literal_to_ovx_string("temperature");
        ovx_token_t attribute{};
        checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 3;
        ovstage_read_handle_t read{};
        waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
        ovstage_read_group_t group{};
        check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
        const DLTensor& tensor = group.data.tensors[0];
        const auto* values = reinterpret_cast<const float*>(
            static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
        std::printf("%.1f\n", static_cast<double>(values[0]));
        // 30.0
        // Release the borrowed data before releasing the read and query.
        ovstage_release_group(stage, &group);
        waitOp(stage, ovstage_release_read(stage, read), "release read");
        waitOp(stage, ovstage_release_query(stage, query), "release query");
        path_dictionary_release_path_list_reference(dict, pathList);
    }
    // [/snippet:population-time-c]

    // [snippet:population-ordinals-c]
    // Start after the three ordinals already used in this example.
    for (int frame = 0; frame < 3; ++frame)
    {
        const ovstage_ordinal_t ordinal = 4 + frame;
        waitPop(stage, ovstage_population_apply_usd_time(stage, ordinal, frame / 24.0), "sample frame");
        watermark.ordinal = ordinal;
        waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
        // Consumers can now read through this ordinal before the next update.
    }
    // [/snippet:population-ordinals-c]


    // [snippet:population-subtree-c]
    // Add size for Cube prims below /World to the predefined physics selection.
    const ovx_string_t roots[] = {literal_to_ovx_string("/World")};
    const ovx_string_t types[] = {literal_to_ovx_string("Cube")};
    const ovx_string_t subtreeProperties[] = {literal_to_ovx_string("size")};
    const ovstage_population_prim_predicate_t conditions[] = {
        ovstage_population_prim_predicate_is_under_path(roots, 1),
        ovstage_population_prim_predicate_has_type(types, 1)};
    ovstage_population_selector_t subtreeSelector{};
    subtreeSelector.prim_predicate = ovstage_population_prim_predicate_and(conditions, 2);
    subtreeSelector.property_predicate = ovstage_population_property_predicate_has_name(subtreeProperties, 1);
    ovstage_population_desc_t subtreeDesc{};
    subtreeDesc.domains = OVSTAGE_POPULATION_DOMAIN_PHYSICS;
    subtreeDesc.selectors = &subtreeSelector;
    subtreeDesc.selector_count = 1;
    waitPop(stage, ovstage_population_open_usd_from_file_with_desc(
        stage, scenePath, 7, OVSTAGE_POPULATION_DEFAULT_TIME, &subtreeDesc, 1), "populate subtree");
    watermark.ordinal = 7;
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
    // [/snippet:population-subtree-c]
    ovstage_destroy_instance(stage);

    // A separate stage isolates the deliberate load failure.
    check(nullptr, ovstage_create_instance(&instanceDesc, &stage), "create failure stage");
    // [snippet:population-failure-c]
    const std::string missing = scene + ".missing";
    const ovstage_ordinal_t ordinal = 1;
    ovstage_ordinal_t retryOrdinal = ordinal;
    const auto operation = ovstage_population_open_usd_from_file(
        stage, {missing.c_str(), missing.size()}, ordinal,
        OVSTAGE_POPULATION_DEFAULT_TIME, OVSTAGE_POPULATION_DOMAIN_PHYSICS);
    if (operation.status == OVSTAGE_OK)
    {
        ovstage_population_op_wait_result_t result{};
        const auto status = ovstage_population_wait_op(
            stage, operation.op_index, OVSTAGE_TIMEOUT_INFINITE, &result);
        // A successful wait can still report a failed operation.
        if (status == OVSTAGE_OK && result.error_op_id_count == 0)
        {
            std::fprintf(stderr, "Expected the missing-file load to fail\n");
            ovstage_destroy_instance(stage);
            return EXIT_FAILURE;
        }
        retryOrdinal = ordinal + 1; // accepted: allow for partial writes
    }
    // Submission rejection leaves retryOrdinal unchanged: no writes ran.
    // Read the diagnostic before the retry replaces it.
    const ovx_string_t error = ovstage_population_get_last_error();
    std::fprintf(stderr, "Population failed: %.*s\n", static_cast<int>(error.length), error.ptr);
    // Population failed: <diagnostic for the missing file>
    waitPop(stage, ovstage_population_open_usd_from_file(
        stage, scenePath, retryOrdinal, OVSTAGE_POPULATION_DEFAULT_TIME,
        OVSTAGE_POPULATION_DOMAIN_PHYSICS), "retry with valid file");
    watermark.ordinal = retryOrdinal;
    waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
    // [/snippet:population-failure-c]
    ovstage_destroy_instance(stage);
}
