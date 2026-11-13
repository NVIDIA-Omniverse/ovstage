// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage map/unmap test: map_attribute prepares writable staging buffers
// you fill directly. Commit each group via unmap_group, or commit remaining groups
// in the final unmap. Covers existing and newly created columns. CPU-only.
// The write-flavors example is the workflow tour.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace
{

ovx_string_t str(const char* s)
{
    return ovx_string_t{ s, std::strlen(s) };
}

bool waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq, const char* what)
{
    if (enq.status != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " enqueue rejected (code " << enq.status << ")";
        return false;
    }
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t werr = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    for (size_t i = 0; i < wait.error_op_id_count; ++i)
    {
        const ovx_string_t e = ovstage_get_last_op_error(stage, wait.error_op_ids[i]);
        ADD_FAILURE() << what << " op failed: " << std::string(e.ptr ? e.ptr : "", e.ptr ? e.length : 0);
    }
    ovstage_release_op(stage, enq.op_index);
    if (werr != OVSTAGE_OK)
        ADD_FAILURE() << what << " wait failed (code " << werr << ")";
    return werr == OVSTAGE_OK && wait.error_op_id_count == 0;
}

class MapTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.map";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);
        const ovx_string_t paths[] = { str("/World/Mapped/A"), str("/World/Mapped/B") };
        ASSERT_EQ(path_dictionary_create_path_list_from_strings(dict_, paths, 2, &pathList_).status, OVX_API_SUCCESS);
        ASSERT_EQ(ovstage_query_from_path_list(stage_, pathList_, &query_), OVSTAGE_OK);
    }

    void TearDown() override
    {
        if (stage_ && query_ != OVSTAGE_INVALID_QUERY_HANDLE)
            waitOp(stage_, ovstage_release_query(stage_, query_), "release_query");
        if (dict_ && pathList_ != OVX_INVALID_PRIMPATH_LIST)
            path_dictionary_release_path_list_reference(dict_, pathList_);
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    void seal(ovstage_ordinal_t ordinal)
    {
        ovstage_write_floor_desc_t floor{};
        floor.ordinal = ordinal;
        floor.scope = OVSTAGE_SCOPE_ALL;
        ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));
    }

    // Read a fixed-lane column into out[list-index * lanes ...] in query order.
    void read(ovx_token_t attr, ovstage_ordinal_t endOrdinal, float* out, int lanes)
    {
        ovstage_ordinal_range_t range{};
        range.end_ordinal = endOrdinal;
        ovstage_read_handle_t r = OVSTAGE_INVALID_READ_HANDLE;
        ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &attr, 1, range, &r), "read"));
        for (;;)
        {
            size_t fetchedGroupCount = 0;
            ovstage_read_group_t group{};
            const ovstage_api_status_t fetched =
                ovstage_fetch_read_next(stage_, r, OVSTAGE_TIMEOUT_INFINITE, &group, 1, &fetchedGroupCount);
            if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
                break;
            ASSERT_EQ(fetched, OVSTAGE_OK);
            ASSERT_EQ(group.column_count, 1u);
            const ovstage_read_column_t& column = group.columns[0];
            if (!column.is_delete && column.data.tensor_count == 1 && column.data.tensors[0].data)
            {
                const float* rows = static_cast<const float*>(column.data.tensors[0].data);
                for (uint32_t local = 0; local < group.prims.count; ++local)
                {
                    const size_t listIndex = group.prims.index_map ? group.prims.index_map[local]
                                                                   : group.prims.offset + local;
                    const uint32_t row = column.data.index_map ? column.data.index_map[local] : local;
                    for (int l = 0; l < lanes; ++l)
                        out[listIndex * lanes + l] = rows[row * lanes + l];
                }
            }
            ovstage_release_groups(stage_, &group, 1);
        }
        waitOp(stage_, ovstage_release_read(stage_, r), "release_read");
    }

    static DLTensor tokenTensor(ovx_token_t* data, int64_t* shape)
    {
        DLTensor tensor{};
        tensor.data = data;
        tensor.device = { kDLCPU, 0 };
        tensor.ndim = 1;
        tensor.dtype = { kDLUInt, 64, 1 };
        tensor.shape = shape;
        return tensor;
    }

    // Prims matching one string predicate on the live stage.
    size_t countMatching(const char* attribute, ovstage_filter_op_t op, const char* value)
    {
        const ovx_string_t valueView = str(value);
        ovstage_predicate_t predicate{};
        predicate.attribute.string = str(attribute);
        predicate.op = op;
        predicate.values = &valueView;
        predicate.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &predicate;
        filter.count = 1;
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        if (!waitOp(stage_, ovstage_query(stage_, &filter, nullptr, 0, &query), "filter query"))
            return 0;
        ovstage_query_result_t result{};
        size_t count = 0;
        if (ovstage_fetch_query_result(stage_, query, OVSTAGE_TIMEOUT_INFINITE, &result) == OVSTAGE_OK)
        {
            count = result.total_prim_count;
            ovstage_release_query_result(stage_, &result);
        }
        waitOp(stage_, ovstage_release_query(stage_, query), "release_query");
        return count;
    }

    // Read a token column into out[list-index] in query order; an array column
    // yields each prim's set, sorted because tag order is not preserved.
    void readTokens(ovx_token_t attr, ovstage_ordinal_t endOrdinal, std::vector<std::vector<ovx_token_t>>& out)
    {
        out.assign(2, {});
        ovstage_ordinal_range_t range{};
        range.end_ordinal = endOrdinal;
        ovstage_read_handle_t r = OVSTAGE_INVALID_READ_HANDLE;
        ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &attr, 1, range, &r), "read tokens"));
        for (;;)
        {
            size_t fetchedGroupCount = 0;
            ovstage_read_group_t group{};
            const ovstage_api_status_t fetched =
                ovstage_fetch_read_next(stage_, r, OVSTAGE_TIMEOUT_INFINITE, &group, 1, &fetchedGroupCount);
            if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
                break;
            ASSERT_EQ(fetched, OVSTAGE_OK);
            // One attribute was requested, so each group carries exactly one column.
            ASSERT_EQ(group.column_count, 1u);
            const ovstage_read_column_t& column = group.columns[0];
            if (!column.is_delete && column.data.tensors)
            {
                for (uint32_t local = 0; local < group.prims.count; ++local)
                {
                    const size_t listIndex = group.prims.index_map ? group.prims.index_map[local]
                                                                   : group.prims.offset + local;
                    const uint32_t row = column.data.index_map ? column.data.index_map[local] : local;
                    if (column.is_array)
                    {
                        const DLTensor& tensor = column.data.tensors[row];
                        const auto* ids = static_cast<const ovx_token_t*>(tensor.data);
                        out[listIndex].assign(ids, ids + tensor.shape[0]);
                        std::sort(out[listIndex].begin(), out[listIndex].end());
                    }
                    else
                    {
                        out[listIndex] = { static_cast<const ovx_token_t*>(column.data.tensors[0].data)[row] };
                    }
                }
            }
            ovstage_release_groups(stage_, &group, 1);
        }
        waitOp(stage_, ovstage_release_read(stage_, r), "release_read");
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_primpath_list_t pathList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

TEST_F(MapTest, MapExistingAndFreshColumn)
{
    const ovx_string_t names[] = { str("map-existing"), str("map-fresh") };
    ovx_token_t tokens[2] = { OVX_INVALID_TOKEN, OVX_INVALID_TOKEN };
    ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, names, 2, tokens).status, OVX_API_SUCCESS);
    const ovx_token_t existing = tokens[0];
    const ovx_token_t fresh = tokens[1];

    float seed[] = { 1.0f, 2.0f };
    int64_t shape[] = { 2 };
    int64_t strides[] = { 1 };
    DLTensor seedTensor{ seed, { kDLCPU, 0 }, 1, { kDLFloat, 32, 1 }, shape, strides, 0 };
    ovstage_write_data_t seedWrite{};
    seedWrite.tensors = &seedTensor;
    seedWrite.tensor_count = 1;
    seedWrite.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { existing, {} }, 1, seedWrite, OVSTAGE_PRIM_MODE_UPSERT), "seed"));
    seal(1);

    // [snippet:map-unmap-cpu-c]
    // map_attribute prepares writable staging buffers for the session's ordinal.
    // Each buffer is WRITE-ONLY (uninitialized, not a view of current values),
    // so fill every element.
    // Commit per group via unmap_group (streaming), or commit remaining groups
    // with the final unmap_attribute, which also releases the session. CPU
    // unmaps pass a zero ovstage_cuda_sync_t (no GPU work to wait on).
    // [snippet:dlpack-map-existing-c]
    ovstage_map_desc_t existingDesc{};
    existingDesc.attribute = { existing, {} };
    existingDesc.prim_mode = OVSTAGE_PRIM_MODE_UPSERT;
    ovstage_map_handle_t map = OVSTAGE_INVALID_MAP_HANDLE;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_map_attribute(stage_, query_, &existingDesc, 2, nullptr, 0, &map), "map existing"));
    for (;;)
    {
        ovstage_map_group_t mapGroup{};
        const ovstage_api_status_t fetched = ovstage_fetch_map_next(stage_, map, OVSTAGE_TIMEOUT_INFINITE, &mapGroup);
        if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        ASSERT_EQ(fetched, OVSTAGE_OK);
        float* buffer = static_cast<float*>(mapGroup.data.tensors[0].data);
        for (uint32_t local = 0; local < mapGroup.prims.count; ++local)
        {
            const size_t listIndex = mapGroup.prims.index_map ? mapGroup.prims.index_map[local]
                                                              : mapGroup.prims.offset + local;
            const uint32_t row = mapGroup.data.index_map ? mapGroup.data.index_map[local] : local;
            buffer[row] = (listIndex == 0) ? 10.0f : 20.0f;
        }
        ASSERT_TRUE(waitOp(stage_, ovstage_unmap_group(stage_, map, &mapGroup, ovstage_cuda_sync_t{}), "unmap_group"));
    }
    ASSERT_TRUE(waitOp(stage_, ovstage_unmap_attribute(stage_, map, ovstage_cuda_sync_t{}), "unmap_attribute"));
    seal(2);
    // [/snippet:dlpack-map-existing-c]

    // A NEW column: the descriptor dtype (3-lane float32) defines it. No
    // unmap_group this time — the final unmap commits the remaining groups.
    ovstage_map_desc_t freshDesc{};
    freshDesc.attribute = { fresh, {} };
    freshDesc.dtype = DLDataType{ kDLFloat, 32, 3 };
    freshDesc.prim_mode = OVSTAGE_PRIM_MODE_UPSERT;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_map_attribute(stage_, query_, &freshDesc, 3, nullptr, 0, &map), "map fresh"));
    for (;;)
    {
        ovstage_map_group_t mapGroup{};
        const ovstage_api_status_t fetched = ovstage_fetch_map_next(stage_, map, OVSTAGE_TIMEOUT_INFINITE, &mapGroup);
        if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        ASSERT_EQ(fetched, OVSTAGE_OK);
        float* buffer = static_cast<float*>(mapGroup.data.tensors[0].data);
        for (uint32_t local = 0; local < mapGroup.prims.count; ++local)
        {
            const size_t listIndex = mapGroup.prims.index_map ? mapGroup.prims.index_map[local]
                                                              : mapGroup.prims.offset + local;
            const uint32_t row = mapGroup.data.index_map ? mapGroup.data.index_map[local] : local;
            const float base = (listIndex == 0) ? 1.0f : 4.0f;
            buffer[row * 3 + 0] = base;
            buffer[row * 3 + 1] = base + 1.0f;
            buffer[row * 3 + 2] = base + 2.0f;
        }
    }
    ASSERT_TRUE(waitOp(stage_, ovstage_unmap_attribute(stage_, map, ovstage_cuda_sync_t{}), "unmap_attribute"));
    seal(3);
    // [/snippet:map-unmap-cpu-c]

    float existingValues[2] = {};
    read(existing, 2, existingValues, 1);
    EXPECT_FLOAT_EQ(existingValues[0], 10.0f);
    EXPECT_FLOAT_EQ(existingValues[1], 20.0f);

    float freshValues[6] = {};
    read(fresh, 3, freshValues, 3);
    const float expected[6] = { 1, 2, 3, 4, 5, 6 };
    for (int i = 0; i < 6; ++i)
        EXPECT_FLOAT_EQ(freshValues[i], expected[i]);
}

// usd-prim-type, usd-schemas and usd-ancestor-types are lowered to the prim's
// type / applied-schema / ancestor-type tags rather than stored as value
// columns. A map of one of them stages interned token ids and its commit lowers
// them exactly like a copy-in write, so reads and filters observe the mapped
// value.
TEST_F(MapTest, MapReservedMetadataRoundTrip)
{
    const ovx_string_t names[] = { str("usd-prim-type"), str("usd-schemas"),       str("Mesh"),
                                   str("Sphere"),        str("PhysicsRigidBodyAPI"), str("PhysicsMassAPI"),
                                   str("PhysicsCollisionAPI") };
    ovx_token_t tokens[7] = {};
    ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, names, 7, tokens).status, OVX_API_SUCCESS);
    const ovx_token_t primType = tokens[0];
    const ovx_token_t schemas = tokens[1];
    const ovx_token_t mesh = tokens[2];
    const ovx_token_t sphere = tokens[3];
    const ovx_token_t rigidBody = tokens[4];
    const ovx_token_t mass = tokens[5];
    const ovx_token_t collision = tokens[6];

    // Seed both prims through the copy-in write: one row per prim for the fixed
    // usd-prim-type, one single-element tensor per prim for the ragged usd-schemas.
    ovx_token_t meshRows[2] = { mesh, mesh };
    int64_t fixedShape[] = { 2 };
    DLTensor meshTensor = tokenTensor(meshRows, fixedShape);
    ovstage_write_data_t typeWrite{};
    typeWrite.tensors = &meshTensor;
    typeWrite.tensor_count = 1;
    typeWrite.is_array = false;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { primType, {} }, 1, typeWrite, OVSTAGE_PRIM_MODE_UPSERT),
        "seed usd-prim-type"));
    ovx_token_t rigidRows[2] = { rigidBody, rigidBody };
    int64_t raggedShapes[2] = { 1, 1 };
    DLTensor rigidTensors[2] = { tokenTensor(&rigidRows[0], &raggedShapes[0]),
                                 tokenTensor(&rigidRows[1], &raggedShapes[1]) };
    ovstage_write_data_t schemaWrite{};
    schemaWrite.tensors = rigidTensors;
    schemaWrite.tensor_count = 2;
    schemaWrite.is_array = true;
    ASSERT_TRUE(waitOp(stage_,
        ovstage_write_attribute(stage_, query_, { schemas, {} }, 1, schemaWrite, OVSTAGE_PRIM_MODE_UPSERT),
        "seed usd-schemas"));
    seal(1);

    // [snippet:map-reserved-metadata-c]
    // Reserved metadata maps like any recorded column: the staging buffer holds
    // interned token ids (dtype {kDLUInt, 64, 1}, semantic NONE) and the commit
    // lowers them to the prim's tags. One id per prim retypes both prims ...
    ovstage_map_desc_t typeDesc{};
    typeDesc.attribute = { primType, {} };
    typeDesc.dtype = DLDataType{ kDLUInt, 64, 1 };
    typeDesc.prim_mode = OVSTAGE_PRIM_MODE_UPSERT;
    ovstage_map_handle_t map = OVSTAGE_INVALID_MAP_HANDLE;
    ASSERT_TRUE(waitOp(stage_, ovstage_map_attribute(stage_, query_, &typeDesc, 2, nullptr, 0, &map),
                       "map usd-prim-type"));
    for (;;)
    {
        ovstage_map_group_t mapGroup{};
        const ovstage_api_status_t fetched = ovstage_fetch_map_next(stage_, map, OVSTAGE_TIMEOUT_INFINITE, &mapGroup);
        if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        ASSERT_EQ(fetched, OVSTAGE_OK);
        auto* ids = static_cast<ovx_token_t*>(mapGroup.data.tensors[0].data);
        for (uint32_t local = 0; local < mapGroup.prims.count; ++local)
            ids[mapGroup.data.index_map ? mapGroup.data.index_map[local] : local] = sphere;
        ASSERT_TRUE(waitOp(stage_, ovstage_unmap_group(stage_, map, &mapGroup, ovstage_cuda_sync_t{}), "unmap_group"));
    }
    ASSERT_TRUE(waitOp(stage_, ovstage_unmap_attribute(stage_, map, ovstage_cuda_sync_t{}), "unmap_attribute"));

    // ... and one ragged row per prim, sized by element_sizes, replaces that
    // prim's whole applied-schema set (an empty row clears it).
    const size_t elementSizes[2] = { 1, 2 };
    ovstage_map_desc_t schemaDesc{};
    schemaDesc.attribute = { schemas, {} };
    schemaDesc.dtype = DLDataType{ kDLUInt, 64, 1 };
    schemaDesc.prim_mode = OVSTAGE_PRIM_MODE_UPSERT;
    ASSERT_TRUE(waitOp(stage_, ovstage_map_attribute(stage_, query_, &schemaDesc, 2, elementSizes, 2, &map),
                       "map usd-schemas"));
    for (;;)
    {
        ovstage_map_group_t mapGroup{};
        const ovstage_api_status_t fetched = ovstage_fetch_map_next(stage_, map, OVSTAGE_TIMEOUT_INFINITE, &mapGroup);
        if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
            break;
        ASSERT_EQ(fetched, OVSTAGE_OK);
        // Array map groups carry one tensor per prim in group order.
        for (uint32_t i = 0; i < mapGroup.data.tensor_count; ++i)
        {
            const size_t listIndex = mapGroup.prims.index_map ? mapGroup.prims.index_map[i] : mapGroup.prims.offset + i;
            auto* ids = static_cast<ovx_token_t*>(mapGroup.data.tensors[i].data);
            ids[0] = mass;
            if (listIndex == 1)
                ids[1] = collision;
        }
    }
    // The final unmap commits the remaining group and releases the session.
    ASSERT_TRUE(waitOp(stage_, ovstage_unmap_attribute(stage_, map, ovstage_cuda_sync_t{}), "unmap_attribute"));
    seal(2);
    // [/snippet:map-reserved-metadata-c]

    std::vector<std::vector<ovx_token_t>> types;
    readTokens(primType, 2, types);
    EXPECT_EQ(types[0], (std::vector<ovx_token_t>{ sphere }));
    EXPECT_EQ(types[1], (std::vector<ovx_token_t>{ sphere }));
    std::vector<std::vector<ovx_token_t>> applied;
    readTokens(schemas, 2, applied);
    EXPECT_EQ(applied[0], (std::vector<ovx_token_t>{ mass }));
    std::vector<ovx_token_t> expectedSecond{ mass, collision };
    std::sort(expectedSecond.begin(), expectedSecond.end());
    EXPECT_EQ(applied[1], expectedSecond);

    EXPECT_EQ(countMatching("usd-prim-type", OVSTAGE_FILTER_OP_IN, "Sphere"), 2u);
    EXPECT_EQ(countMatching("usd-prim-type", OVSTAGE_FILTER_OP_IN, "Mesh"), 0u);
    EXPECT_EQ(countMatching("usd-schemas", OVSTAGE_FILTER_OP_CONTAINS, "PhysicsRigidBodyAPI"), 0u);
    EXPECT_EQ(countMatching("usd-schemas", OVSTAGE_FILTER_OP_CONTAINS, "PhysicsCollisionAPI"), 1u);
}

} // namespace
