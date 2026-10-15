// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage support-API test: the version accessor and the status->string
// mapping used for diagnostics. CPU-only.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace
{

class SupportApiTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.support";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        ASSERT_NE(stage_, nullptr);
    }

    void TearDown() override
    {
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    ovstage_instance_t* stage_ = nullptr;
};

TEST_F(SupportApiTest, VersionAndErrorString)
{
    // [snippet:version-and-error-c]
    // ovstage_get_version reports the runtime version; ovstage_get_error_string
    // maps any status code to a human-readable string — never NULL, including for
    // OVSTAGE_OK, and distinct per code.
    uint32_t major = 0, minor = 0, patch = 0;
    ovstage_get_version(stage_, &major, &minor, &patch);
    EXPECT_EQ(major, OVSTAGE_VERSION_MAJOR);
    EXPECT_EQ(minor, OVSTAGE_VERSION_MINOR);
    EXPECT_EQ(patch, OVSTAGE_VERSION_PATCH);

    const char* okText = ovstage_get_error_string(stage_, OVSTAGE_OK);
    const char* errText = ovstage_get_error_string(stage_, OVSTAGE_ERROR_INVALID_ARGUMENT);
    // [/snippet:version-and-error-c]

    ASSERT_NE(okText, nullptr);
    ASSERT_NE(errText, nullptr);
    EXPECT_GT(std::strlen(errText), 0u);
    EXPECT_STRNE(okText, errText);  // distinct codes map to distinct strings

    const ovstage_build_info_t* buildInfo = ovstage_get_build_info();
    ASSERT_NE(buildInfo, nullptr);
    EXPECT_GE(buildInfo->struct_size, sizeof(ovstage_build_info_t));
    ASSERT_NE(buildInfo->build_string.ptr, nullptr);
    ASSERT_NE(buildInfo->git_hash.ptr, nullptr);
    const std::string semanticVersion = std::to_string(major) + "." + std::to_string(minor) + "." +
                                        std::to_string(patch);
    EXPECT_EQ(std::string(buildInfo->build_string.ptr, semanticVersion.size()), semanticVersion);
    EXPECT_GT(buildInfo->build_string.length, semanticVersion.size());
    EXPECT_GT(buildInfo->git_hash.length, 0u);
}

TEST(ConfigReadback, CapturesProcessConfigForTheInstance)
{
    ovstage_config_entry_t entry = ovstage_config_entry_runtime_default_hierarchy_computation_model(
        OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL);
    ovstage_config_t config{ &entry, 1 };
    ASSERT_EQ(ovstage_initialize(&config), OVSTAGE_OK);

    // Readback owns its storage instead of borrowing the caller's entry.
    entry.value.uint_value = OVSTAGE_HIERARCHY_COMPUTATION_MODEL_GPU_GLOBAL;

    ovstage_instance_desc_t desc{};
    desc.name = "test.ovstage.config-readback";
    ovstage_instance_t* stage = nullptr;
    EXPECT_EQ(ovstage_create_instance(&desc, &stage), OVSTAGE_OK);
    EXPECT_NE(stage, nullptr);
    if (stage)
    {
        ovstage_config_t capturedConfig{};
        const ovstage_api_status_t getStatus = ovstage_get_config(stage, &capturedConfig);
        EXPECT_EQ(getStatus, OVSTAGE_OK);
        if (getStatus == OVSTAGE_OK)
        {
            EXPECT_EQ(capturedConfig.entry_count, 1u);
            if (capturedConfig.entry_count == 1u)
            {
                EXPECT_EQ(capturedConfig.entries[0].key_type, OVSTAGE_CONFIG_KEY_TYPE_UINT64);
                EXPECT_EQ(capturedConfig.entries[0].key.uint64_key,
                          OVSTAGE_CONFIG_RUNTIME_DEFAULT_HIERARCHY_COMPUTATION_MODEL);
                EXPECT_EQ(
                    capturedConfig.entries[0].value.uint_value, OVSTAGE_HIERARCHY_COMPUTATION_MODEL_CPU_INCREMENTAL);
            }
        }
        EXPECT_EQ(ovstage_destroy_instance(stage), OVSTAGE_OK);
    }
    EXPECT_EQ(ovstage_shutdown(), OVSTAGE_OK);
}

TEST(ProcessConfig, RejectsUnknownKey)
{
    ovstage_config_entry_t entry{};
    entry.key_type = OVSTAGE_CONFIG_KEY_TYPE_BOOL;
    entry.key.bool_key = OVSTAGE_CONFIG_BOOL_COUNT;
    entry.value.bool_value = true;
    const ovstage_config_t config{ &entry, 1 };

    EXPECT_EQ(ovstage_initialize(&config), OVSTAGE_ERROR_INVALID_ARGUMENT);
}

} // namespace

namespace
{

// Initialization must precede instance creation when selecting a process policy.
class OrdinalAdmissionConfigTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        if (stage_)
            EXPECT_EQ(ovstage_destroy_instance(stage_), OVSTAGE_OK);
        if (initialized_)
            EXPECT_EQ(ovstage_shutdown(), OVSTAGE_OK);
    }

    ovstage_api_status_t complete(ovstage_enqueue_result_t operation)
    {
        if (operation.status != OVSTAGE_OK)
            return operation.status;
        ovstage_op_wait_result_t result{};
        const auto status = ovstage_wait_op(stage_, operation.op_index, OVSTAGE_TIMEOUT_INFINITE, &result);
        EXPECT_EQ(ovstage_release_op(stage_, operation.op_index), OVSTAGE_OK);
        return status;
    }

    bool initialized_ = false;
    ovstage_instance_t* stage_ = nullptr;
};

TEST_F(OrdinalAdmissionConfigTest, ConfiguresAndEnforcesNondecreasing)
{
    // [snippet:configure-ordinal-admission-c]
    // Choose the policy before the first initialize or instance in this lifetime.
    // NONDECREASING allows another edit at the latest ordinal, but rejects older edits.
    ovstage_config_entry_t entry = ovstage_config_entry_runtime_default_ordinal_admission_policy(
        OVSTAGE_ORDINAL_ADMISSION_NONDECREASING);
    const ovstage_config_t config{ &entry, 1 };
    ASSERT_EQ(ovstage_initialize(&config), OVSTAGE_OK);
    initialized_ = true;

    ovstage_instance_desc_t desc{};
    desc.name = "test.ovstage.ordinal-admission-config";
    ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
    ASSERT_NE(stage_, nullptr);
    ovstage_config_t captured{};
    ASSERT_EQ(ovstage_get_config(stage_, &captured), OVSTAGE_OK);
    ASSERT_EQ(captured.entry_count, 1u);
    EXPECT_EQ(captured.entries[0].key_type, OVSTAGE_CONFIG_KEY_TYPE_UINT64);
    EXPECT_EQ(captured.entries[0].key.uint64_key, OVSTAGE_CONFIG_RUNTIME_DEFAULT_ORDINAL_ADMISSION_POLICY);
    EXPECT_EQ(captured.entries[0].value.uint_value, OVSTAGE_ORDINAL_ADMISSION_NONDECREASING);
    // [/snippet:configure-ordinal-admission-c]

    path_dictionary_instance_t* dictionary = ovstage_get_path_dictionary(stage_);
    ASSERT_NE(dictionary, nullptr);
    const ovx_string_t path = literal_to_ovx_string("/World/OrdinalAdmission");
    ovx_primpath_list_t paths = OVX_INVALID_PRIMPATH_LIST;
    ASSERT_EQ(path_dictionary_create_path_list_from_strings(dictionary, &path, 1, &paths).status, OVX_API_SUCCESS);
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    ASSERT_EQ(ovstage_query_from_path_list(stage_, paths, &query), OVSTAGE_OK);
    ASSERT_EQ(path_dictionary_release_path_list_reference(dictionary, paths).status, OVX_API_SUCCESS);
    const ovx_string_t attributeName = literal_to_ovx_string("temperature");
    ovx_token_t attribute = OVX_INVALID_TOKEN;
    ASSERT_EQ(path_dictionary_create_tokens_from_strings(dictionary, &attributeName, 1, &attribute).status,
              OVX_API_SUCCESS);

    float value = 10.0f;
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
    ASSERT_EQ(complete(ovstage_write_attribute(stage_, query, { attribute, {} }, 10, write,
                                               OVSTAGE_PRIM_MODE_UPSERT)), OVSTAGE_OK);
    value = 20.0f;
    ASSERT_EQ(complete(ovstage_write_attribute(stage_, query, { attribute, {} }, 10, write,
                                               OVSTAGE_PRIM_MODE_UPSERT)), OVSTAGE_OK);
    const auto older = ovstage_write_attribute(stage_, query, { attribute, {} }, 9, write,
                                               OVSTAGE_PRIM_MODE_UPSERT);
    ASSERT_EQ(older.status, OVSTAGE_OK); // Enqueue success is not operation success.
    EXPECT_EQ(complete(older), OVSTAGE_ERROR_OP_FAILED);

    // Mutating the original entry must not alter the captured policy or config.
    entry.value.uint_value = OVSTAGE_ORDINAL_ADMISSION_FLOOR_ONLY;
    EXPECT_EQ(ovstage_initialize(&config), OVSTAGE_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(captured.entries[0].value.uint_value, OVSTAGE_ORDINAL_ADMISSION_NONDECREASING);

    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 10;
    floor.scope = OVSTAGE_SCOPE_ALL;
    ASSERT_EQ(complete(ovstage_advance_write_floor(stage_, &floor)), OVSTAGE_OK);
    const auto sealed = ovstage_write_attribute(stage_, query, { attribute, {} }, 10, write,
                                                OVSTAGE_PRIM_MODE_UPSERT);
    EXPECT_EQ(sealed.status, OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION);
    EXPECT_EQ(sealed.op_index, OVSTAGE_INVALID_OP_ID);

    ovstage_ordinal_range_t range{};
    range.end_ordinal = 10;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    ASSERT_EQ(complete(ovstage_read_attributes(stage_, query, &attribute, 1, range, &read)), OVSTAGE_OK);
    ovstage_read_group_t group{};
    ASSERT_EQ(ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group), OVSTAGE_OK);
    ASSERT_EQ(group.data.tensor_count, 1u);
    ASSERT_NE(group.data.tensors, nullptr);
    ASSERT_NE(group.data.tensors[0].data, nullptr);
    EXPECT_FLOAT_EQ(*static_cast<const float*>(group.data.tensors[0].data), 20.0f);
    EXPECT_EQ(ovstage_release_group(stage_, &group), OVSTAGE_OK);
    EXPECT_EQ(complete(ovstage_release_read(stage_, read)), OVSTAGE_OK);
    EXPECT_EQ(complete(ovstage_release_query(stage_, query)), OVSTAGE_OK);
}

} // namespace
