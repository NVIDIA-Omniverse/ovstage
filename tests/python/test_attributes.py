# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage attribute-semantics test: a semantic (the authored USD meaning of
# a column's bytes) round-trips through the write→read cycle, orthogonal to the
# storage dtype; TOKEN_ID pins uint64 storage carrying pre-interned token ids.
# CPU-only. The write-flavors example is the workflow tour; this file asserts it.

import numpy as np

from ovstage import (AttributeSemantic, DLDataType, DLDataTypeCode, OrdinalRange, PathDictionary,
                     make_dltensor)

FLOAT3 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)
MAT4 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=64, lanes=16)


def _read_one_group(stage, query, attr, end_ordinal):
    read = stage.read_attributes(query, [attr], OrdinalRange.latest(end_ordinal))
    read.wait()
    return read, read.fetch_next()


def test_semantic_roles_round_trip(stage):
    with PathDictionary(stage) as paths:
        plist = paths.create_path_list_from_strings(["/World/Semantics/A", "/World/Semantics/B"])
        query = stage.query_from_path_list(plist)
        try:
            # [snippet:semantic-roles]
            # A semantic is the authored USD interpretation of a column's bytes,
            # orthogonal to the storage dtype: the write stamps it at creation and
            # reads recover it (group.raw.semantic). The same 3-lane float32 storage
            # is POINT on one column and COLOR on another; a 16-lane float64 is
            # MATRIX. TOKEN_ID differs: it pins uint64 storage and the payload must
            # be pre-interned path-dictionary token ids — ovstage never stringifies.
            steel = paths.intern_token("steel")
            rubber = paths.intern_token("rubber")
            stage.write_attribute(query, "points", ordinal=1, is_array=False,
                                  tensors=make_dltensor(np.array([0, 0, 1, 0, 1, 0], np.float32), dtype=FLOAT3,
                                                        shape=[2]), semantic=AttributeSemantic.POINT).wait()
            stage.write_attribute(query, "display-color", ordinal=1, is_array=False,
                                  tensors=make_dltensor(np.array([1, 0, 0, 0, 0.5, 0], np.float32), dtype=FLOAT3,
                                                        shape=[2]), semantic=AttributeSemantic.COLOR).wait()
            stage.write_attribute(query, "local-matrix", ordinal=1, is_array=False,
                                  tensors=make_dltensor(np.tile(np.eye(4, dtype=np.float64).ravel(), 2), dtype=MAT4,
                                                        shape=[2]), semantic=AttributeSemantic.MATRIX).wait()
            stage.write_attribute(query, "material", ordinal=1, is_array=False,
                                  tensors=np.array([steel, rubber], np.uint64),
                                  semantic=AttributeSemantic.TOKEN_ID).wait()
            stage.advance_write_floor(ordinal=1).wait()
            # [/snippet:semantic-roles]

            for name, expected in (("points", AttributeSemantic.POINT),
                                   ("display-color", AttributeSemantic.COLOR),
                                   ("local-matrix", AttributeSemantic.MATRIX),
                                   ("material", AttributeSemantic.TOKEN_ID)):
                attr = paths.intern_token(name)
                read, group = _read_one_group(stage, query, attr, 1)
                assert group is not None, name
                assert AttributeSemantic(group.raw.semantic) == expected, name
                if name == "local-matrix":
                    assert group.tensor(0).dtype.lanes == 16
                if name == "material":
                    resolved = [paths.token_to_string(int(t)) for t in np.asarray(group.array(0))]
                    assert set(resolved) == {"steel", "rubber"}  # order-independent
                stage.release_group(group)
                read.release().wait()
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(plist)
