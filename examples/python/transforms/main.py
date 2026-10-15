# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

# [snippet:transform-imports]
import pathlib
import numpy as np
import ovstage
from ovstage import AttributeSemantic, HierarchyComputationModel, OrdinalRange, population
# [/snippet:transform-imports]

def main():
    """Populate a hierarchy, change inheritance, and read the computed matrix."""
    with ovstage.Stage("example.transforms") as stage, ovstage.PathDictionary(stage) as paths:
        # Include every Xform so population seeds the hierarchy and output rows.
        selector = population.Selector(prim_predicate=population.PrimPredicate.has_type("Xform"))
        population.open_usd_with_desc(stage, str(pathlib.Path(__file__).with_name("car.usda")),
                                     ordinal=1, time=0.0, descs=[population.Desc(selectors=[selector])])
        stage.advance_write_floor(ordinal=1).wait()
        with paths.create_path_list_from_strings(["/World/Car/Wheel"]) as plist:
            with stage.query_from_path_list(plist) as query:
                # [snippet:transform-layout]
                # One row-major matrix for the wheel; translation is in the last row.
                matrices = np.eye(4, dtype=np.float64)[None, :, :]
                matrices[0, 3, :3] = [4, 0, 0]
                # A boolean per prim controls whether the parent transform is inherited.
                reset = np.array([False], dtype=np.bool_)
                # [/snippet:transform-layout]

                # [snippet:transform-write]
                # Both attributes describe the same update, at ordinal 2.
                stage.write_attribute(query, "omni:xform", ordinal=2, tensors=matrices,
                                      is_array=False, semantic=AttributeSemantic.MATRIX).wait()
                stage.write_attribute(query, "omni:resetXformStack", ordinal=2,
                                      tensors=reset, is_array=False).wait()
                # False keeps the wheel relative to the car. True would make it world-relative.
                # [/snippet:transform-write]

                # [snippet:transform-compute]
                # CPU incremental computation supports this USD-populated hierarchy.
                model = HierarchyComputationModel.CPU_INCREMENTAL
                if model not in [item.model_id for item in stage.get_hierarchy_computation_models()]:
                    raise RuntimeError("CPU incremental hierarchy computation is unavailable")
                stage.compute_hierarchy(input_ordinal=2, output_ordinal=2, model=model)
                stage.advance_write_floor(ordinal=2).wait()
                # Car x=10 plus wheel local x=4 gives wheel world x=14.
                # [/snippet:transform-compute]

                # [snippet:transform-override]
                # Override the runtime reset without editing the source USD's xformOpOrder.
                stage.write_attribute(query, "omni:resetXformStack", ordinal=3,
                                      tensors=np.array([True], dtype=np.bool_), is_array=False).wait()
                stage.compute_hierarchy(input_ordinal=3, output_ordinal=3, model=model)
                stage.advance_write_floor(ordinal=3).wait()
                # The unchanged matrix now puts the wheel at world x=4, ignoring the car.
                # [/snippet:transform-override]

                # [snippet:transform-read]
                # Read the derived output only after computation; never write it directly.
                world = paths.intern_token("omni:fabric:worldMatrix")
                found = False
                with stage.read_attributes(query, [world], OrdinalRange.latest(3)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            world_matrices = group.array(0).reshape(-1, 4, 4)
                            for i in range(group.prim_count):
                                row = group.data_row_index(i) if group.has_data_index_map else i
                                print("Wheel world x:", float(world_matrices[row, 3, 0]))
                                # Wheel world x: 4.0
                                found = True
                if not found:
                    print("No derived world-matrix row is available.")
                    # No derived world-matrix row is available.
                # [/snippet:transform-read]


if __name__ == "__main__":
    main()
