# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

# [snippet:export-example-imports]
import math
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np

from ovstage import AttributeSemantic, OvstageError, PathDictionary, PopulationDomain, Stage, population
# [/snippet:export-example-imports]


# Demonstrate rule-based export on a single runtime-created prim.
def export_runtime_data(directory: Path):
    # [snippet:export-example-setup]
    with Stage("example.usd-export.runtime") as stage, PathDictionary(stage) as paths:
        # [snippet:export-write-runtime]
        with paths.create_path_list_from_strings(["/World/Ball"]) as selected:
            with stage.query_from_path_list(selected) as query:
                matrix = np.eye(4, dtype=np.float64)[None, :, :]
                matrix[0, 3, :3] = [1, 2, 3]
                stage.write_attribute(query, "omni:xform", ordinal=1, tensors=matrix,
                                      is_array=False, semantic=AttributeSemantic.MATRIX).wait()
                stage.write_attribute(query, "userProperties:temperature", ordinal=1,
                                      tensors=np.array([20], dtype=np.float32), is_array=False).wait()
                stage.write_attribute(query, "solver:collisionRadius", ordinal=1,
                                      tensors=np.array([2.5], dtype=np.float64), is_array=False).wait()
                # Record the USD type so DEF exports can create a Sphere.
                stage.write_attribute(query, "usd-prim-type", ordinal=1,
                                      tensors=np.array([paths.intern_token("Sphere")], dtype=np.uint64),
                                      is_array=False).wait()
        stage.advance_write_floor(ordinal=1).wait()
        # [/snippet:export-write-runtime]
        # Run the later snippets inside these same context managers.
    # [/snippet:export-example-setup]

        # [snippet:export-file]
        # A one-shot export creates, saves, and closes an overlay layer.
        report = population.export_to_usd_file(
            stage, str(directory / "transforms.usda"), ordinal=1,
            attribute_rules=["omni:xform"],
        )
        print("Transforms exported:", report["transforms_exported"])
        # Transforms exported: 1
        # [/snippet:export-file]

        # [snippet:export-lifetime]
        usd = population.ExportDestination.create(stage, str(directory / "discarded.usda"))
        try:
            usd.export(ordinal=1, attribute_rules=["omni:xform"])
            # No save: discard this export when the destination closes.
        finally:
            usd.close()  # Close before destroying the source stage.
        # [/snippet:export-lifetime]
        assert not (directory / "discarded.usda").exists()

        # [snippet:export-reuse]
        with population.ExportDestination.create(stage, str(directory / "combined.usda")) as usd:
            usd.export(ordinal=1, attribute_rules=["omni:xform"])
            usd.export(ordinal=1, attribute_rules=[{
                "source_attribute_name": "userProperties:temperature",
                "usd_type_name": "float",
            }])
            # Save both exports together. The destination remains open until block exit.
            usd.save()
        # [/snippet:export-reuse]

        # [snippet:export-custom]
        population.export_to_usd_file(
            stage, str(directory / "custom.usda"), ordinal=1,
            attribute_rules=[{
                "source_attribute_name": "userProperties:*",
                "source_attribute_name_match": population.OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB,
                # Infer the USD type and fail rather than silently skip this data.
                "flags": (
                    population.OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE
                    | population.OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED
                ),
            }],
        )
        # [/snippet:export-custom]

        # [snippet:export-rule]
        radius_rule = {
            "source_attribute_name": "solver:collisionRadius",
            "destination_attribute_name": "radius",
            "usd_type_name": "double",
            "flags": population.OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED,
        }
        # [/snippet:export-rule]

        # [snippet:export-rename]
        population.export_to_usd_file(
            stage, str(directory / "sphere.usda"), ordinal=1,
            layer_mode=population.OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF,
            attribute_rules=[radius_rule],
        )
        # The recorded usd-prim-type creates a Sphere with radius 2.5.
        # [/snippet:export-rename]

        # [snippet:export-selection]
        population.export_to_usd_file(
            stage, str(directory / "selected.usda"), ordinal=1,
            prim_predicate=population.PrimPredicate.has_path("/World/Ball"),
            property_predicate=population.PropertyPredicate.in_namespace("userProperties"),
            attribute_rules=[{
                "source_attribute_name": "userProperties:temperature",
                "usd_type_name": "float",
            }],
        )
        # [/snippet:export-selection]

        # [snippet:export-matrix]
        population.export_to_usd_file(
            stage, str(directory / "matrix.usda"), ordinal=1,
            layer_mode=population.OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF,
            transform_policy=population.OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_MATRIX_OP,
            attribute_rules=["omni:xform"],
        )
        # Creates a Sphere with a matrix xform operation instead of decomposed operations.
        # [/snippet:export-matrix]

        # [snippet:export-report]
        print("Attributes exported:", report["attributes_exported"])
        # Attributes exported: 1
        try:
            # The current watermark is 1; ordinal 0 cannot select an older snapshot.
            population.export_to_usd_file(
                stage, str(directory / "stale.usda"), ordinal=0,
                attribute_rules=["omni:xform"],
            )
        except OvstageError:
            print("Stale export rejected")
            # Stale export rejected
        else:
            raise RuntimeError("Expected the stale ordinal to fail")
        # [/snippet:export-report]

        # [snippet:export-async]
        operation = population.export_to_usd_file_async(
            stage, str(directory / "async.usda"), ordinal=1,
            attribute_rules=["omni:xform"],
        )
        # This export-specific wait includes saving the file and returns its report.
        async_report = operation.wait()
        print("Async transforms exported:", async_report["transforms_exported"])
        # Async transforms exported: 1
        # [/snippet:export-async]

        # [snippet:export-changed]
        # custom.usda already holds the baseline temperature 20 at ordinal 1.
        with paths.create_path_list_from_strings(["/World/Ball"]) as selected:
            with stage.query_from_path_list(selected) as query:
                stage.write_attribute(query, "userProperties:temperature", ordinal=2,
                                      tensors=np.array([24.5], dtype=np.float32), is_array=False).wait()
        stage.advance_write_floor(ordinal=2).wait()
        changed = population.export_to_usd_file(
            stage, str(directory / "changed.usda"), ordinal=2, since_ordinal=1,
            selection=population.OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL,
            attribute_rules=[{
                "source_attribute_name": "userProperties:temperature",
                "usd_type_name": "float",
            }],
        )
        print("Changed attributes exported:", changed["attributes_exported"])
        # Changed attributes exported: 1
        # [/snippet:export-changed]

        # Confirm the example files contain the intended authored values.
        assert 'double radius = 2.5' in (directory / "sphere.usda").read_text(encoding="utf-8")
        assert 'matrix4d xformOp:transform' in (directory / "matrix.usda").read_text(encoding="utf-8")
        assert 'float userProperties:temperature = 24.5' in (directory / "changed.usda").read_text(encoding="utf-8")


def main() -> int:
    """Export a populated hierarchy, then exercise direct-write export rules."""
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
    def Xform "SimulatedMeshes"
    {
        def Mesh "Floor" (prepend apiSchemas = ["PhysicsCollisionAPI"])
        {
            point3f[] points = [(0, 0, 0), (2, 0, 0), (0, 2, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
        }
        def Mesh "Dynamic" (prepend apiSchemas = ["PhysicsCollisionAPI", "PhysicsRigidBodyAPI"])
        {
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
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
                time=math.nan,
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

            # [snippet:export-mesh]
            mesh_report = population.export_typed_hierarchy_to_usd_file(
                stage, str(Path(temp_dir) / "meshes.usda"), "/World/SimulatedMeshes", ordinal=1,
            )
            print("Mesh APIs exported:", mesh_report["api_schemas_applied"])
            # Mesh APIs exported: 5
            # [/snippet:export-mesh]
            mesh_text = (Path(temp_dir) / "meshes.usda").read_text(encoding="utf-8")
            assert 'def Mesh "Floor"' in mesh_text and 'def Mesh "Dynamic"' in mesh_text
            assert 'point3f[] points' in mesh_text and 'int[] faceVertexIndices' in mesh_text

        export_runtime_data(Path(temp_dir))

    print("saved /World/Props as typed USD to destination.usda")
    # saved /World/Props as typed USD to destination.usda
    print(
        f"report: {report['prims_exported']} prims, {report['attributes_exported']} attributes, "
        f"{report['api_schemas_applied']} applied schemas"
    )
    # report: 7 prims, 4 attributes, 4 applied schemas
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
