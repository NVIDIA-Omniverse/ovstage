# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

import numpy as np
import pytest

from ovstage import (AttributeSemantic, DLDataTypeCode, ErrorCode, OrdinalRange, OvstageError,
                     PathDictionary, PopulationDomain, population)

# The path does not resolve, so the resolved half must come back as token id 0.
ASSET_USDA = """#usda 1.0
(
    customLayerData = {
        bool populateAllAuthoredAttributes = true
    }
    defaultPrim = "World"
)
def Xform "World"
{
    asset test:asset = @nonexistent-python-asset.usd@
}
"""


def _pairs(rows) -> np.ndarray:
    """One 16-byte (authored, resolved) element per prim."""
    return np.array(rows, dtype=np.uint64).reshape(len(rows), 2)


def _check_asset_group(group):
    """An asset column is a fixed 16-byte id pair, not a ragged byte row."""
    assert not group.is_array
    assert AttributeSemantic(group.raw.semantic) == AttributeSemantic.ASSET_PATH_ID
    for row in range(group.tensor_count):
        dtype = group.tensor(row).dtype
        assert dtype.code == DLDataTypeCode.kDLUInt
        assert dtype.bits == 64
        assert dtype.lanes == 2


def _pairs_by_path(group, paths):
    """Map each covered prim's path to its (authored, resolved) id pair.

    Group indices address the group's path list, not the caller's path list.
    """
    list_paths = paths.get_path_strings(group.prim_list)
    values = np.asarray(group.array(0)).reshape(-1, 2)
    out = {}
    for local in range(group.prim_count):
        pair = values[group.data_row_index(local)]
        out[list_paths[group.prim_index(local)]] = (int(pair[0]), int(pair[1]))
    return out


def _read_one_group(stage, query, attr, end_ordinal):
    read = stage.read_attributes(query, [attr], OrdinalRange.latest(end_ordinal))
    read.wait()
    return read, read.fetch_next()


def test_native_asset_pairs_round_trip(stage):
    with PathDictionary(stage) as paths:
        plist = paths.create_path_list_from_strings(["/World/A", "/World/B", "/World/C"])
        query = stage.query_from_path_list(plist)
        try:
            expected = {
                "/World/A": (paths.intern_token("a.usd"), paths.intern_token("/abs/a.usd")),
                "/World/B": (paths.intern_token("deliberately_longer_b.usd"), 0),
                "/World/C": (paths.intern_token("c.usd"), paths.intern_token("/abs/c.usd")),
            }
            rows = [expected["/World/A"], expected["/World/B"], expected["/World/C"]]
            stage.write_attribute(query, "test:asset", ordinal=1, is_array=False,
                                  tensors=_pairs(rows),
                                  semantic=AttributeSemantic.ASSET_PATH_ID).wait()
            stage.advance_write_floor(ordinal=1).wait()

            attr = paths.intern_token("test:asset")
            read, group = _read_one_group(stage, query, attr, 1)
            assert group is not None
            _check_asset_group(group)
            assert group.prim_count == 3
            assert _pairs_by_path(group, paths) == expected
            stage.release_group(group)
            read.release().wait()

            # A token id of 0 means no path; resolving it yields "".
            assert paths.token_to_string(0) == ""
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


def test_asset_write_rejects_byte_rows(stage):
    # The semantic pins a 16-byte id pair. Text bytes carry no interned ids.
    with PathDictionary(stage) as paths:
        plist = paths.create_path_list_from_strings(["/World/A"])
        query = stage.query_from_path_list(plist)
        try:
            # A byte row is one ragged row per prim, so declare it as one. The
            # message assertion below proves the payload guard rejected it: any
            # other guard reports a different message and fails the test.
            with pytest.raises(OvstageError) as exc:
                stage.write_attribute(query, "test:asset", ordinal=1, is_array=True,
                                      tensors=np.frombuffer(b"a.usd", dtype=np.uint8),
                                      semantic=AttributeSemantic.ASSET_PATH_ID).wait()
            assert exc.value.code == ErrorCode.INVALID_ARGUMENT
            assert "ASSET_PATH_ID" in str(exc.value)
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)


@pytest.mark.skipif(not population.available(),
                    reason="libovstage was built without the ovstage population bridge")
def test_populated_scalar_asset_round_trip(stage):
    population.open_usd_from_string(stage, ASSET_USDA, ordinal=1, domains=PopulationDomain.RENDERING)
    with PathDictionary(stage) as paths:
        plist = paths.create_path_list_from_strings(["/World"])
        query = stage.query_from_path_list(plist)
        try:
            attr = paths.intern_token("test:asset")

            read, group = _read_one_group(stage, query, attr, 1)
            assert group is not None
            _check_asset_group(group)
            assert group.prim_count == 1
            authored, resolved = _pairs_by_path(group, paths)["/World"]
            assert paths.token_to_string(authored) == "nonexistent-python-asset.usd"
            assert resolved == 0
            stage.release_group(group)
            read.release().wait()

            updated = (paths.intern_token("updated_python_asset.usd"),
                       paths.intern_token("/abs/updated_python_asset.usd"))
            stage.write_attribute(query, "test:asset", ordinal=2, is_array=False,
                                  tensors=_pairs([updated]),
                                  semantic=AttributeSemantic.ASSET_PATH_ID).wait()
            stage.advance_write_floor(ordinal=2).wait()

            read, group = _read_one_group(stage, query, attr, 2)
            assert group is not None
            _check_asset_group(group)
            assert _pairs_by_path(group, paths) == {"/World": updated}
            stage.release_group(group)
            read.release().wait()
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)
