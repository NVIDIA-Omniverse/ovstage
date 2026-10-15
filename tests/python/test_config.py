# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Public Python contract coverage for process-scoped stage configuration."""

import numpy as np
import pytest


def test_configure_runtime_default_hierarchy_model(ovstage_mod):
    del ovstage_mod  # Fixture verifies that the produced wheel and native library load.

    # [snippet:configure-transform-updates]
    import ovstage

    config = ovstage.StageConfig(
        runtime_default_hierarchy_computation_model=(
            ovstage.HierarchyComputationModel.CPU_INCREMENTAL
        )
    )

    with ovstage.Stage("simulation", config=config) as stage:
        # A manual computation can select the same configured model.
        stage.compute_hierarchy(
            input_ordinal=1,
            output_ordinal=1,
            model=ovstage.HierarchyComputationModel.RUNTIME_DEFAULT,
        )
    # [/snippet:configure-transform-updates]


def test_concrete_config_entry_reaches_runtime(ovstage_mod):
    cpu_config = ovstage_mod.StageConfig(
        runtime_default_hierarchy_computation_model=(
            ovstage_mod.HierarchyComputationModel.CPU_INCREMENTAL
        )
    )
    gpu_config = ovstage_mod.StageConfig(
        runtime_default_hierarchy_computation_model=(
            ovstage_mod.HierarchyComputationModel.GPU_INCREMENTAL
        )
    )

    with ovstage_mod.Stage("config.cpu", config=cpu_config) as stage:
        assert stage.config == cpu_config

        # The conflicting GPU entry is rejected before a GPU-backed stage is
        # created, so this remains a CPU-only public contract test.
        with pytest.raises(ovstage_mod.OvstageError) as excinfo:
            ovstage_mod.Stage("config.gpu-conflict", config=gpu_config)
        assert excinfo.value.code == ovstage_mod.ErrorCode.INVALID_ARGUMENT


def test_configure_ordinal_admission(ovstage_mod):
    del ovstage_mod  # Fixture verifies that the produced wheel and native library load.

    # [snippet:configure-ordinal-admission]
    import ovstage

    config = ovstage.StageConfig(
        runtime_default_ordinal_admission_policy=ovstage.OrdinalAdmissionPolicy.NONDECREASING
    )
    with ovstage.Stage("simulation", config=config) as stage:
        assert (
            stage.config.runtime_default_ordinal_admission_policy
            == ovstage.OrdinalAdmissionPolicy.NONDECREASING
        )
    # [/snippet:configure-ordinal-admission]


@pytest.mark.parametrize(
    "policy_name, admit_older, admit_equal",
    [
        ("FLOOR_ONLY", True, True),
        ("NONDECREASING", False, True),
        ("STRICTLY_INCREASING", False, False),
        ("DEFAULT", False, True),
        (None, False, True),
    ],
)
def test_ordinal_policy_controls_completed_writes(ovstage_mod, policy_name, admit_older, admit_equal):
    policy = getattr(ovstage_mod.OrdinalAdmissionPolicy, policy_name) if policy_name is not None else None
    config = ovstage_mod.StageConfig(runtime_default_ordinal_admission_policy=policy)
    with ovstage_mod.Stage("config.ordinal-admission", config=config) as stage:
        with ovstage_mod.PathDictionary(stage) as paths:
            with paths.create_path_list_from_strings(["/World/Admission/A", "/World/Admission/B"]) as plist:
                with stage.query_from_path_list(plist) as query:
                    expected = {}
                    # Each attribute gets its own seed so the older, equal, and
                    # newer attempts all compare against exactly ordinal 5.
                    for ordinal, admitted in ((4, admit_older), (5, admit_equal), (6, True)):
                        attr = paths.intern_token(f"score_{ordinal}")
                        stage.write_attribute(
                            query, attr, ordinal=5, tensors=np.array([10.0, 20.0], np.float32),
                            is_array=False, mask=[0b01], count=2,
                        ).wait()
                        op = stage.write_attribute(
                            query, attr, ordinal=ordinal, tensors=np.array([99.0, 99.0], np.float32),
                            is_array=False, mask=[0b01], count=2,
                        )
                        if admitted:
                            op.wait()
                        else:
                            # Admission is checked during execution. Inspecting
                            # enqueue success alone would miss a policy failure.
                            with pytest.raises(ovstage_mod.OvstageError) as excinfo:
                                op.wait()
                            assert excinfo.value.code == ovstage_mod.ErrorCode.OP_FAILED

                        # Even the strict policy permits a disjoint cell at the
                        # same ordinal as a prior committed change.
                        stage.write_attribute(
                            query, attr, ordinal=5, tensors=np.array([10.0, 20.0], np.float32),
                            is_array=False, mask=[0b10], count=2,
                        ).wait()
                        expected[attr] = {0: 99.0 if admitted else 10.0, 1: 20.0}

                    stage.advance_write_floor(ordinal=6).wait()
                    for attr, values in expected.items():
                        with stage.read_attributes(query, [attr], ovstage_mod.OrdinalRange.latest(6)) as read:
                            read.wait()
                            actual = {}
                            for group in read.groups():
                                with group:
                                    assert not group.is_delete
                                    data = group.array(0)
                                    for local in range(group.prim_count):
                                        actual[group.prim_index(local)] = float(data[group.data_row_index(local)])
                            assert actual == values

                    # Policy selection never disables the explicit write floor.
                    with pytest.raises(ovstage_mod.OvstageError) as excinfo:
                        stage.write_attribute(
                            query, attr, ordinal=6, tensors=np.array([0.0, 0.0], np.float32),
                            is_array=False,
                        ).wait()
                    assert excinfo.value.code == ovstage_mod.ErrorCode.WRITE_FLOOR_VIOLATION
