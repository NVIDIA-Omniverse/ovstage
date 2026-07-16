// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage map/unmap test: map_attribute reserves writable storage-side
// buffers you fill directly (the zero-copy programming model), committed per group
// via unmap_group or all at once by the final unmap. Covers an existing column and
// a freshly-created one. CPU-only. The write-flavors example is the workflow tour.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <cstdint>
#include <cstring>

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
            ovstage_read_group_t group{};
            const ovstage_api_status_t fetched = ovstage_fetch_read_next(stage_, r, OVSTAGE_TIMEOUT_INFINITE, &group);
            if (fetched == OVSTAGE_ERROR_END_OF_ITERATION)
                break;
            ASSERT_EQ(fetched, OVSTAGE_OK);
            if (!group.is_delete && group.data.tensor_count == 1 && group.data.tensors[0].data)
            {
                const float* rows = static_cast<const float*>(group.data.tensors[0].data);
                for (uint32_t local = 0; local < group.prims.count; ++local)
                {
                    const size_t listIndex = group.prims.index_map ? group.prims.index_map[local]
                                                                   : group.prims.offset + local;
                    const uint32_t row = group.data.index_map ? group.data.index_map[local] : local;
                    for (int l = 0; l < lanes; ++l)
                        out[listIndex * lanes + l] = rows[row * lanes + l];
                }
            }
            ovstage_release_group(stage_, &group);
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
    // map_attribute reserves storage-side buffers you fill directly. The mapped
    // buffer is a WRITE-ONLY staging buffer for the session's ordinal
    // (uninitialized, not a view of current values), so fill every element.
    // Commit per group via unmap_group (streaming) or all at once with the final
    // unmap_attribute, which also releases the session. CPU unmaps pass a zero
    // ovstage_cuda_sync_t (no GPU work to wait on).
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

    // A NEW column: the descriptor dtype (3-lane float32) defines it. No
    // unmap_group this time — the final unmap commits everything.
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

} // namespace
