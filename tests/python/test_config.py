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

    with ovstage_mod.Stage("config.cpu", config=cpu_config):
        # The conflicting GPU entry is rejected before a GPU-backed stage is
        # created, so this remains a CPU-only public contract test.
        with pytest.raises(ovstage_mod.OvstageError) as excinfo:
            ovstage_mod.Stage("config.gpu-conflict", config=gpu_config)
        assert excinfo.value.code == ovstage_mod.ErrorCode.INVALID_ARGUMENT
