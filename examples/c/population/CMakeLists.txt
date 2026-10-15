# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

cmake_minimum_required(VERSION 3.18)
project(population)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FETCHCONTENT_QUIET FALSE)
# Override FetchContent's base directory to share large deps among examples.
# If copying this project to your own workspace, delete this line or override it.
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/../_deps" CACHE PATH "")

# Fetch ovstage package
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")
include(ovstage)
ovstage_fetch()

add_executable(population main.cpp)
target_link_libraries(population PRIVATE ovstage::ovstage)

add_custom_command(TARGET population POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_LIST_DIR}/scene.usda"
        "$<TARGET_FILE_DIR:population>/scene.usda"
    COMMENT "Copying scene.usda to build directory"
)

# Setup runtime dependencies (rpath / loader-path guidance)
ovstage_setup_runtime(population)
