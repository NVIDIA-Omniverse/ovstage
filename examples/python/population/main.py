# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Populate selected USD data, inspect its representation, and advance time."""

# [snippet:population-imports]
from pathlib import Path

import ovstage
from ovstage import OrdinalRange, PopulationDomain, population
# [/snippet:population-imports]


# Run the population and readback sequence against the adjacent scene.
def main():
    # [snippet:population-setup]
    scene = str(Path(__file__).with_name("scene.usda"))
    with ovstage.Stage("example.population") as stage, ovstage.PathDictionary(stage) as paths:
        # Select physics data, including Body's rigid-body API and its ancestors.
        population.open_usd(stage, scene, ordinal=1, domains=PopulationDomain.PHYSICS)
        stage.advance_write_floor(ordinal=1).wait()
        # Continue the following examples inside these context managers.
    # [/snippet:population-setup]

        # [snippet:population-paths]
        # Select only /World/Body. is_under_path("/World") also includes its descendants.
        body_condition = population.PrimPredicate.has_path("/World/Body")
        # [/snippet:population-paths]

        # [snippet:population-property-metadata]
        # Request the values used below, plus metadata on those properties.
        selector = population.Selector(
            prim_predicate=body_condition,
            property_predicate=population.PropertyPredicate.has_name(
                "size", "temperature", "state", "points", "emptyName", "emptyArray", "extent"
            ),
            prim_metadata_paths=["customData:label"],
            property_metadata_paths=["customData:unit"],
        )
        # [/snippet:population-property-metadata]
        # [snippet:population-stage-metadata]
        desc = population.Desc(
            domains=PopulationDomain.NONE,
            selectors=[selector],
            stage_metadata_paths=["timeCodesPerSecond"],
        )
        population.open_usd_with_desc(
            stage, scene, ordinal=2, time=population.DEFAULT_TIME, descs=[desc]
        )
        stage.advance_write_floor(ordinal=2).wait()
        # [/snippet:population-stage-metadata]

        # [snippet:population-default]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("size")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(float(group.array(0).ravel()[0]))
                            # 2.0
        # [/snippet:population-default]

        # [snippet:population-numeric]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("temperature")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(float(group.array(0).ravel()[0]))
                            # 20.0
        # [/snippet:population-numeric]

        # [snippet:population-strings]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("state")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            token = int(group.array(0).ravel()[0])
                            print(paths.token_to_string(token))
                            # ready
        # [/snippet:population-strings]

        # [snippet:population-arrays]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("points")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(group.array(0).reshape(-1, 3).tolist())
                            # [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]
        # [/snippet:population-arrays]

        # [snippet:population-empty]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("emptyName")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(int(group.array(0).ravel()[0]))
                            # 0
        # [/snippet:population-empty]

        # [snippet:population-empty-array]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("emptyArray")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(group.array(0).tolist())
                            # []
        # [/snippet:population-empty-array]

        # [snippet:population-extent]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("extent")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(group.array(0).dtype, group.array(0).reshape(-1, 6).tolist())
                            # float64 [[-1.0, -1.0, -1.0, 1.0, 1.0, 1.0]]
        # [/snippet:population-extent]

        # [snippet:population-stage-metadata-read]
        with paths.create_path_list_from_strings(["/"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("usd-metadata:timeCodesPerSecond")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(float(group.array(0).ravel()[0]))
                            # 24.0
        # [/snippet:population-stage-metadata-read]

        # [snippet:population-property-metadata-read]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("temperature:usd-metadata:customData:unit")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(paths.token_to_string(int(group.array(0).ravel()[0])))
                            # C
        # [/snippet:population-property-metadata-read]

        # [snippet:population-prim-metadata-read]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("usd-metadata:customData:label")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(paths.token_to_string(int(group.array(0).ravel()[0])))
                            # robot
        # [/snippet:population-prim-metadata-read]

        # [snippet:population-reserved]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("usd-prim-type")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(paths.token_to_string(int(group.array(0).ravel()[0])))
                            # Cube
        # [/snippet:population-reserved]

        # [snippet:population-schemas]
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("usd-schemas")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print([paths.token_to_string(int(token)) for token in group.array(0).ravel()])
                            # ['PhysicsRigidBodyAPI']
        # [/snippet:population-schemas]

        # [snippet:population-ancestor]
        with paths.create_path_list_from_strings(["/World"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("omni:xform")], OrdinalRange.latest(2)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(group.array(0).reshape(4, 4)[3, :3].tolist())
                            # [10.0, 0.0, 0.0]
        # [/snippet:population-ancestor]

        # [snippet:population-time]
        # One second is USD time code 24 in this scene. The ordinal is independent.
        population.update_from_usd_time(stage, time=1.0, ordinal=3)
        stage.advance_write_floor(ordinal=3).wait()
        with paths.create_path_list_from_strings(["/World/Body"]) as selected:
            with stage.query_from_path_list(selected) as query:
                with stage.read_attributes(query, [paths.intern_token("temperature")], OrdinalRange.latest(3)) as read:
                    read.wait()
                    for group in read.groups():
                        with group:
                            print(float(group.array(0).ravel()[0]))
                            # 30.0
        # [/snippet:population-time]

        # [snippet:population-ordinals]
        # Population has already used ordinals 1, 2, and 3 above.
        for frame in range(3):
            ordinal = 4 + frame
            population.update_from_usd_time(stage, time=frame / 24.0, ordinal=ordinal)
            stage.advance_write_floor(ordinal=ordinal).wait()
            # Consumers can now read through this ordinal before the next update.
        # [/snippet:population-ordinals]


        # [snippet:population-subtree]
        # Add size for Cube prims below /World to the predefined physics selection.
        subtree_selector = population.Selector(
            prim_predicate=population.PrimPredicate.and_(
                population.PrimPredicate.is_under_path("/World"),
                population.PrimPredicate.has_type("Cube"),
            ),
            property_predicate=population.PropertyPredicate.has_name("size"),
        )
        population.open_usd_with_desc(
            stage, scene, ordinal=7, time=population.DEFAULT_TIME,
            descs=[population.Desc(domains=PopulationDomain.PHYSICS, selectors=[subtree_selector])],
        )
        stage.advance_write_floor(ordinal=7).wait()
        # [/snippet:population-subtree]

    # Use a separate stage to demonstrate a failed load without losing the scene above.
    with ovstage.Stage("example.population.failure") as stage:
        # [snippet:population-failure]
        ordinal = 1
        try:
            operation = population.open_usd_async(
                stage, scene + ".missing", ordinal=ordinal, domains=PopulationDomain.PHYSICS
            )
        except ovstage.OvstageError:
            # Rejected at submission: no writes ran, so this ordinal can be reused.
            retry_ordinal = ordinal
        else:
            try:
                operation.wait()
            except ovstage.OvstageError:
                # Accepted but failed while running: allow for partial writes.
                retry_ordinal = ordinal + 1
            else:
                raise RuntimeError("Expected the missing file to fail")
        # Fix the input and retry at the chosen ordinal.
        population.open_usd(stage, scene, ordinal=retry_ordinal, domains=PopulationDomain.PHYSICS)
        stage.advance_write_floor(ordinal=retry_ordinal).wait()
        # [/snippet:population-failure]


if __name__ == "__main__":
    main()
