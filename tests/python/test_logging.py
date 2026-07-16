# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage logging test: install a process-global log callback, flush, and
# clear it (works on any build), and prove a channel-prefix filter with a NONE
# default threshold suppresses delivery even while the stage does log-producing
# work. CPU-only. Tested source for the logging skill.
#
# Note: there is no public API to emit a log message on demand, so this suite does
# not assert that messages ARE delivered (some builds or workloads may legitimately
# emit none); it asserts the callback lifecycle and suppression.

import math

from ovstage import LogSeverity, PopulationDomain, flush_log, population, set_log_callback

# The counting/collecting callbacks never block, so a bounded flush cannot hang.
FLUSH_TIMEOUT_NS = 5_000_000_000  # 5 s

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


def _populate(stage):
    """Best-effort log-producing work; a no-op without the population bridge."""
    if population.available():
        population.open_usd_from_string(
            stage, CUBE_USDA, ordinal=1, time_code=math.nan, domains=PopulationDomain.RENDERING
        )


def test_log_callback_lifecycle(stage):
    """Install, flush, and clear a log callback. Requires a live Stage to bootstrap
    the runtime; needs no population bridge. Any delivered message is well-formed."""
    seen = []

    def on_log(severity, timestamp, message):
        seen.append((severity, message))

    set_log_callback(on_log, severity=LogSeverity.VERBOSE)
    try:
        assert flush_log(FLUSH_TIMEOUT_NS) is True  # drains (nothing buffered blocks)
    finally:
        set_log_callback(None)  # clear: flushes and tears the dispatcher down

    for severity, message in seen:
        assert isinstance(severity, LogSeverity)
        assert isinstance(message, str)


def test_log_callback_channel_filter_suppresses(stage):
    """A high default threshold (NONE disables all unmatched channels) plus a
    channel filter that matches nothing delivers no message, even under log-producing
    work."""
    count = [0]

    def on_log(severity, timestamp, message):
        count[0] += 1

    # [snippet:log-callback-filter]
    # set_log_callback installs a process-global callback (requires a live Stage to
    # bootstrap the runtime). The default severity is the threshold for channels not
    # named in channel_filter; LogSeverity.NONE disables all of them. channel_filter
    # is a comma-separated "<channel>=<level>" list — here a prefix that matches no
    # real channel, so nothing is delivered. flush_log forces pending messages
    # through; passing None clears the callback.
    set_log_callback(
        on_log, severity=LogSeverity.NONE, channel_filter="this.channel.does.not.exist.42=verbose"
    )
    try:
        _populate(stage)  # log-producing work the filter must suppress
        assert flush_log(FLUSH_TIMEOUT_NS) is True
    finally:
        set_log_callback(None)
    # [/snippet:log-callback-filter]

    assert count[0] == 0
