// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage device-placement test: a write may name which device's copy of
// an attribute it produces. CPU-only -- every case either uses a CPU placement
// or asserts the rejection of one this build cannot serve, so the file runs on a
// host with no CUDA device.

#include <ovstage/ovstage.h>
#include <ovstage/ovstage_api/experimental/ovstage_placement_extension.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

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

// ovstage_placement_t carries no size or version field, so its layout *is* its
// ABI (a change to it bumps OVSTAGE_PLACEMENT_EXTENSION_VERSION), and the Python
// ctypes mirror asserts the same numbers from the other side. Pinning both ends
// is what makes a member added to one of them fail loudly instead of letting the
// native side read past what the other allocated.
static_assert(offsetof(ovstage_placement_t, device) == 0, "ovstage_placement_t layout drifted");
static_assert(offsetof(ovstage_placement_t, stream) == sizeof(DLDevice),
              "ovstage_placement_t layout drifted");
static_assert(offsetof(ovstage_placement_t, availability_event) == sizeof(DLDevice) + sizeof(uintptr_t),
              "ovstage_placement_t layout drifted");
static_assert(sizeof(ovstage_placement_t) == sizeof(DLDevice) + 2 * sizeof(uintptr_t),
              "ovstage_placement_t layout drifted");

// OVSTAGE_PLACEMENT_INIT has to cover every field, including ones added after it
// was written. A caller that initializes with the macro and then assigns only
// `device` hands ovstage whatever the macro left behind -- and a stale
// `availability_event` is a cudaEvent_t ovstage would record into.
//
// A non-zero availability_event is not exercised here: recording one needs a
// CUDA device, and this suite is CPU-only by contract. Its behaviour is covered
// by the GPU suite.
TEST(PlacementInit, LeavesEveryFieldUnspecified)
{
    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    EXPECT_EQ(static_cast<int>(placement.device.device_type), 0);
    EXPECT_EQ(placement.device.device_id, 0);
    EXPECT_EQ(placement.stream, uintptr_t(0));
    EXPECT_EQ(placement.availability_event, uintptr_t(0));
}

class PlacementTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.placement";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        const ovx_string_t attrName = str("temperature");
        ASSERT_EQ(path_dictionary_create_tokens_from_strings(dict_, &attrName, 1, &attr_).status, OVX_API_SUCCESS);
        const ovx_string_t paths[] = { str("/World/Placed/A"), str("/World/Placed/B") };
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

    // A two-element CPU float column, written through the placed write.
    ovstage_enqueue_result_t writePlaced(const ovstage_placement_t* placement, ovstage_ordinal_t ordinal)
    {
        shape_[0] = 2;
        strides_[0] = 1;
        tensor_ = DLTensor{};
        tensor_.data = values_;
        tensor_.device = { kDLCPU, 0 };
        tensor_.ndim = 1;
        tensor_.dtype = { kDLFloat, 32, 1 };
        tensor_.shape = shape_;
        tensor_.strides = strides_;

        ovstage_write_data_t data{};
        data.tensors = &tensor_;
        data.tensor_count = 1;
        data.is_array = false;

        const ovx_string_or_token_t attribute{ attr_, {} };

        return ovstage_write_attribute_on_device(
            stage_, query_, attribute, ordinal, data, OVSTAGE_PRIM_MODE_UPSERT, placement);
    }

    // Seal the two-element column at ordinal 1 so there is something to read.
    // Placement on a read selects which *existing* copy is handed back; unlike a
    // write it produces nothing, so every read case needs data in place first.
    bool seedColumn()
    {
        if (!waitOp(stage_, writePlaced(nullptr, 1), "seed write"))
            return false;
        ovstage_write_floor_desc_t floor{};
        floor.ordinal = 1;
        floor.scope = OVSTAGE_SCOPE_ALL;
        return waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor");
    }

    ovstage_enqueue_result_t readPlaced(const ovstage_placement_t* placement, ovstage_read_handle_t* outRead)
    {
        ovstage_ordinal_range_t range{};
        range.end_ordinal = 1;
        // The placed read takes its placement *before* the out-handle, matching
        // the placed map and not the placed write. Spelled out once here so each
        // case below does not have to re-derive it.
        return ovstage_read_attributes_on_device(stage_, query_, &attr_, 1, range, placement, outRead);
    }

    // Consume one group and assert it is the seeded host column. Host-side only:
    // this suite never has a device pointer it is allowed to dereference.
    void expectSeededHostGroup(ovstage_read_handle_t read)
    {
        ovstage_read_group_t group{};
        ASSERT_EQ(ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group), OVSTAGE_OK);
        ASSERT_EQ(group.data.tensor_count, 1u);
        const DLTensor& tensor = group.data.tensors[0];
        ASSERT_NE(tensor.data, nullptr);
        ASSERT_GE(tensor.shape[0], 2);
        EXPECT_EQ(tensor.device.device_type, kDLCPU);
        float out[2] = { 0.0f, 0.0f };
        std::memcpy(out, tensor.data, sizeof(out));
        EXPECT_FLOAT_EQ(out[0], 7.0f);
        EXPECT_FLOAT_EQ(out[1], 8.0f);
        ovstage_release_group(stage_, &group);
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_token_t attr_ = 0;
    ovx_primpath_list_t pathList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t query_ = OVSTAGE_INVALID_QUERY_HANDLE;
    float values_[2] = { 7.0f, 8.0f };
    int64_t shape_[1] = { 2 };
    int64_t strides_[1] = { 1 };
    DLTensor tensor_{};
};

// Placement is an extension, not a vtable slot: an implementation advertises it
// through query_extension, so a caller can check for it instead of calling past
// the end of a vtable that does not have it.
TEST_F(PlacementTest, PlacementIsADiscoverableExtension)
{
    const ovstage_placement_extension_v1_t* ext = nullptr;
    ASSERT_EQ(ovstage_query_placement_extension(stage_, &ext), OVSTAGE_OK);
    ASSERT_NE(ext, nullptr);
    EXPECT_EQ(ext->header.version, OVSTAGE_PLACEMENT_EXTENSION_VERSION);
    EXPECT_GE(ext->header.struct_size, sizeof(ovstage_placement_extension_v1_t));
    EXPECT_NE(ext->write_attribute_on_device, nullptr);
    EXPECT_NE(ext->write_attributes_on_device, nullptr);
    EXPECT_NE(ext->map_attribute_on_device, nullptr);
    EXPECT_NE(ext->read_attributes_on_device, nullptr);

    // The helper is query_extension plus an exact-version and size check; both
    // reach the same table.
    const void* raw = nullptr;
    ASSERT_EQ(ovstage_query_extension(stage_, OVSTAGE_PLACEMENT_EXTENSION_NAME, &raw), OVSTAGE_OK);
    EXPECT_EQ(raw, static_cast<const void*>(ext));
}

TEST_F(PlacementTest, PlacementExtensionQueryRejectsNullOut)
{
    EXPECT_EQ(ovstage_query_placement_extension(stage_, nullptr), OVSTAGE_ERROR_INVALID_ARGUMENT);
}

namespace
{

const ovstage_placement_extension_v1_t* g_servedTable = nullptr;

ovstage_api_status_t serveTable(ovstage_context_t*, const char* name, const void** out)
{
    *out = nullptr;
    if (std::strcmp(name, OVSTAGE_PLACEMENT_EXTENSION_NAME) != 0)
        return OVSTAGE_ERROR_NOT_FOUND;
    *out = g_servedTable;
    return OVSTAGE_OK;
}

} // namespace

// The lookup accepts only this header's exact version: a table at another one
// is incompatible by definition, however large it is. A fake instance serves
// versions a real implementation never does.
TEST(PlacementExtensionVersion, OnlyTheExactVersionIsAccepted)
{
    ovstage_placement_extension_v1_t table{};
    ovstage_vtable_t vtable{};
    vtable.query_extension = serveTable;
    ovstage_instance_t fake{ &vtable, nullptr };
    g_servedTable = &table;

    table.header.struct_size = static_cast<uint32_t>(sizeof(table)) + 16u;
    for (uint32_t version : { 0u, OVSTAGE_PLACEMENT_EXTENSION_VERSION + 1u })
    {
        table.header.version = version;
        const ovstage_placement_extension_v1_t* ext = &table;
        EXPECT_EQ(ovstage_query_placement_extension(&fake, &ext), OVSTAGE_ERROR_NOT_FOUND)
            << "version " << version;
        EXPECT_EQ(ext, nullptr);
    }
    table.header.version = OVSTAGE_PLACEMENT_EXTENSION_VERSION;
    const ovstage_placement_extension_v1_t* ext = nullptr;
    EXPECT_EQ(ovstage_query_placement_extension(&fake, &ext), OVSTAGE_OK);
    EXPECT_EQ(ext, &table);
    g_servedTable = nullptr;
}

TEST_F(PlacementTest, UnknownExtensionIsNotFound)
{
    const void* raw = &raw; // non-null, so the test proves query_extension clears it
    EXPECT_EQ(ovstage_query_extension(stage_, "ovstage.no-such-extension", &raw), OVSTAGE_ERROR_NOT_FOUND);
    EXPECT_EQ(raw, nullptr);
}

// A NULL placement is exactly the unplaced call: this is the compatibility
// guarantee that lets placed and unplaced callers share one implementation.
TEST_F(PlacementTest, NullPlacementBehavesLikeAnUnplacedWrite)
{
    EXPECT_TRUE(waitOp(stage_, writePlaced(nullptr, 1), "write_attribute_on_device(NULL)"));
}

// OVSTAGE_PLACEMENT_INIT alone names no device, so it is also "unspecified".
TEST_F(PlacementTest, UnspecifiedDeviceIsAccepted)
{
    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    EXPECT_TRUE(waitOp(stage_, writePlaced(&placement, 1), "write_attribute_on_device(unspecified)"));
}

// A placement naming the device the source already lives on is honored. This is
// the suite's one positive placed write that reads its values back, so it is
// what pins that a placed write actually commits rather than merely enqueuing.
TEST_F(PlacementTest, CpuPlacementAgreeingWithSourceIsHonored)
{
    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLCPU, 0 };
    ASSERT_TRUE(waitOp(stage_, writePlaced(&placement, 1), "write_attribute_on_device(cpu)"));

    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 1;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_TRUE(waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor"));

    ovstage_ordinal_range_t range{};
    range.end_ordinal = 1;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_TRUE(waitOp(stage_, ovstage_read_attributes(stage_, query_, &attr_, 1, range, &read), "read"));

    ovstage_read_group_t group{};
    ASSERT_EQ(ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group), OVSTAGE_OK);
    ASSERT_EQ(group.data.tensor_count, 1u);
    const DLTensor& tensor = group.data.tensors[0];
    ASSERT_NE(tensor.data, nullptr);
    ASSERT_GE(tensor.shape[0], 2);
    float out[2] = { 0.0f, 0.0f };
    std::memcpy(out, tensor.data, sizeof(out));
    EXPECT_FLOAT_EQ(out[0], 7.0f);
    EXPECT_FLOAT_EQ(out[1], 8.0f);
    // A CPU-placed column must report itself as CPU-resident.
    EXPECT_EQ(tensor.device.device_type, kDLCPU);

    ovstage_release_group(stage_, &group);
    waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
}

// A host payload with a CUDA placement is staged onto the device.
//
// This suite is CPU-only, so the assertion has to hold on both kinds of host:
// with a CUDA device the write is accepted and the data lands on the GPU;
// without one, device 0 names nothing and the id check rejects it. What it pins
// either way is that this direction is no longer refused as unsupported -- the
// full device matrix is covered by the GPU suite.
TEST_F(PlacementTest, HostPayloadMayBePlacedOnACudaDevice)
{
    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLCUDA, 0 };
    const ovstage_enqueue_result_t enq = writePlaced(&placement, 1);
    EXPECT_TRUE(enq.status == OVSTAGE_OK || enq.status == OVSTAGE_ERROR_INVALID_ARGUMENT)
        << "a host->CUDA placement must be staged or rejected for a missing device, got " << enq.status;
}

// A placed map has no source tensor, so unlike a placed write there is nothing
// to diverge from; only the device itself is validated.
TEST_F(PlacementTest, MapWithOutOfRangeDeviceIdIsRejected)
{
    ovstage_map_desc_t desc{};
    desc.attribute = ovx_string_or_token_t{ attr_, {} };
    desc.dtype = DLDataType{ kDLFloat, 32, 1 };
    desc.prim_mode = OVSTAGE_PRIM_MODE_UPSERT;

    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLCUDA, 4096 };

    ovstage_map_handle_t map = OVSTAGE_INVALID_MAP_HANDLE;
    const ovstage_enqueue_result_t enq =
        ovstage_map_attribute_on_device(stage_, query_, &desc, 1, nullptr, 0, &placement, &map);
    EXPECT_EQ(enq.status, OVSTAGE_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(map, OVSTAGE_INVALID_MAP_HANDLE);
}

// An unsupported device *kind* is a capability gap; a bad *id* is a caller bug.
// Keeping those on different statuses is what lets a caller tell them apart.
TEST_F(PlacementTest, OutOfRangeDeviceIdIsInvalidArgument)
{
    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLCUDA, 4096 };
    const ovstage_enqueue_result_t enq = writePlaced(&placement, 1);
    EXPECT_EQ(enq.status, OVSTAGE_ERROR_INVALID_ARGUMENT);
}

TEST_F(PlacementTest, UnsupportedDeviceTypeIsNotSupported)
{
    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLVulkan, 0 };
    const ovstage_enqueue_result_t enq = writePlaced(&placement, 1);
    EXPECT_EQ(enq.status, OVSTAGE_ERROR_NOT_SUPPORTED);
}

// Placement is an operation-level modifier, so the batched slot takes one for
// every entry rather than one per attribute.
TEST_F(PlacementTest, BatchedWriteAcceptsOnePlacement)
{
    shape_[0] = 2;
    strides_[0] = 1;
    tensor_ = DLTensor{};
    tensor_.data = values_;
    tensor_.device = { kDLCPU, 0 };
    tensor_.ndim = 1;
    tensor_.dtype = { kDLFloat, 32, 1 };
    tensor_.shape = shape_;
    tensor_.strides = strides_;

    ovstage_write_data_t data{};
    data.tensors = &tensor_;
    data.tensor_count = 1;
    data.is_array = false;

    ovstage_attribute_write_t write{};
    write.attribute = ovx_string_or_token_t{ attr_, {} };
    write.data = data;

    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLCPU, 0 };

    EXPECT_TRUE(waitOp(stage_,
                       ovstage_write_attributes_on_device(
                           stage_, query_, &write, 1, 1, OVSTAGE_PRIM_MODE_UPSERT, &placement),
                       "write_attributes_on_device"));
}

// ── Placed reads ────────────────────────────────────────────────────────────
//
// A placed read selects which of an attribute's copies the returned groups point
// at. Serving one from a CUDA device needs a CUDA device, so this suite pins the
// surface and the validation only: the host cases, and the statuses a bad
// placement produces. The device matrix -- transfer, zero-copy, availability
// event, payload lifetime -- is the GPU suite's.

// A NULL placement is exactly read_attributes. Every read_attributes caller in
// existence is a NULL-placement caller, so this is the compatibility guarantee
// the additive slot rests on.
TEST_F(PlacementTest, ReadWithNullPlacementMatchesTheUnplacedRead)
{
    ASSERT_TRUE(seedColumn());

    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_TRUE(waitOp(stage_, readPlaced(nullptr, &read), "read_attributes_on_device(NULL)"));
    expectSeededHostGroup(read);
    waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
}

// A host placement is the same call again, now stated explicitly. Naming the
// device the column is already current on is the zero-copy case; here there is
// nothing to transfer and nothing to prove beyond "the values came back".
TEST_F(PlacementTest, CpuPlacementOnAReadBehavesLikeTheUnplacedRead)
{
    ASSERT_TRUE(seedColumn());

    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLCPU, 0 };
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_TRUE(waitOp(stage_, readPlaced(&placement, &read), "read_attributes_on_device(cpu)"));
    expectSeededHostGroup(read);
    waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
}

// OVSTAGE_PLACEMENT_INIT names no device, so a struct that is otherwise empty is
// "unspecified" rather than "device zero of some kind".
TEST_F(PlacementTest, ReadWithAnUnspecifiedDeviceIsAccepted)
{
    ASSERT_TRUE(seedColumn());

    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_TRUE(waitOp(stage_, readPlaced(&placement, &read), "read_attributes_on_device(unspecified)"));
    expectSeededHostGroup(read);
    waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
}

// The permissive rule, as far as a CPU-only runner can see it: the column here is
// current on the host, and asking for it on a CUDA device must not be refused for
// that reason: a placed read makes the data available on the named device. A
// device-residency refusal here would be a regression, even though this suite
// cannot see the transfer itself.
//
// The assertion has to hold on both kinds of host: with a CUDA device the read is
// accepted and served from it; without one, device 0 names nothing and the id
// check rejects it before residency is ever consulted.
TEST_F(PlacementTest, ReadOfAHostColumnOnACudaDeviceIsNotRefused)
{
    ASSERT_TRUE(seedColumn());

    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLCUDA, 0 };
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    const ovstage_enqueue_result_t enq = readPlaced(&placement, &read);
    if (enq.status == OVSTAGE_ERROR_INVALID_ARGUMENT)
    {
        EXPECT_EQ(read, OVSTAGE_INVALID_READ_HANDLE);
        return; // No CUDA device on this runner.
    }
    // Accepted. The payload is deliberately not fetched: what lands in the group
    // is a device pointer, and asserting anything about it would make this case
    // GPU-dependent. That it was served at all is the GPU suite's to prove.
    EXPECT_TRUE(waitOp(stage_, enq, "read_attributes_on_device(cuda)"));
    waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
}

// Same status split as the placed write: an unsupported device *kind* is a
// capability gap, an *id* naming no device is a caller bug. A read that validated
// neither would mislabel its payload with a device the memory is not on.
TEST_F(PlacementTest, ReadWithOutOfRangeDeviceIdIsRejected)
{
    ASSERT_TRUE(seedColumn());

    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLCUDA, 4096 };
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    const ovstage_enqueue_result_t enq = readPlaced(&placement, &read);
    EXPECT_EQ(enq.status, OVSTAGE_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(enq.op_index, OVSTAGE_INVALID_OP_ID);
    EXPECT_EQ(read, OVSTAGE_INVALID_READ_HANDLE);
}

TEST_F(PlacementTest, ReadWithUnsupportedDeviceTypeIsNotSupported)
{
    ASSERT_TRUE(seedColumn());

    ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;
    placement.device = DLDevice{ kDLVulkan, 0 };
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    const ovstage_enqueue_result_t enq = readPlaced(&placement, &read);
    EXPECT_EQ(enq.status, OVSTAGE_ERROR_NOT_SUPPORTED);
    EXPECT_EQ(read, OVSTAGE_INVALID_READ_HANDLE);
}

} // namespace
