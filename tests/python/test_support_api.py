# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage support-API test: the library version accessor and the DLPack
# helpers (make_dltensor / dltensor_to_numpy) used to move tensor data in and out.
# CPU-only; no Stage required. The ovstage_mod fixture skips cleanly if the native
# library is not loadable.

import numpy as np

from ovstage import DLDataType, DLDataTypeCode, dltensor_to_numpy, library_version, make_dltensor


def test_library_version(ovstage_mod):
    version = library_version()
    assert isinstance(version, tuple)
    assert len(version) == 3
    assert all(isinstance(part, int) and part >= 0 for part in version)


def test_dlpack_scalar_round_trip(ovstage_mod):
    # [snippet:dlpack-round-trip]
    # make_dltensor wraps a numpy array as a DLTensor (zero-copy view); the array,
    # shape storage, and tensor stay linked so the C-visible pointers remain valid.
    # dltensor_to_numpy returns a flat numpy view back (CPU only).
    source = np.array([1.5, 2.5, 3.5], np.float32)
    tensor = make_dltensor(source)
    view = np.asarray(dltensor_to_numpy(tensor))
    assert view.shape[0] == 3
    assert bool(np.allclose(view, source))
    # [/snippet:dlpack-round-trip]


def test_dlpack_lane_folding(ovstage_mod):
    # A 3-lane float32 over 2 prims reads back as 6 flat base elements: vector lanes
    # fold into the element count on read, not into the numpy dtype.
    float3 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)
    source = np.arange(6, dtype=np.float32)
    tensor = make_dltensor(source, dtype=float3, shape=[2])
    view = np.asarray(dltensor_to_numpy(tensor))
    assert view.shape[0] == 6
    assert bool(np.allclose(view, source))
