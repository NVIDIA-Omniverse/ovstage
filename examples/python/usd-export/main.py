# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

import math
from pathlib import Path
from tempfile import TemporaryDirectory

from ovstage import PopulationDomain, Stage, population


def main() -> int:
    source = """#usda 1.0
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
"""

    with TemporaryDirectory(prefix="ovstage-usd-export-") as temp_dir:
        destination = Path(temp_dir, "destination.usda")

        with Stage("example.usd-export.source") as stage:
            population.open_usd_from_string(
                stage,
                source,
                ordinal=1,
                time_code=math.nan,
                domains=PopulationDomain.ALL,
            )
            stage.advance_write_floor(ordinal=1).wait()

            # [snippet:typed-hierarchy-python-example]
            report = population.export_typed_hierarchy_to_usd_file(
                stage,
                str(destination),
                "/World/Props",
                ordinal=1,
            )

            text = destination.read_text(encoding="utf-8")
            assert 'def Sphere "Ball"' in text
            assert 'def Cube "Crate"' in text
            assert "PhysicsCollisionAPI" in text
            assert "PhysicsRigidBodyAPI" in text
            assert "OutsideSelection" not in text
            # [/snippet:typed-hierarchy-python-example]

    print("saved /World/Props as typed USD to destination.usda")
    print(
        f"report: {report['prims_exported']} prims, {report['attributes_exported']} attributes, "
        f"{report['api_schemas_applied']} applied schemas"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
