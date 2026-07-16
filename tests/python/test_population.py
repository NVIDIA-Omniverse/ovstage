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

from ovstage import Filter, FilterOp, OvstageError, PopulationDomain, Predicate, population

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
