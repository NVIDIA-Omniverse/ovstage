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
from ovstage._src import bindings

assert bindings._loader._version is None
build_info = ovstage.get_build_info()
assert build_info.build_string and build_info.git_hash
assert bindings._loader._version is None
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

_ORDINAL_POLICY_CHILD = """import ovstage

policy_name = {policy_name!r}
policy = getattr(ovstage.OrdinalAdmissionPolicy, policy_name) if policy_name is not None else None
config = ovstage.StageConfig(runtime_default_ordinal_admission_policy=policy) if policy is not None else None
resolved = (
    ovstage.OrdinalAdmissionPolicy.NONDECREASING
    if policy in (None, ovstage.OrdinalAdmissionPolicy.DEFAULT)
    else policy
)
conflicting = (
    ovstage.OrdinalAdmissionPolicy.NONDECREASING
    if resolved == ovstage.OrdinalAdmissionPolicy.FLOOR_ONLY
    else ovstage.OrdinalAdmissionPolicy.FLOOR_ONLY
)
with ovstage.Stage("startup.first-policy", config=config) as first:
    original = first.config
    assert original.runtime_default_ordinal_admission_policy == policy
    for requested in (None, ovstage.OrdinalAdmissionPolicy.DEFAULT, resolved):
        inherited_config = ovstage.StageConfig(runtime_default_ordinal_admission_policy=requested)
        with ovstage.Stage("startup.inherited-policy", config=inherited_config) as inherited:
            assert inherited.config == original
    try:
        with ovstage.Stage(
            "startup.conflicting-policy",
            config=ovstage.StageConfig(runtime_default_ordinal_admission_policy=conflicting),
        ):
            raise AssertionError("a conflicting process policy was accepted")
    except ovstage.OvstageError as error:
        assert error.code == ovstage.ErrorCode.INVALID_ARGUMENT
    assert first.config == original

with ovstage.Stage(
    "startup.policy-after-release",
    config=ovstage.StageConfig(runtime_default_ordinal_admission_policy=conflicting),
) as reset:
    assert reset.config.runtime_default_ordinal_admission_policy == conflicting
print("STARTUP_OK")
"""


@pytest.mark.usefixtures("ovstage_mod")
@pytest.mark.parametrize("policy_name", [None, "DEFAULT", "FLOOR_ONLY"])
def test_ordinal_admission_selection_in_a_fresh_process(policy_name):
    _assert_started(_run(["-c", _ORDINAL_POLICY_CHILD.format(policy_name=policy_name)]))
