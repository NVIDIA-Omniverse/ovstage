# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

# examples/c/CMakeLists.txt (top-level for all examples)
cmake_minimum_required(VERSION 3.18)
project(ovstage_examples)

# Preserve debug symbols in Release builds (for stack traces and debugging)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g")
elseif(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Zi")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG")
endif()

# Include every example present in this tree. A published or filtered tree may
# omit individual examples, so an absent one is skipped with a note instead of
# failing the whole configure (the test driver skips running it too).
foreach(_ovstage_example
    authoring-hierarchy
    minimal
    producer-consumer
    queries
    runtime-loop
    time-and-ordinals
    write-flavors)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_ovstage_example}/CMakeLists.txt")
        add_subdirectory(${_ovstage_example})
    else()
        message(STATUS "skipping example ${_ovstage_example}: not in this tree")
    endif()
endforeach()
