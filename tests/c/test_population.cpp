// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
// property and proprietary rights in and to this material, related
// documentation and any modifications thereto. Any use, reproduction,
// disclosure or distribution of this material and related documentation
// without an express license agreement from NVIDIA CORPORATION or
// its affiliates is strictly prohibited.
//
// Public ovstage population test (USD -> ovstage): populate from inline USDA and
// query back; add/remove a USD reference; reset; and confirm a missing file
// fails. Tested source for the loading-usd skill's C snippets; the Python sibling
// is tests/python/test_population.py. CPU-only.

#include <ovstage/ovstage.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

namespace
{

ovx_string_t str(const char* s)
{
    return ovx_string_t{ s, std::strlen(s) };
}

// Drive an ordinary (non-population) enqueue to completion.
bool waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq)
{
    if (enq.status != OVSTAGE_OK)
        return false;
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t status = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    ovstage_release_op(stage, enq.op_index);
    return status == OVSTAGE_OK && wait.error_op_id_count == 0;
}

// Drive a population enqueue to completion; true iff it ran error-free.
bool waitPopOk(ovstage_instance_t* stage, ovstage_population_enqueue_result_t pop, const char* what)
{
    if (pop.status != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " enqueue rejected (code " << pop.status << ")";
        return false;
    }
    ovstage_population_op_wait_result_t wait{};
    const ovstage_api_status_t st = ovstage_population_wait_op(stage, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    if (st != OVSTAGE_OK || wait.error_op_id_count != 0)
    {
        ADD_FAILURE() << what << " op failed (code " << st << ")";
        return false;
    }
    return true;
}

// A population op expected to FAIL (rejected at enqueue, or failed at wait).
bool waitPopFails(ovstage_instance_t* stage, ovstage_population_enqueue_result_t pop)
{
    if (pop.status != OVSTAGE_OK)
        return true;
    ovstage_population_op_wait_result_t wait{};
    const ovstage_api_status_t st = ovstage_population_wait_op(stage, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    return st != OVSTAGE_OK || wait.error_op_id_count != 0;
}

// A minimal USD scene: one Cube under a World Xform.
const char kCubeUsda[] =
    "#usda 1.0\n"
    "(\n"
    "    defaultPrim = \"World\"\n"
    ")\n"
    "def Xform \"World\"\n"
    "{\n"
    "    def Cube \"Cube\"\n"
    "    {\n"
    "        double size = 1.0\n"
    "    }\n"
    "}\n";

// A self-contained, referenceable layer (defaultPrim set so the reference composes).
const char kRefUsda[] =
    "#usda 1.0\n"
    "(\n"
    "    defaultPrim = \"Ref\"\n"
    ")\n"
    "def Xform \"Ref\"\n"
    "{\n"
    "    def Cube \"Cube\"\n"
    "    {\n"
    "        double size = 1.0\n"
    "    }\n"
    "}\n";

class PopulationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.population";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        ASSERT_NE(stage_, nullptr);
    }

    void TearDown() override
    {
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    // Count prims whose usd-path starts with `prefix` (latest committed state).
    size_t prefixCount(const char* prefix)
    {
        ovx_string_t value = str(prefix);
        ovstage_predicate_t pred{};
        pred.attribute.string = str("usd-path");
        pred.op = OVSTAGE_FILTER_OP_PREFIX;
        pred.values = &value;
        pred.value_count = 1;
        ovstage_filter_t filter{};
        filter.predicates = &pred;
        filter.count = 1;

        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        if (!waitOp(stage_, ovstage_query(stage_, &filter, nullptr, 0, &query)))
        {
            if (query != OVSTAGE_INVALID_QUERY_HANDLE)
                waitOp(stage_, ovstage_release_query(stage_, query));
            return 0;
        }
        ovstage_query_result_t result{};
        size_t count = 0;
        if (ovstage_fetch_query_result(stage_, query, OVSTAGE_TIMEOUT_INFINITE, &result) == OVSTAGE_OK)
        {
            count = result.total_prim_count;
            ovstage_release_query_result(stage_, &result);
        }
        waitOp(stage_, ovstage_release_query(stage_, query));
        return count;
    }

    ovstage_instance_t* stage_ = nullptr;
};

TEST_F(PopulationTest, PopulateFromUsdaAndQuery)
{
    size_t matched = 0;
    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;

    // [snippet:populate-and-query-c]
    // Populate the ovstage from an inline USDA string (the RENDERING domain
    // mirrors meshes/lights/materials/cameras). Population is asynchronous — wait
    // for the op. Then confirm the prim landed by querying it back by its
    // usd-path; queries resolve against the latest committed state.
    ovstage_population_enqueue_result_t pop = ovstage_population_open_usd_from_string(
        stage_, str(kCubeUsda), /*ordinal=*/1, /*time=*/0.0, OVSTAGE_POPULATION_DOMAIN_RENDERING);
    ASSERT_EQ(pop.status, OVSTAGE_OK);
    ASSERT_EQ(ovstage_population_wait_op(stage_, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, nullptr), OVSTAGE_OK);

    ovx_string_t pathValue = str("/World/Cube");
    ovstage_predicate_t predicate{};
    predicate.attribute.string = str("usd-path");
    predicate.op = OVSTAGE_FILTER_OP_IN;
    predicate.values = &pathValue;
    predicate.value_count = 1;

    ovstage_filter_t filter{};
    filter.predicates = &predicate;
    filter.count = 1;

    const bool queryOk = waitOp(stage_, ovstage_query(stage_, &filter, nullptr, 0, &query));
    if (!queryOk)
    {
        if (query != OVSTAGE_INVALID_QUERY_HANDLE)
            waitOp(stage_, ovstage_release_query(stage_, query));
        ASSERT_TRUE(queryOk);
    }

    ovstage_query_result_t result{};
    const ovstage_api_status_t fetched = ovstage_fetch_query_result(stage_, query, OVSTAGE_TIMEOUT_INFINITE, &result);
    if (fetched == OVSTAGE_OK)
    {
        matched = result.total_prim_count;
        ovstage_release_query_result(stage_, &result);
    }
    // [/snippet:populate-and-query-c]

    ASSERT_TRUE(waitOp(stage_, ovstage_release_query(stage_, query)));
    ASSERT_EQ(fetched, OVSTAGE_OK);

    EXPECT_EQ(matched, 1u);
}

TEST_F(PopulationTest, AddRemoveUsdReference)
{
    ASSERT_TRUE(waitPopOk(stage_,
        ovstage_population_open_usd_from_string(stage_, str(kCubeUsda), 1, NAN, OVSTAGE_POPULATION_DOMAIN_RENDERING),
        "open"));

    // [snippet:usd-reference-c]
    // add_usd_reference edits the USD source only; apply_usd_changes propagates it
    // into the stage (at the ordinal you pass). The add reserves a handle
    // synchronously for a later remove; removing again propagates the tombstone.
    ovstage_population_usd_reference_handle_t handle = OVSTAGE_POPULATION_INVALID_USD_REFERENCE_HANDLE;
    ASSERT_TRUE(waitPopOk(stage_,
        ovstage_population_add_usd_reference_from_string(stage_, str(kRefUsda), str("/World/Props"), &handle),
        "add_reference"));
    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_apply_usd_changes(stage_, 2), "apply_usd_changes"));
    EXPECT_GT(prefixCount("/World/Props"), 0u);  // the referenced subtree materialized

    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_remove_usd_reference(stage_, handle), "remove_reference"));
    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_apply_usd_changes(stage_, 3), "apply_usd_changes"));
    EXPECT_EQ(prefixCount("/World/Props"), 0u);  // the referenced subtree was removed
    // [/snippet:usd-reference-c]

    // The handle is spent: removing it again fails.
    EXPECT_TRUE(waitPopFails(stage_, ovstage_population_remove_usd_reference(stage_, handle)));
}

TEST_F(PopulationTest, ResetUsdAndRepopulate)
{
    ASSERT_TRUE(waitPopOk(stage_,
        ovstage_population_open_usd_from_string(stage_, str(kCubeUsda), 1, NAN, OVSTAGE_POPULATION_DOMAIN_RENDERING),
        "open"));
    EXPECT_GT(prefixCount("/World"), 0u);

    // [snippet:reset-usd-c]
    // reset_usd clears the USD source; apply_usd_changes propagates the cleared
    // state. The stage stays usable afterwards — repopulating from USD still works.
    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_reset_usd(stage_), "reset_usd"));
    ASSERT_TRUE(waitPopOk(stage_, ovstage_population_apply_usd_changes(stage_, 2), "apply_usd_changes"));
    EXPECT_EQ(prefixCount("/World"), 0u);  // cleared before repopulating
    ASSERT_TRUE(waitPopOk(stage_,
        ovstage_population_open_usd_from_string(stage_, str(kCubeUsda), 3, NAN, OVSTAGE_POPULATION_DOMAIN_RENDERING),
        "reopen"));
    // [/snippet:reset-usd-c]

    EXPECT_GT(prefixCount("/World"), 0u);
}

TEST_F(PopulationTest, OpenMissingFileFails)
{
    // [snippet:open-missing-file-c]
    // A missing/unreadable file fails the populate op: the enqueue is accepted but
    // the failure surfaces from ovstage_population_wait_op.
    EXPECT_TRUE(waitPopFails(stage_,
        ovstage_population_open_usd_from_file(
            stage_, str("/nonexistent/ovstage-does-not-exist.usda"), 1, NAN, OVSTAGE_POPULATION_DOMAIN_RENDERING)));
    // [/snippet:open-missing-file-c]
}

} // namespace
