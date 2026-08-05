# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

# ovstage.cmake - Fetch and configure the ovstage package
#
# Usage:
#   list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")
#   include(ovstage)
#   ovstage_fetch()
#
#   add_executable(myapp main.cpp)
#   target_link_libraries(myapp PRIVATE ovstage::ovstage)
#   ovstage_setup_runtime(myapp)

# Capture this file's directory at parse time (before macro expansion)
# CMAKE_CURRENT_LIST_DIR inside a macro would refer to the caller's directory
set(_OVSTAGE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Locate an ovstage package, downloading the pinned release if none is found
macro(ovstage_fetch)
    find_package(ovstage QUIET)

    if (ovstage_FOUND)
        message(STATUS "found ovstage at: ${ovstage_DIR}")
    else()
        set(FETCHCONTENT_QUIET FALSE)

        # Override FetchContent's base directory to share large deps among examples.
        # Defaults to examples/c/_deps (the same directory the shipped examples
        # pre-set), so a consumer who copies only this module still shares one
        # download. Delete or override when copying to your own workspace.
        if(NOT DEFINED CACHE{FETCHCONTENT_BASE_DIR})
            set(FETCHCONTENT_BASE_DIR "${_OVSTAGE_CMAKE_DIR}/../_deps" CACHE PATH "Shared FetchContent directory")
        endif()

        # Platform-specific package selection.
        # OVSTAGE_HASH pins the package zip's SHA256 for download verification;
        # an empty hash skips verification rather than failing the fetch.
        if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            set(OVSTAGE_PACKAGE_SYSTEM "windows-x86_64")
            set(OVSTAGE_HASH "21ec62c2eb94ff346678e3fd2a41c4068a08a35d240341f95ec4e2ce1f23cf08")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            if (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
                set(OVSTAGE_PACKAGE_SYSTEM "manylinux_2_35_aarch64")
                set(OVSTAGE_HASH "7892d2e8139b6aaae14b993f9a9b0c08b36cf53b7c00767a46ed0e5036222051")
            elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
                set(OVSTAGE_PACKAGE_SYSTEM "manylinux_2_35_x86_64")
                set(OVSTAGE_HASH "34703875ac9ec81d8f12ea8695d90fe16ebe9921c610f66202c8b6ee2729c49f")
            else()
                message(FATAL_ERROR "Unsupported system: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
            endif()
        else()
            message(FATAL_ERROR "Unsupported system: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
        endif()

        if(OVSTAGE_HASH)
            set(_OVSTAGE_URL_HASH URL_HASH "SHA256=${OVSTAGE_HASH}")
        else()
            message(WARNING "OVSTAGE_HASH is empty; package download verification is disabled until pins are stamped")
            set(_OVSTAGE_URL_HASH "")
        endif()

        include(FetchContent)

        FetchContent_Declare(
            ovstage
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            URL "https://github.com/NVIDIA-Omniverse/ovstage/releases/download/v0.1.1/ovstage@0.1.1.355824.553acd42.${OVSTAGE_PACKAGE_SYSTEM}.zip"
            ${_OVSTAGE_URL_HASH}
        )


        FetchContent_MakeAvailable(ovstage)

        # Make ovstage findable by find_package
        list(APPEND CMAKE_PREFIX_PATH ${ovstage_SOURCE_DIR})

        find_package(ovstage REQUIRED)

    endif()
endmacro()

# Setup runtime dependencies for a target (rpath / loader-path guidance)
function(ovstage_setup_runtime TARGET_NAME)
    # IMPORTANT: never copy the ovstage shared library next to the executable.
    # libovstage discovers its bundled runtime (bin/plugins/**, bin/ovstage_usd_schemas/)
    # relative to the directory it is loaded from, so it must stay in the package
    # bin/ beside that tree; a copied library has no sibling plugins/ and
    # ovstage_create_instance() fails to load its runtime support.
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        # Windows has no rpath: put the package bin/ on PATH when running, e.g.
        #   $env:PATH = "<ovstage-package>\bin;$env:PATH"
        message(STATUS "ovstage runtime: add ${OVSTAGE_BINARY_DIR} to PATH before running ${TARGET_NAME}")
    else()
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUILD_RPATH "${OVSTAGE_BINARY_DIR}"
            INSTALL_RPATH "${OVSTAGE_BINARY_DIR}"
        )
    endif()
endfunction()
