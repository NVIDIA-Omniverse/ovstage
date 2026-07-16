# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.
#
# ovstage public C tests. Consumes the *produced* ovstage package exactly like an
# external project would (find_package(ovstage) + link ovstage::ovstage), same as
# ../../examples/smoke. Point CMake at an unpacked package via CMAKE_PREFIX_PATH:
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/unpacked-ovstage
#   cmake --build build --config Release
#   ctest --test-dir build --output-on-failure
#
# run_tests.sh does this for you (and sets the runtime loader path). CPU-only.

cmake_minimum_required(VERSION 3.18)
project(ovstage_public_tests_c CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ovstage REQUIRED)

# Fetch GoogleTest (parity with rendering/ovrtx/public/tests/docs/c).
include(FetchContent)
FetchContent_Declare(
    googletest
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(ovstage_public_tests
    test_minimal.cpp
    test_population.cpp
    test_clone.cpp
    test_queries.cpp
    test_error_handling.cpp
    test_attribute_shapes.cpp
    test_write_modes.cpp
    test_sparse_writes.cpp
    test_delete.cpp
    test_attributes.cpp
    test_map_attribute.cpp
    test_logging.cpp
    test_support_api.cpp)
target_link_libraries(ovstage_public_tests PRIVATE ovstage::ovstage GTest::gtest_main)

include(GoogleTest)
# PRE_TEST discovery: enumerate the tests at ctest time, not post-build. The test
# exe links ovstage, which on Windows has no rpath — so running it at build time
# (the default POST_BUILD discovery) fails to load ovstage.dll (0xC0000135
# DLL_NOT_FOUND). At ctest time the runner puts the package bin/ on the loader
# path, so both discovery and execution find the runtime. DISCOVERY_TIMEOUT covers
# the one-time cost of loading ovstage's runtime closure while listing tests.
gtest_discover_tests(ovstage_public_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120)
