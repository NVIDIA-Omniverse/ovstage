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
project(transforms)

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

add_executable(transforms main.cpp)
target_link_libraries(transforms PRIVATE ovstage::ovstage)

add_custom_command(TARGET transforms POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_LIST_DIR}/car.usda"
        "$<TARGET_FILE_DIR:transforms>/car.usda"
    COMMENT "Copying car.usda to build directory"
)

# Setup runtime dependencies (rpath / loader-path guidance)
ovstage_setup_runtime(transforms)

# Optional helper for applications that already own CUDA transform buffers.
option(OVSTAGE_EXAMPLE_CUDA "Build CUDA write helpers" OFF)
if(OVSTAGE_EXAMPLE_CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_library(transforms-gpu-write OBJECT gpu_write.cpp)
    target_link_libraries(transforms-gpu-write PRIVATE ovstage::ovstage CUDA::cudart)
endif()
