# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Public Python contract for the opt-in history feature.

A stage of ``history_selective`` turns history on for the ``(prim, attribute)``
pairs that ``Stage.enable_history()`` selects. A stage of ``history_latest``,
the default, keeps history off and raises ``NOT_SUPPORTED``.
"""

import numpy as np
import pytest


def _read_one(stage, query, attr, ordinal_range):
    """Return the single float of a one-prim query, or None when no value exists."""
    read = stage.read_attributes(query, [attr], ordinal_range)
    read.wait()
    value = None
    while True:
        group = read.fetch_next()
        if group is None:
            break
        column = group.columns[0]
        if not column.is_delete and column.tensor_count == 1:
            value = float(column.array(0)[0])
        stage.release_group(group)
    read.release().wait()
    return value


def test_default_stage_rejects_history_selection(ovstage_mod, stage):
    paths = ovstage_mod.PathDictionary(stage)
    with paths.create_path_list_from_strings(["/World/A"]) as plist:
        query = stage.query_from_path_list(plist)
        with pytest.raises(ovstage_mod.OvstageError) as excinfo:
            stage.enable_history(query, ["temperature"])
        assert excinfo.value.code == ovstage_mod.ErrorCode.NOT_SUPPORTED
        query.release().wait()


def test_selected_pair_retains_earlier_sealed_values(ovstage_mod):
    del ovstage_mod  # Fixture verifies that the produced wheel and native library load.

    # [snippet:history-enable-read]
    import ovstage

    # Select the implementation "history_selective": history is on for the
    # pairs that enable_history selects. The selection is per stage: a stage
    # of "history_latest", the default, in the same process keeps history off.
    config = ovstage.StageConfig(implementation="history_selective")
    with ovstage.Stage("history.selected", config=config) as stage:
        paths = ovstage.PathDictionary(stage)
        temperature = paths.intern_token("temperature")
        with paths.create_path_list_from_strings(["/World/A"]) as plist:
            query = stage.query_from_path_list(plist)

            # Write and seal the first value, then select the exact pair.
            # The sealed value becomes the retained baseline.
            stage.write_attribute(query, temperature, 1, np.asarray([1.0], dtype=np.float32), is_array=False).wait()
            stage.advance_write_floor(1, ovstage.Scope.ALL).wait()
            stage.enable_history(query, [temperature])

            # A later sealed write keeps the earlier value readable for this pair.
            stage.write_attribute(query, temperature, 2, np.asarray([2.0], dtype=np.float32), is_array=False).wait()
            stage.advance_write_floor(2, ovstage.Scope.ALL).wait()
            first = _read_one(stage, query, temperature, ovstage.OrdinalRange.between(1, 1))
            latest = _read_one(stage, query, temperature, ovstage.OrdinalRange.latest(2))

            # Disabling discards the retained history and keeps the live value.
            stage.disable_history(query, [temperature])
            query.release().wait()
    # [/snippet:history-enable-read]

    assert first == pytest.approx(1.0)
    assert latest == pytest.approx(2.0)


def _read_token_rows(stage, query, attr, ordinal_range):
    """Return (deleted, values) of a one-prim query: a deletion flag and the token ids read."""
    read = stage.read_attributes(query, [attr], ordinal_range)
    read.wait()
    deleted = False
    values = []
    while True:
        group = read.fetch_next()
        if group is None:
            break
        column = group.columns[0]
        if column.is_delete:
            deleted = True
        else:
            for index in range(column.tensor_count):
                values.extend(int(token) for token in column.array(index).tolist())
        stage.release_group(group)
    read.release().wait()
    return deleted, values


@pytest.mark.parametrize(
    ("attribute", "value", "is_array"),
    [("usd-prim-type", "Cube", False), ("usd-schemas", "MaterialBindingAPI", True)],
)
def test_deleted_reserved_metadata_is_retained_as_a_deletion(ovstage_mod, attribute, value, is_array):
    del ovstage_mod
    import ovstage

    config = ovstage.StageConfig(implementation="history_selective")
    with ovstage.Stage("history.metadata-delete", config=config) as stage:
        paths = ovstage.PathDictionary(stage)
        token = paths.intern_token(attribute)
        value_id = paths.intern_token(value).raw_id
        with paths.create_path_list_from_strings(["/World/A"]) as plist:
            query = stage.query_from_path_list(plist)
            stage.write_attribute(query, token, 1, np.asarray([value_id], dtype=np.uint64), is_array=is_array).wait()
            stage.advance_write_floor(1, ovstage.Scope.ALL).wait()
            stage.enable_history(query, [token])

            # The delete of the selected reserved attribute is retained at its
            # ordinal: the seal succeeds, the live read reports no cell, the
            # read at the delete ordinal reports the deletion, and the earlier
            # ordinal keeps its value.
            stage.delete_attributes(query, [token], 2).wait()
            stage.advance_write_floor(2, ovstage.Scope.ALL).wait()
            live = _read_token_rows(stage, query, token, ovstage.OrdinalRange.latest(ovstage.ORDINAL_MAX))
            at_two = _read_token_rows(stage, query, token, ovstage.OrdinalRange.between(2, 2))
            at_one = _read_token_rows(stage, query, token, ovstage.OrdinalRange.between(1, 1))
            query.release().wait()

    assert live == (False, [])
    assert at_two == (True, [])
    assert at_one == (False, [value_id])
