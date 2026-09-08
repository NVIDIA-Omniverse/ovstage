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
#include <ovstage/ovstage_population.h>
#include <ovstage/ovstage_population_export.h>
#include <ovx/types.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "../common/ovstage_example_utils.h"

static ovstage_instance_t* createStage(const char* name)
{
    ovstage_instance_desc_t desc{};
    desc.name = name;
    ovstage_instance_t* stage = nullptr;
    check(nullptr, ovstage_create_instance(&desc, &stage), "create_instance");
    return stage;
}

int main(int argc, char** argv)
{
    const char* destination = argc > 1 ? argv[1] : "destination.usda";
    ovstage_instance_t* runtime = createStage("example.usd-export.source");

    const char* source = R"usd(#usda 1.0
def Xform "World"
{
    def Xform "Props"
    {
        def Sphere "Ball" (prepend apiSchemas = ["PhysicsCollisionAPI"])
        {
            double radius = 1.5
            bool physics:collisionEnabled = true
        }
        def Cube "Crate" (prepend apiSchemas = ["PhysicsRigidBodyAPI"])
        {
            double size = 2
            bool physics:rigidBodyEnabled = true
        }
    }
    def Sphere "OutsideSelection"
    {
        double radius = 10
    }
}
)usd";
    waitPop(runtime,
            ovstage_population_open_usd_from_string(
                runtime, { source, std::strlen(source) }, 1, NAN, OVSTAGE_POPULATION_DOMAIN_ALL),
            "open_usd_from_string");

    ovstage_write_floor_desc_t floor{};
    floor.ordinal = 1;
    floor.scope = OVSTAGE_SCOPE_ALL;
    waitOp(runtime, ovstage_advance_write_floor(runtime, &floor), "advance_write_floor");

    // [snippet:typed-hierarchy-c-example]
    const ovx_string_t root = literal_to_ovx_string("/World/Props");
    ovstage_population_export_desc_t exportDesc{};
    check(runtime,
          ovstage_population_export_desc_init_typed_hierarchy(&exportDesc, 1, &root),
          "export_desc_init_typed_hierarchy");

    ovstage_population_export_report_t report{};
    const ovx_string_t destinationId{ destination, std::strlen(destination) };
    check(runtime,
          ovstage_population_export_to_usd_file(runtime, destinationId, &exportDesc, &report),
          "export_to_usd_file");

    std::ifstream output(destination, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(output)), std::istreambuf_iterator<char>());
    if (text.find("def Sphere \"Ball\"") == std::string::npos ||
        text.find("def Cube \"Crate\"") == std::string::npos ||
        text.find("OutsideSelection") != std::string::npos)
    {
        std::fprintf(stderr, "the exported USD file does not contain the selected typed hierarchy\n");
        std::exit(EXIT_FAILURE);
    }
    // [/snippet:typed-hierarchy-c-example]

    std::printf("saved /World/Props as typed USD to %s\n", destination);
    std::printf("report: %llu prims, %llu attributes, %llu applied schemas\n",
                static_cast<unsigned long long>(report.prims_exported),
                static_cast<unsigned long long>(report.attributes_exported),
                static_cast<unsigned long long>(report.api_schemas_applied));
    ovstage_destroy_instance(runtime);
    return 0;
}
