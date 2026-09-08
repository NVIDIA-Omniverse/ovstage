# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Normal public workflows print nothing to the console.

ovstage's contract routes diagnostics through the opt-in log callback
(:func:`ovstage.set_log_callback`) and failures through status codes and error
strings — a client that installs neither gets a silent console. This held only
partially in practice: USD-support-layer diagnostics used to fall through to a
default stderr printer whose message format embeds build-machine source paths of
the emitting component, and other runtime components could print bracketed
log-prefix lines.

The child below runs the same USD-backed flow as the shipped runtime-loop
example (open a real on-disk layer, populate, advance the write floor) with no
log callback installed, and its console must stay clean: stderr empty, stdout
free of diagnostic-printer signatures and build-machine paths.

Each case needs its own child process: the guarantee under test is about the
*default* console state of a fresh consumer process.
"""

import re
import subprocess
import sys

import pytest

_SCENE = """#usda 1.0
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

_CHILD = """import pathlib
import sys

import ovstage
from ovstage import PopulationDomain, population

scene = pathlib.Path(sys.argv[1])

with ovstage.Stage("test.ovstage.output-cleanliness") as stage:
    if population.available():
        population.open_usd(
            stage, str(scene), ordinal=1, time_code=0.0, domains=PopulationDomain.RENDERING
        )
        stage.advance_write_floor(ordinal=1).wait()
        print("POPULATED")
print("CLEANLINESS_OK")
"""

# Signatures that must never appear on a default-configured console:
# - build-machine checkout roots baked into diagnostics ("/builds/", "\\builds\\")
# - the USD diagnostic default-printer format ("<Kind>: in <function> at line ...",
#   with an optional "(secondary thread)" tag)
# - the console logger's "[<severity>] [<component>]" prefixes
_FORBIDDEN = (
    "/builds/",
    "\\builds\\",
    "Status: in ",
    "Status (secondary thread)",
    "Warning: in ",
    "Warning (secondary thread)",
    "Error: in ",
    "Error (secondary thread)",
    "Coding Error",
    "[Verbose] [",
    "[Info] [",
    "[Warning] [",
    "[Error] [",
    "[Fatal] [",
)

# OpenUSD announces host-environment setting overrides (e.g. a runner exporting
# PXR_WORK_THREAD_LIMIT) with a '#'-framed stderr banner at library load. That
# is host-local configuration output caused by the host's own environment, not
# package-originated diagnostics — the silence guarantee under test — so those
# banner lines are filtered before asserting emptiness. The forbidden-signature
# scan still runs over the RAW streams.
_HOST_ENV_BANNER_LINE = re.compile(r"^#+$|^#\s.*\bis overridden\b.*#$")


def _without_host_env_banners(stderr):
    kept = [
        line
        for line in stderr.splitlines()
        if line.strip() and not _HOST_ENV_BANNER_LINE.match(line.strip())
    ]
    return "\n".join(kept)


def _assert_clean(result):
    assert result.returncode == 0 and "CLEANLINESS_OK" in result.stdout, (
        f"child failed: returncode={result.returncode}\n"
        f"stdout={result.stdout!r}\nstderr={result.stderr!r}"
    )
    # The produced package ships the population bridge; a child that skipped the
    # USD-backed flow would make this test pass without covering the leak path.
    assert "POPULATED" in result.stdout, (
        f"population bridge unavailable — the USD-backed leak path was not exercised:\n"
        f"stdout={result.stdout!r}"
    )
    assert _without_host_env_banners(result.stderr) == "", (
        f"default console must be silent (host-env override banners excepted), got stderr:\n"
        f"{result.stderr}"
    )
    for needle in _FORBIDDEN:
        assert needle not in result.stdout and needle not in result.stderr, (
            f"diagnostic signature {needle!r} leaked:\n"
            f"stdout={result.stdout!r}\nstderr={result.stderr!r}"
        )


@pytest.mark.usefixtures("ovstage_mod")
def test_usd_backed_workflow_prints_nothing_by_default(tmp_path):
    scene = tmp_path / "cleanliness.usda"
    scene.write_text(_SCENE)
    child = tmp_path / "cleanliness_child.py"
    child.write_text(_CHILD)
    result = subprocess.run(
        [sys.executable, "-u", str(child), str(scene)],
        capture_output=True,
        text=True,
        timeout=300,
    )
    _assert_clean(result)
