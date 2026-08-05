# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""ovstage is usable however the host process was started.

A Python process can be launched in many shapes: a script file, an inline `-c`
command, arguments carrying arbitrary text. None of that should change whether
ovstage imports and opens a stage.

Each case needs its own child process, because a process's command line is fixed
before Python starts. The children run unbuffered so their output survives an
abnormal exit.

Keep the first line of the payload non-empty. A payload that begins with a blank
line puts the line break on an argument boundary rather than inside an argument,
which is a weaker case than these tests are here to cover.
"""

import subprocess
import sys

import pytest

_CHILD = """import ovstage

ovstage.library_version()
with ovstage.Stage("test.ovstage.launch"):
    pass
print("STARTUP_OK")
"""


def _run(argv):
    return subprocess.run([sys.executable, "-u", *argv], capture_output=True, text=True, timeout=300)


def _assert_started(result):
    assert result.returncode == 0 and "STARTUP_OK" in result.stdout, (
        f"ovstage did not start: returncode={result.returncode} "
        f"(a negative value means the process was killed by a signal)\n"
        f"stdout={result.stdout!r}\nstderr={result.stderr!r}"
    )


@pytest.mark.usefixtures("ovstage_mod")
def test_usable_from_a_multiline_inline_command():
    _assert_started(_run(["-c", _CHILD]))


@pytest.mark.usefixtures("ovstage_mod")
def test_usable_when_an_argument_spans_multiple_lines(tmp_path):
    script = tmp_path / "startup_child.py"
    script.write_text(_CHILD)
    _assert_started(_run([str(script), "embedded\nnewline"]))
