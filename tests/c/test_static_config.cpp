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
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

TEST(StaticConfig, NonDefaultBinaryRoot)
{
    const char* packageBin = std::getenv("OVSTAGE_TEST_BINARY_ROOT");
    ASSERT_NE(packageBin, nullptr);
    const std::string binaryRoot(packageBin);

    // [snippet:initialize-static-loader-c]
    // binaryRoot is the extracted package's bin/ directory, not its top level.
    // Keep its string storage alive until ovstage_initialize returns.
    const ovx_string_t root{ binaryRoot.data(), binaryRoot.size() };
    const ovstage_config_entry_t entry = ovstage_config_entry_binary_package_root_path(root);
    const ovstage_config_t config{ &entry, 1 };
    // This must be the first ovstage_* call when using a non-default static-loader root.
    ASSERT_EQ(ovstage_initialize(&config), OVSTAGE_OK);

    ovstage_instance_desc_t desc{};
    desc.name = "static-loader-consumer";
    ovstage_instance_t* stage = nullptr;
    const ovstage_api_status_t createStatus = ovstage_create_instance(&desc, &stage);
    EXPECT_EQ(createStatus, OVSTAGE_OK);
    EXPECT_NE(stage, nullptr);
    if (stage)
    {
        EXPECT_NE(ovstage_get_path_dictionary(stage), nullptr);
        EXPECT_EQ(ovstage_destroy_instance(stage), OVSTAGE_OK);
    }
    // Destroy instances first, then balance the successful explicit initialization.
    EXPECT_EQ(ovstage_shutdown(), OVSTAGE_OK);
    // [/snippet:initialize-static-loader-c]
}
