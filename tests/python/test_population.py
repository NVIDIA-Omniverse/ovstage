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
# Public ovstage population test (USD -> ovstage): populate from inline USDA and
# query back; add/remove a USD reference; reset; and confirm a missing file fails.
# These are the tested source for the loading-usd skill's snippets. The C sibling
# is tests/c/test_population.cpp. CPU-only.

import math

import pytest

from ovstage import (Filter, FilterOp, OvstageError, PathDictionary, PopulationDomain, Predicate,
                     population)

pytestmark = pytest.mark.skipif(
    not population.available(),
    reason="libovstage was built without the ovstage population bridge",
)

# A minimal USD scene: one Cube under a World Xform.
CUBE_USDA = """#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World"
{
    def Cube "Cube"
    {
        double size = 1.0
    }
}
"""

# A scene with one prim carrying a physics schema and one that does not, so a
# description that names the schema is distinguishable from one that selects everything.
SCHEMA_USDA = """#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World"
{
    def Cube "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI"])
    {
        double size = 4.0
    }
    def Cube "Prop"
    {
        double size = 2.0
    }
    def Sphere "Ball"
    {
        double radius = 1.0
    }
}
def Cube "Outside"
{
    double size = 9.0
}
"""

# A self-contained, referenceable layer (sets defaultPrim so the reference composes).
REF_USDA = """#usda 1.0
(
    defaultPrim = "Ref"
)
def Xform "Ref"
{
    def Cube "Cube"
    {
        double size = 1.0
    }
}
"""


def _prefix_count(stage, prefix: str) -> int:
    query = stage.query(filter=Filter([Predicate("usd-path", FilterOp.PREFIX, [prefix])]))
    query.wait()
    count = stage.fetch_query_result(query).total_prim_count
    stage.release_query(query).wait()
    return count


def _publishes(stage, prefix: str, attribute: str) -> bool:
    """Whether ovstage discovered `attribute` on the prims under `prefix`."""
    with PathDictionary(stage) as paths:
        token = paths.intern_token(attribute)
        query = stage.query(filter=Filter([Predicate("usd-path", FilterOp.PREFIX, [prefix])]))
        query.wait()
        result = stage.fetch_query_result(query)
        found = token in set(result.attributes)
        stage.release_query(query).wait()
        return found


def test_populate_from_usda_and_query(stage):
    # [snippet:populate-and-query]
    # Populate the ovstage from an inline USDA string (the RENDERING domain
    # mirrors meshes/lights/materials/cameras). open_usd_from_string blocks until
    # the populate op completes. Then confirm the prim landed by querying it back
    # by its usd-path — queries resolve against the latest committed state, so no
    # write-floor advance is needed.
    population.open_usd_from_string(
        stage, CUBE_USDA, ordinal=1, time_code=math.nan, domains=PopulationDomain.RENDERING
    )
    query = stage.query(filter=Filter([Predicate("usd-path", FilterOp.IN, ["/World/Cube"])]))
    query.wait()
    matched = stage.fetch_query_result(query).total_prim_count
    stage.release_query(query).wait()
    # [/snippet:populate-and-query]

    assert matched == 1


def test_add_remove_usd_reference(stage):
    population.open_usd_from_string(stage, CUBE_USDA, ordinal=1, domains=PopulationDomain.RENDERING)

    # [snippet:usd-reference]
    # add_usd_reference edits the USD source only; apply_usd_changes propagates it
    # into the stage (at the ordinal you pass). Each add reserves a handle for a
    # later remove; removing again propagates the tombstone.
    handle = population.add_usd_reference_from_string(stage, REF_USDA, "/World/Props")
    population.apply_usd_changes(stage, ordinal=2)
    assert _prefix_count(stage, "/World/Props") > 0  # the referenced subtree materialized

    population.remove_usd(stage, handle)
    population.apply_usd_changes(stage, ordinal=3)
    assert _prefix_count(stage, "/World/Props") == 0  # the referenced subtree was removed
    # [/snippet:usd-reference]

    # The handle is spent: removing it again is rejected.
    with pytest.raises(OvstageError):
        population.remove_usd(stage, handle)


def test_reset_usd_and_repopulate(stage):
    population.open_usd_from_string(stage, CUBE_USDA, ordinal=1, domains=PopulationDomain.RENDERING)
    assert _prefix_count(stage, "/World") > 0

    # [snippet:reset-usd]
    # reset_usd clears the USD source; apply_usd_changes propagates the cleared
    # state into the stage. The stage stays usable afterwards — repopulating from
    # USD still works.
    population.reset_usd(stage)
    population.apply_usd_changes(stage, ordinal=2)
    assert _prefix_count(stage, "/World") == 0  # cleared before repopulating
    population.open_usd_from_string(stage, CUBE_USDA, ordinal=3, domains=PopulationDomain.RENDERING)
    # [/snippet:reset-usd]

    assert _prefix_count(stage, "/World") > 0


def test_open_missing_file_fails(stage):
    # [snippet:open-missing-file]
    # A missing/unreadable file fails the populate op. The blocking open_usd
    # raises OvstageError (the async variant surfaces it from .wait()).
    with pytest.raises(OvstageError):
        population.open_usd(stage, "/nonexistent/ovstage-does-not-exist.usda", ordinal=1)
    # [/snippet:open-missing-file]


def test_populate_by_schema(stage):
    """Populate exactly the prims carrying a schema, with the properties it declares."""
    # [snippet:populate-by-schema]
    # A description says what to populate. The prim predicate picks the prims -- here
    # anything conforming to PhysicsRigidBodyAPI, whether as its type or as an applied
    # API schema -- and the property predicate picks which of their properties are
    # published. The same schema list serves both halves.
    schemas = ["PhysicsRigidBodyAPI"]
    selector = population.Selector(
        prim_predicate=population.PrimPredicate.has_schema(*schemas),
        property_predicate=population.PropertyPredicate.declared_by_schema(*schemas),
    )
    population.open_usd_from_string_with_desc(
        stage, SCHEMA_USDA, ordinal=1, time_code=math.nan,
        descs=[population.Desc(selectors=[selector])],
    )
    # [/snippet:populate-by-schema]

    # The prim carrying the schema is populated; the one that does not is not.
    assert _prefix_count(stage, "/World/Body") == 1
    assert _prefix_count(stage, "/World/Prop") == 0


def test_populate_narrowed_to_a_subtree(stage):
    """Combine predicates: the same selection, scoped to one branch of the stage."""
    # [snippet:populate-narrowed]
    # Predicates compose with and_ / or_ / not_. Several values of one kind already
    # mean "any of these", so AND is how conditions of *different* kinds combine.
    selector = population.Selector(
        prim_predicate=population.PrimPredicate.and_(
            population.PrimPredicate.has_type("Cube"),
            population.PrimPredicate.is_under_path("/World"),
        ),
        property_predicate=population.PropertyPredicate.has_name("size"),
    )
    population.open_usd_from_string_with_desc(
        stage, SCHEMA_USDA, ordinal=1, time_code=math.nan,
        descs=[population.Desc(selectors=[selector])],
    )
    # [/snippet:populate-narrowed]

    assert _prefix_count(stage, "/World/Body") == 1
    assert _prefix_count(stage, "/World/Prop") == 1
    
    assert _prefix_count(stage, "/World/Ball") == 0
    assert _prefix_count(stage, "/Outside") == 0


def test_populate_stage_metadata(stage):
    """Stage metadata is populated onto the root prim when a description asks for it."""
    # [snippet:populate-stage-metadata]
    # Stage metadata lands on the root prim, `/`, under the `usd-metadata:` prefix. A
    # path is a field name, extended with a `:`-joined key path to reach inside a
    # dictionary field.
    population.open_usd_from_string_with_desc(
        stage, SCHEMA_USDA,
        descs=[population.Desc(stage_metadata_paths=["timeCodesPerSecond"])],
        ordinal=1, time_code=math.nan,
    )
    # [/snippet:populate-stage-metadata]

    assert _prefix_count(stage, "/") >= 1


def test_descriptions_from_several_contributors(stage):
    """Descs compose, so each library can supply its own without agreeing on one."""
    # [snippet:populate-several-descs]
    # `domains` is OR-ed across the descriptions and their selectors are combined, so a
    # host can collect one description per library and pass them together.
    renderer = population.Desc(domains=PopulationDomain.RENDERING)
    mine = population.Desc(
        selectors=[
            population.Selector(
                prim_predicate=population.PrimPredicate.has_schema("PhysicsRigidBodyAPI"),
                property_predicate=population.PropertyPredicate.declared_by_schema("PhysicsRigidBodyAPI"),
            )
        ]
    )
    population.open_usd_from_string_with_desc(
        stage, SCHEMA_USDA, ordinal=1, time_code=math.nan, descs=[renderer, mine]
    )
    # [/snippet:populate-several-descs]

    assert _prefix_count(stage, "/World/Body") == 1
    assert _publishes(stage, "/World/Body", "physics:velocity")
    assert not _publishes(stage, "/World/Prop", "physics:velocity")
