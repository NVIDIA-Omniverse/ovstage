// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage logging test: install a process-global log callback, flush, and
// clear it (works on any build), and prove a channel-prefix filter with a NONE
// default threshold suppresses delivery even while the stage does log-producing
// work. CPU-only. Tested source for the logging skill.
//
// Note: there is no public API to emit a log message on demand, so this suite does
// not assert that messages ARE delivered (a given workload may legitimately emit
// none); it asserts the callback lifecycle and suppression.

#include <ovstage/ovstage.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace
{

// The counting callback never blocks, so a bounded flush cannot hang.
constexpr ovstage_timeout_ns_t kFlushTimeoutNs = 5'000'000'000ull;  // 5 s

std::atomic<int> g_messageCount{ 0 };

void countMessages(ovstage_log_severity_t /*severity*/, double /*timestamp*/, ovx_string_t /*message*/,
                   void* userData)
{
    static_cast<std::atomic<int>*>(userData)->fetch_add(1, std::memory_order_relaxed);
}

ovx_string_t str(const char* s)
{
    return ovx_string_t{ s, std::strlen(s) };
}

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

class LoggingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.logging";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        g_messageCount.store(0);
    }

    void TearDown() override
    {
        // Always clear the process-global callback before tearing the stage down.
        ovstage_set_log_callback(OVSTAGE_LOG_NONE, nullptr, nullptr, nullptr);
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    // Best-effort log-producing work; a no-op if the population bridge is absent.
    void populate()
    {
        ovstage_population_enqueue_result_t pop = ovstage_population_open_usd_from_string(
            stage_, str(kCubeUsda), 1, NAN, OVSTAGE_POPULATION_DOMAIN_RENDERING);
        if (pop.status == OVSTAGE_OK)
            ovstage_population_wait_op(stage_, pop.op_index, OVSTAGE_TIMEOUT_INFINITE, nullptr);
    }

    ovstage_instance_t* stage_ = nullptr;
};

TEST_F(LoggingTest, CallbackLifecycle)
{
    // Install, flush, and clear — the callback API is usable on any build (a live
    // instance bootstraps the runtime; no population bridge needed).
    ASSERT_EQ(ovstage_set_log_callback(OVSTAGE_LOG_VERBOSE, nullptr, &countMessages, &g_messageCount), OVSTAGE_OK);
    EXPECT_EQ(ovstage_flush_log(kFlushTimeoutNs), OVSTAGE_OK);
    EXPECT_EQ(ovstage_set_log_callback(OVSTAGE_LOG_NONE, nullptr, nullptr, nullptr), OVSTAGE_OK);
}

TEST_F(LoggingTest, ChannelFilterSuppresses)
{
    // [snippet:log-callback-filter-c]
    // ovstage_set_log_callback installs a process-global callback (needs a live
    // instance to bootstrap the runtime). The severity is the threshold for
    // channels not named in the filter; OVSTAGE_LOG_NONE disables all of them. The
    // channel_filter is a comma-separated "<channel>=<level>" list — here a prefix
    // that matches no real channel, so nothing is delivered. ovstage_flush_log
    // forces pending messages through; a NULL callback clears it.
    ovx_string_t bogusFilter = str("this.channel.does.not.exist.42=verbose");
    ASSERT_EQ(ovstage_set_log_callback(OVSTAGE_LOG_NONE, &bogusFilter, &countMessages, &g_messageCount), OVSTAGE_OK);
    populate();  // log-producing work the filter must suppress
    ASSERT_EQ(ovstage_flush_log(kFlushTimeoutNs), OVSTAGE_OK);
    ovstage_set_log_callback(OVSTAGE_LOG_NONE, nullptr, nullptr, nullptr);  // clear
    // [/snippet:log-callback-filter-c]

    EXPECT_EQ(g_messageCount.load(), 0);
}

} // namespace
