# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage support-API test: the library version accessor and the DLPack
# helpers (numpy_to_dldatatype / make_dltensor / dltensor_to_numpy) used to move
# tensor data in and out. CPU-only; no Stage required. The ovstage_mod fixture
# skips cleanly if the native library is not loadable.

import gc
import sys

import numpy as np
import pytest

from ovstage import (
    DLDataType,
    DLDataTypeCode,
    ManagedDLTensor,
    dltensor_to_numpy,
    library_version,
    make_dltensor,
    numpy_to_dldatatype,
)


class _DLPackProducer:
    """Hide numpy's direct path so the test exercises protocol ingestion."""

    def __init__(self, array):
        self.array = array

    def __dlpack__(self, **kwargs):
        return self.array.__dlpack__(**kwargs)

    def __dlpack_device__(self):
        return self.array.__dlpack_device__()


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


def test_numpy_to_dldatatype_accepts_valid_lanes(ovstage_mod):
    dtype = numpy_to_dldatatype(np.dtype("float32"), lanes=3)
    assert (dtype.code, dtype.bits, dtype.lanes) == (DLDataTypeCode.kDLFloat, 32, 3)
    assert numpy_to_dldatatype(np.dtype("float32")).lanes == 1
    assert numpy_to_dldatatype(np.dtype("float32"), lanes=65535).lanes == 65535


@pytest.mark.parametrize(
    "spelling, expected",
    [
        (np.float32, (DLDataTypeCode.kDLFloat, 32)),
        (np.float64, (DLDataTypeCode.kDLFloat, 64)),
        (np.int32, (DLDataTypeCode.kDLInt, 32)),
        (np.uint8, (DLDataTypeCode.kDLUInt, 8)),
        (np.bool_, (DLDataTypeCode.kDLBool, 8)),
        ("f4", (DLDataTypeCode.kDLFloat, 32)),
        (float, (DLDataTypeCode.kDLFloat, 64)),
        ("float32", (DLDataTypeCode.kDLFloat, 32)),
        (np.dtype("float32"), (DLDataTypeCode.kDLFloat, 32)),
    ],
)
def test_numpy_to_dldatatype_normalizes_dtype_spellings(ovstage_mod, spelling, expected):
    # Every spelling numpy itself accepts must resolve. A scalar *type* such as
    # np.float32 stringifies as "<class 'numpy.float32'>", so a lookup keyed by
    # dtype name rejects the most natural way to name a dtype unless the input is
    # normalized through np.dtype first.
    result = numpy_to_dldatatype(spelling)
    assert (result.code, result.bits, result.lanes) == (*expected, 1)


def test_numpy_to_dldatatype_rejects_unsupported_dtype(ovstage_mod):
    # Normalizing the input must not turn "unsupported" into "accepted with the
    # wrong code": a dtype numpy understands but DLPack has no mapping for, and an
    # object numpy cannot interpret at all, both still raise ValueError. None is the
    # one input numpy would happily normalize — np.dtype(None) is float64, "the
    # default dtype" — but this factory describes a specific buffer, so an unset
    # dtype must stay an error rather than silently become f8.
    for unsupported in (np.dtype([("a", "f4"), ("b", "i4")]), np.complex128, object(), None):
        with pytest.raises(ValueError):
            numpy_to_dldatatype(unsupported)


def test_numpy_to_dldatatype_rejects_out_of_range_lanes(ovstage_mod):
    # A bad lane count must fail loudly: the DLPack lanes field is uint16, and a
    # silently wrapped value (e.g. -1 -> 65535) describes a tensor far larger than
    # its backing buffer, turning the later decode into an out-of-bounds read.
    for bad_lanes in (-1, 0, 65536):
        with pytest.raises(ValueError):
            numpy_to_dldatatype(np.dtype("float32"), lanes=bad_lanes)


def test_numpy_to_dldatatype_rejects_non_integer_lanes(ovstage_mod):
    for bad_lanes in (1.5, "3", None):
        with pytest.raises(TypeError):
            numpy_to_dldatatype(np.dtype("float32"), lanes=bad_lanes)


def test_dlpack_cleanup_survives_failing_deleter_callback(ovstage_mod):
    # A deleter_callback that raises must not strand the rest of the cleanup. The
    # capsule context retains manager_ctx, so skipping its release pins the backing
    # object forever — observable as a refcount that never returns to baseline.
    #
    # The callback runs inside a C-level capsule destructor, so its failure surfaces
    # as an *unraisable* exception. The default hook (and pytest's) retains that
    # exception, and its traceback holds this frame's locals — which would pin
    # `backing` no matter what the code under test does. Swallow it for the duration
    # so the refcount measures only the behavior being tested.
    def boom(_ctx):
        raise RuntimeError("deleter callback failed")

    backing = np.zeros(4, np.float32)
    baseline = sys.getrefcount(backing)
    previous_hook = sys.unraisablehook
    sys.unraisablehook = lambda _unraisable: None
    try:
        for _ in range(5):
            tensor = ManagedDLTensor(
                make_dltensor(backing), manager_ctx=backing, deleter_callback=boom
            )
            capsule = tensor.__dlpack__(max_version=(1, 0))
            del capsule
            del tensor
    finally:
        sys.unraisablehook = previous_hook
    gc.collect()
    assert sys.getrefcount(backing) == baseline


def test_readonly_dlpack_export_rejects_consumer_write(ovstage_mod):
    # Regression guard for the reported failure, with no Stage and no numpy version
    # gate. The DLPack deleter must execute no Python: it used to be a ctypes
    # callback, so numpy releasing the tensor while its own "assignment destination
    # is read-only" ValueError propagated entered that callback with the error
    # indicator already set. CPython aborts a callback in that state at its first C
    # call, so the ValueError was reported *unraisable* and cleared, and the caller
    # saw "SystemError: error return without exception set" instead.
    #
    # Only numpy >= 2.1 requests a versioned capsule and honors the read-only flag;
    # on older numpy the write simply succeeds. What must not happen on any version
    # is a SystemError or an unraisable exception, so both outcomes pass here and
    # anything else propagates.
    unraisable = []
    previous_hook = sys.unraisablehook
    sys.unraisablehook = unraisable.append
    try:
        backing = np.zeros(4, np.float32)
        managed = ManagedDLTensor(make_dltensor(backing), manager_ctx=backing, readonly=True)
        try:
            np.from_dlpack(managed)[:] = 1.0
        except ValueError:
            pass  # numpy >= 2.1: the clean rejection this test is asking for
    finally:
        sys.unraisablehook = previous_hook
    assert unraisable == []


def test_dlpack_capsule_cleanup_survives_pending_exception(ovstage_mod):
    # An *unconsumed* capsule can be released by C code that is already raising. The
    # capsule destructor is a ctypes callback, so it must clear the error indicator
    # before doing anything else or CPython aborts it at its first C call and the
    # cleanup never runs, stranding the capsule context (which retains manager_ctx)
    # and the tensor block. Below, the list holds the only reference to the capsule
    # and str.join raises in C while freeing it.
    #
    # The caller unavoidably sees SystemError rather than the original TypeError:
    # ctypes discards whatever exception is set when a callback returns, so nothing
    # can be carried back across that boundary. Only the cleanup is guaranteed, and
    # that is what this test pins.
    class Owner:
        pass

    owner = Owner()
    baseline = sys.getrefcount(owner)
    previous_hook = sys.unraisablehook
    sys.unraisablehook = lambda _unraisable: None
    try:
        managed = ManagedDLTensor(
            make_dltensor(np.zeros(4, np.float32)), manager_ctx=owner, readonly=True
        )
        with pytest.raises((TypeError, SystemError)):
            "".join([managed.__dlpack__(max_version=(1, 0))])
        del managed
    finally:
        sys.unraisablehook = previous_hook
    gc.collect()
    assert sys.getrefcount(owner) == baseline


def test_dltensor_to_numpy_rejects_zero_lanes(ovstage_mod):
    # lanes == 0 is not a valid DLPack dtype; reject it instead of silently
    # coercing it to a 1-lane read.
    zero_lanes = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=0)
    tensor = make_dltensor(np.zeros(4, np.float32), dtype=zero_lanes)
    with pytest.raises(ValueError):
        dltensor_to_numpy(tensor)


def test_dltensor_to_numpy_rejects_view_beyond_backing_buffer(ovstage_mod):
    # ctypes wraps out-of-range assignments on direct DLDataType construction, so
    # an oversized lane count can still reach the decode path; it must be refused
    # there rather than returned as a view ~262 KB past the 4-byte buffer.
    oversized = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=65535)
    tensor = make_dltensor(np.zeros(1, np.float32), dtype=oversized)
    with pytest.raises(ValueError):
        dltensor_to_numpy(tensor)


@pytest.mark.parametrize(
    ("source_shape", "lanes"),
    [
        pytest.param((2, 3), 3, id="single-axis-lanes-3"),
        pytest.param((2, 4), 4, id="single-axis-lanes-4"),
        pytest.param((2, 2, 2), 4, id="multi-axis-lanes-4"),
    ],
)
def test_dlpack_producer_trailing_dimension_fold(ovstage_mod, source_shape, lanes):
    # External DLPack producers can explicitly reinterpret complete trailing
    # component axes as lanes without copying or changing their base dtype.
    vector_dtype = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=lanes)
    source = np.arange(int(np.prod(source_shape)), dtype=np.float32).reshape(source_shape)
    tensor = make_dltensor(_DLPackProducer(source), dtype=vector_dtype)
    assert tensor.data == source.ctypes.data
    assert tensor.shape_tuple == (2,)
    assert tensor.dtype.code == DLDataTypeCode.kDLFloat
    assert tensor.dtype.bits == 32
    assert tensor.dtype.lanes == lanes
    view = np.asarray(dltensor_to_numpy(tensor)).reshape(source_shape)
    assert bool(np.array_equal(view, source))


def test_dlpack_producer_fold_rejects_zero_bit_widths(ovstage_mod):
    backing = np.zeros(3, dtype=np.uint8)
    zero_bit_scalar = DLDataType(code=DLDataTypeCode.kDLUInt, bits=0, lanes=1)
    zero_bit_vector = DLDataType(code=DLDataTypeCode.kDLUInt, bits=0, lanes=3)
    uint8_vector = DLDataType(code=DLDataTypeCode.kDLUInt, bits=8, lanes=3)

    def make_zero_bit_producer():
        source = make_dltensor(backing, dtype=zero_bit_scalar)
        return ManagedDLTensor(source, manager_ctx=backing)

    with pytest.raises(ValueError, match="positive source and requested bit widths"):
        make_dltensor(make_zero_bit_producer(), dtype=uint8_vector)
    with pytest.raises(ValueError, match="positive source and requested bit widths"):
        make_dltensor(make_zero_bit_producer(), dtype=zero_bit_vector)

    with pytest.raises(ValueError, match="positive source and requested bit widths"):
        make_dltensor(_DLPackProducer(backing), dtype=zero_bit_vector)


def test_dlpack_producer_fully_consumed_fold_normalizes_to_one_element(ovstage_mod):
    # Folding the only source axis describes one logical vector element. Keep a
    # leading size-one dimension because ovstage row transport does not consume
    # rank-zero descriptors with null shape pointers.
    float3 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=3)
    source = np.array([1.0, 2.0, 3.0], dtype=np.float32)
    tensor = make_dltensor(_DLPackProducer(source), dtype=float3)
    assert tensor.data == source.ctypes.data
    assert tensor.ndim == 1
    assert tensor.shape_tuple == (1,)
    assert tensor.dtype.lanes == 3
    view = np.asarray(dltensor_to_numpy(tensor))
    assert bool(np.array_equal(view, source))

    # Canonical explicit overrides use the normalized one-element shape.
    explicit = make_dltensor(
        _DLPackProducer(source),
        dtype=float3,
        shape=[1],
        ndim=1,
        strides=[1],
    )
    assert explicit.shape_tuple == (1,)

    with pytest.raises(ValueError, match="canonical folded shape"):
        make_dltensor(
            _DLPackProducer(source),
            dtype=float3,
            shape=[],
            ndim=0,
            strides=[],
        )
