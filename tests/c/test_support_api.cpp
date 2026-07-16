// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage support-API test: the version accessor and the status->string
// mapping used for diagnostics. CPU-only.

#include <ovstage/ovstage.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

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
}

} // namespace
