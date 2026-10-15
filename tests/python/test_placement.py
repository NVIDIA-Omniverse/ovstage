# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# Public ovstage device-placement test: a write may name which device's copy of
# an attribute it produces. CPU-only -- every case here either uses a CPU
# placement or asserts the rejection of one this build cannot serve, so the file
# runs on a host with no CUDA device.

import ctypes

import numpy as np
import pytest

from ovstage import (
    DLDataType,
    DLDataTypeCode,
    ErrorCode,
    OrdinalRange,
    OvstageError,
    PathDictionary,
    Placement,
    WriteDesc,
)

#: DLPack device types. CPU is 1; 0 is not assigned by DLPack and means "unspecified".
_DL_CPU = 1
_DL_CUDA = 2

#: A fresh mapped attribute needs an explicit element type to be created.
_FLOAT1 = DLDataType(code=DLDataTypeCode.kDLFloat, bits=32, lanes=1)


def _write(stage, query, attr, ordinal, **kwargs):
    stage.write_attribute(
        query,
        attr,
        ordinal=ordinal,
        tensors=np.array([1.0, 2.0, 3.0], np.float32),
        is_array=False,
        **kwargs,
    ).wait()


@pytest.fixture
def placement_fixture(stage):
    """A sealed three-prim query plus its path dictionary, torn down in order."""
    with PathDictionary(stage) as paths:
        attr = paths.intern_token("temperature")
        prim_paths = paths.create_path_list_from_strings(["/World/A", "/World/B", "/World/C"])
        query = stage.query_from_path_list(prim_paths)
        try:
            yield stage, query, attr
        finally:
            stage.release_query(query).wait()
            paths.destroy_path_list(prim_paths)


def test_unplaced_write_is_unchanged(placement_fixture):
    """placement=None must behave exactly as before placement existed."""
    stage, query, attr = placement_fixture
    _write(stage, query, attr, 1)
    stage.advance_write_floor(ordinal=1).wait()

    read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
    read.wait()
    group = read.fetch_next()
    values = np.array(group.array(0))
    stage.release_group(group)
    read.release().wait()
    np.testing.assert_allclose(values, [1.0, 2.0, 3.0])


def test_cpu_placement_agreeing_with_source_round_trips(placement_fixture):
    """A placement naming the device the source is already on is honored."""
    stage, query, attr = placement_fixture
    # [snippet:write-with-placement]
    # Name the device whose copy of the attribute this write should produce. A
    # later read on that same device is then served without a migration.
    stage.write_attribute(
        query,
        attr,
        ordinal=1,
        tensors=np.array([7.0, 8.0, 9.0], np.float32),
        is_array=False,
        placement=Placement(device_type=_DL_CPU, device_id=0),
    ).wait()
    # [/snippet:write-with-placement]
    stage.advance_write_floor(ordinal=1).wait()

    read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
    read.wait()
    group = read.fetch_next()
    values = np.array(group.array(0))
    stage.release_group(group)
    read.release().wait()
    np.testing.assert_allclose(values, [7.0, 8.0, 9.0])


def test_unspecified_device_in_a_supplied_placement_is_accepted(placement_fixture):
    """device_type 0 inside a real struct means the same as passing no placement."""
    stage, query, attr = placement_fixture
    _write(stage, query, attr, 1, placement=Placement())


def test_batched_write_accepts_one_placement(placement_fixture):
    """Placement is an operation-level modifier, so it covers every entry."""
    stage, query, attr = placement_fixture
    stage.write_attributes(
        query,
        [WriteDesc(attr, np.array([1.0, 2.0, 3.0], np.float32), is_array=False)],
        1,
        placement=Placement(device_type=_DL_CPU),
    ).wait()


def test_host_payload_may_be_placed_on_a_cuda_device(placement_fixture):
    """A host payload with a CUDA placement is staged onto the device.

    This suite is CPU-only, so the assertion has to hold on both kinds of host:
    with a CUDA device the write is accepted and the data lands on the GPU;
    without one, device 0 names nothing and the id check rejects it. What it
    pins either way is that this direction is no longer refused as unsupported
    -- the full device matrix (host->device staged, device->host and
    device->device still refused) is covered by the GPU suite.
    """
    stage, query, attr = placement_fixture
    try:
        _write(stage, query, attr, 1, placement=Placement(device_type=_DL_CUDA, device_id=0))
    except OvstageError as excinfo:
        assert excinfo.code == ErrorCode.INVALID_ARGUMENT, (
            f"a host->CUDA placement must be staged or rejected for a missing device, got {excinfo}"
        )


def _ragged_rows():
    """One variable-length row per prim in the fixture query.

    A multi-tensor array write requires exactly one tensor per logical prim, so
    this has to track the fixture rather than be an arbitrary list.
    """
    return [
        np.array([1.0], np.float32),
        np.array([2.0, 3.0], np.float32),
        np.array([4.0, 5.0, 6.0], np.float32),
    ]


def test_ragged_payload_may_be_placed_on_a_cuda_device(placement_fixture):
    """A ragged payload is staged row by row onto the placed device.

    Variable-length rows transport one tensor each, so staging them means moving
    every row into one allocation and handing down a parallel tensor array. The
    assertion has to hold on a CPU-only runner too, so it accepts the
    missing-device rejection; the GPU suite proves the rows land and read back.
    """
    stage, query, attr = placement_fixture
    rows = _ragged_rows()
    try:
        stage.write_attribute(
            query, attr, ordinal=1, tensors=rows, is_array=True,
            placement=Placement(device_type=_DL_CUDA, device_id=0),
        ).wait()
    except OvstageError as excinfo:
        assert excinfo.code == ErrorCode.INVALID_ARGUMENT, (
            f"a ragged host->CUDA placement must be staged or rejected for a missing device, got {excinfo}"
        )


def test_ragged_payload_round_trips_through_a_cpu_placement(placement_fixture):
    """Staging must not disturb the row structure when no transfer is needed."""
    stage, query, attr = placement_fixture
    rows = _ragged_rows()
    stage.write_attribute(
        query, attr, ordinal=1, tensors=rows, is_array=True,
        placement=Placement(device_type=_DL_CPU, device_id=0),
    ).wait()
    stage.advance_write_floor(ordinal=1).wait()

    read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
    read.wait()
    group = read.fetch_next()
    assert group is not None
    out = [np.array(group.array(i)).tolist() for i in range(group.raw.data.tensor_count)]
    stage.release_group(group)
    read.release().wait()
    assert out == [[1.0], [2.0, 3.0], [4.0, 5.0, 6.0]]


def test_placement_with_an_index_map_gathers_correctly(placement_fixture):
    """Staging must not disturb sparse row selection.

    The staged payload is copied as transported rows, and the index map selects
    among those rows afterwards -- so a staging size computed from the logical
    element count instead of the transported row count would corrupt this.
    """
    stage, query, attr = placement_fixture
    stage.write_attribute(
        query, attr, ordinal=1, tensors=np.array([9.0, 8.0], np.float32), is_array=False,
        index_map=[1, 0, 1], count=3, placement=Placement(device_type=_DL_CPU),
    ).wait()
    stage.advance_write_floor(ordinal=1).wait()

    read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
    read.wait()
    group = read.fetch_next()
    values = np.array(group.array(0)).tolist()
    stage.release_group(group)
    read.release().wait()
    assert values == [8.0, 9.0, 8.0]


def test_placement_with_a_mask_is_accepted(placement_fixture):
    """A masked placed write still carries a payload row per logical element."""
    stage, query, attr = placement_fixture
    stage.write_attribute(
        query, attr, ordinal=1, tensors=np.array([1.0, 2.0, 3.0], np.float32), is_array=False,
        mask=[0b101], count=3, placement=Placement(device_type=_DL_CPU),
    ).wait()


def test_out_of_range_device_id_is_an_invalid_argument(placement_fixture):
    """A device_id naming no device is a caller bug, not a capability gap.

    This is the same status split the source-tensor check uses: an unsupported
    device *kind* is NOT_SUPPORTED, a bad *id* is INVALID_ARGUMENT.
    """
    stage, query, attr = placement_fixture
    with pytest.raises(OvstageError) as excinfo:
        _write(stage, query, attr, 1, placement=Placement(device_type=_DL_CUDA, device_id=4096))
    assert excinfo.value.code == ErrorCode.INVALID_ARGUMENT


def test_write_accepts_a_plain_placement(placement_fixture):
    """A placed write round-trips through the device-targeted entry point."""
    stage, query, attr = placement_fixture
    stage.write_attribute(
        query, attr, ordinal=1, tensors=np.array([1.0, 2.0, 3.0], np.float32), is_array=False,
        placement=Placement(device_type=_DL_CPU),
    ).wait()
    stage.advance_write_floor(ordinal=1).wait()

    read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
    read.wait()
    group = read.fetch_next()
    values = np.array(group.array(0)).tolist()
    stage.release_group(group)
    read.release().wait()
    assert values == [1.0, 2.0, 3.0]


def test_map_accepts_a_plain_placement(placement_fixture):
    """Same for a map."""
    stage, query, attr = placement_fixture
    with stage.map_attribute(
        query, attr, ordinal=1, dtype=_FLOAT1,
        placement=Placement(device_type=_DL_CPU),
    ) as session:
        session.wait()
        for group in session.groups():
            buffer = group.array(0)
            for local in range(group.prim_count):
                row = group.data_row_index(local) if group.has_data_index_map else local
                buffer[row] = 4.0
    stage.advance_write_floor(ordinal=1).wait()

    read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
    read.wait()
    group = read.fetch_next()
    values = np.array(group.array(0)).tolist()
    stage.release_group(group)
    read.release().wait()
    assert values == [4.0, 4.0, 4.0]


def test_placement_structs_match_the_c_layout():
    """Pin the ctypes mirror against the C struct field for field.

    The struct carries no size or version field, so a mirror that is missing a
    trailing member does not fail loudly -- the native side simply reads past the
    end of what Python allocated. Sizes are the cheapest thing that catches it.
    """
    from ovstage._src import bindings as _bindings

    # ovstage_placement_t { DLDevice device; uintptr_t stream;
    #                        uintptr_t availability_event; }: a device
    # (DLDeviceType + int32), the stream the op's device work runs on, and the
    # caller-owned cudaEvent_t ovstage records into once the memory is safe for
    # the caller to dereference.
    assert [name for name, _ in _bindings.ovstage_placement_t._fields_] == [
        "device",
        "stream",
        "availability_event",
    ]
    assert ctypes.sizeof(_bindings.ovstage_placement_t) == 24

    # Placement reaches the API through dedicated *_on_device entry points, so
    # there is no per-operation options struct to mirror -- and a read places
    # nothing at all. Assert the absence instead of dropping the check: a mirror
    # left behind for a struct the C side no longer has is the silent kind of
    # drift this test exists to catch.
    for gone in ("ovstage_write_options_t", "ovstage_map_options_t", "ovstage_read_options_t"):
        assert not hasattr(_bindings, gone), f"{gone} no longer exists in the C API"


@pytest.mark.parametrize(
    "kwargs",
    [
        {"device_type": 7},  # in range for int32, not a device type ovstage serves
        {"device_type": _DL_CUDA, "device_id": -1},
        {"device_type": _DL_CUDA, "device_id": 2**31},
        {"device_type": _DL_CPU, "stream": -1},
        {"device_type": _DL_CPU, "stream": 2**64},
        # availability_event is a cudaEvent_t address in the same uintptr_t field
        # shape as stream, so it needs the same guard: a wrapped value would not
        # fail, it would name some other live handle for ovstage to record into.
        {"device_type": _DL_CPU, "availability_event": -1},
        {"device_type": _DL_CPU, "availability_event": 2**64},
    ],
)
def test_placement_rejects_out_of_range_fields(kwargs):
    """ctypes wraps silently, so every field needs a guard in the binding.

    A negative that wraps to all-ones is the same defect as 2**32 wrapping to 0;
    both must raise here rather than reach the C side as a different value.
    """
    with pytest.raises(ValueError):
        Placement(**kwargs)


@pytest.mark.parametrize(
    "kwargs",
    [
        {"device_type": 1.5},
        {"device_type": _DL_CPU, "stream": 5.0},
        {"device_type": _DL_CPU, "availability_event": 5.0},
    ],
)
def test_placement_rejects_non_integer_fields(kwargs):
    """operator.index, not int(): a float must not truncate into the struct.

    5.0 is the case int() would wave through. A handle field is the worst place
    for that: the truncated value is still a plausible address, so ovstage would
    record into whatever it names rather than reject the call.
    """
    with pytest.raises(TypeError):
        Placement(**kwargs)


def test_placement_carries_every_field_into_the_c_struct():
    """The mirror is only half the guard; the conversion has to fill it too.

    ovstage_placement_t has no size field, so a member make_placement forgets to
    set is a member the native side still reads -- and for availability_event
    that is an uninitialized cudaEvent_t.
    """
    placement = Placement(device_type=_DL_CPU, device_id=0, stream=7, availability_event=11)
    native = placement._to_c()
    # device_type is a ctypes.c_int *subclass*, which ctypes hands back as an
    # instance rather than an int, so .value is required -- without it the
    # comparison is an identity test that is simply always false.
    assert native.device.device_type.value == _DL_CPU
    assert native.device.device_id == 0
    assert native.stream == 7
    assert native.availability_event == 11

    # And the defaults really are "none" rather than whatever was last in memory.
    empty = Placement()._to_c()
    assert empty.device.device_type.value == 0
    assert empty.stream == 0
    assert empty.availability_event == 0


def test_placement_requires_a_placement_object(placement_fixture):
    """A bare (device_type, device_id) tuple is the easy mistake; catch it early."""
    stage, query, attr = placement_fixture
    with pytest.raises(TypeError):
        _write(stage, query, attr, 1, placement=(_DL_CPU, 0))


def test_from_dlpack_device_accepts_a_pair_and_a_producer():
    """The ergonomic path: place storage where the caller's tensors already are."""
    assert Placement.from_dlpack_device((_DL_CUDA, 3)) == Placement(device_type=_DL_CUDA, device_id=3)

    class _Producer:
        def __dlpack_device__(self):
            return (_DL_CUDA, 1)

    # The device comes from the tensor; the synchronization stays the caller's,
    # in both directions.
    assert Placement.from_dlpack_device(_Producer(), stream=7, availability_event=9) == Placement(
        device_type=_DL_CUDA, device_id=1, stream=7, availability_event=9
    )


def test_from_dlpack_device_rejects_a_non_producer():
    with pytest.raises(TypeError):
        Placement.from_dlpack_device(object())


def test_map_accepts_a_cpu_placement(placement_fixture):
    stage, query, attr = placement_fixture
    with stage.map_attribute(
        query, attr, ordinal=1, dtype=_FLOAT1, placement=Placement(device_type=_DL_CPU)
    ) as session:
        session.wait()
        for group in session.groups():
            buffer = group.array(0)
            for local in range(group.prim_count):
                row = group.data_row_index(local) if group.has_data_index_map else local
                buffer[row] = 1.0
    stage.advance_write_floor(ordinal=1).wait()


def test_map_rejects_an_out_of_range_device_id(placement_fixture):
    stage, query, attr = placement_fixture
    with pytest.raises(OvstageError) as excinfo:
        stage.map_attribute(
            query, attr, ordinal=1, dtype=_FLOAT1,
            placement=Placement(device_type=_DL_CUDA, device_id=4096),
        ).wait()
    assert excinfo.value.code == ErrorCode.INVALID_ARGUMENT


# ── placed reads ─────────────────────────────────────────────────────────────
#
# A placed read selects which of an attribute's copies the returned groups point
# at. Serving one from a CUDA device needs a CUDA device, so this suite pins the
# surface and the validation only: the host cases, and the statuses a bad
# placement produces. The device matrix -- transfer, zero-copy, availability
# event, payload lifetime -- belongs to the GPU suite.


def _seed(stage, query, attr):
    """Write and seal the column a read case needs.

    Unlike a write, a read places nothing: it selects among copies that already
    exist, so there has to be sealed data below the write floor first.
    """
    _write(stage, query, attr, 1)
    stage.advance_write_floor(ordinal=1).wait()


def _read_once(stage, query, attr, **kwargs):
    """Drive one read to completion and return its values and reported device."""
    read = stage.read_attributes(query, [attr], OrdinalRange.latest(1), **kwargs)
    read.wait()
    group = read.fetch_next()
    assert group is not None
    values = np.array(group.array(0)).tolist()
    # device_type is a ctypes.c_int *subclass*, handed back as an instance rather
    # than an int, so .value is required -- without it the comparison below is an
    # identity test that is simply always false.
    device = group.raw.data.tensors[0].device.device_type.value
    stage.release_group(group)
    read.release().wait()
    return values, device


def test_read_with_no_placement_is_unchanged(placement_fixture):
    """The unplaced read keeps its signature and its behaviour."""
    stage, query, attr = placement_fixture
    _seed(stage, query, attr)
    values, device = _read_once(stage, query, attr)
    assert values == [1.0, 2.0, 3.0]
    assert device == _DL_CPU


def test_read_with_a_cpu_placement_matches_the_unplaced_read(placement_fixture):
    """A host placement is the same call, now stated explicitly.

    Naming the device the column is already current on is the zero-copy case:
    nothing is transferred and the group points straight at the existing copy.
    """
    stage, query, attr = placement_fixture
    _seed(stage, query, attr)
    assert _read_once(stage, query, attr, placement=Placement(device_type=_DL_CPU)) == ([1.0, 2.0, 3.0], _DL_CPU)


def test_read_with_an_unspecified_placement_is_accepted(placement_fixture):
    """device_type 0 inside a real struct means the same as passing no placement."""
    stage, query, attr = placement_fixture
    _seed(stage, query, attr)
    assert _read_once(stage, query, attr, placement=Placement()) == ([1.0, 2.0, 3.0], _DL_CPU)


def test_read_of_a_host_column_on_a_cuda_device_is_not_refused(placement_fixture):
    """The permissive rule, as far as a CPU-only runner can see it.

    This column is current on the host, and asking for it on a CUDA device must
    not be refused for that reason: a placed read makes the data available on the
    named device rather than failing. A device-residency refusal here would be a
    regression.

    The assertion has to hold on both kinds of host: with a CUDA device the read
    is accepted and served from it; without one, device 0 names nothing and the id
    check rejects it before residency is ever consulted. The payload is
    deliberately not fetched -- what lands in the group is a device pointer, and
    asserting anything about it would make this case GPU-dependent.
    """
    stage, query, attr = placement_fixture
    _seed(stage, query, attr)
    read = stage.read_attributes(
        query, [attr], OrdinalRange.latest(1), placement=Placement(device_type=_DL_CUDA, device_id=0)
    )
    try:
        read.wait()
    except OvstageError as excinfo:
        assert excinfo.code == ErrorCode.INVALID_ARGUMENT, (
            f"a CUDA-placed read must be served or rejected for a missing device, got {excinfo}"
        )
    else:
        read.release().wait()


def test_read_rejects_an_out_of_range_device_id(placement_fixture):
    """Same status split as the placed write: a bad *id* is a caller bug.

    Without this check the id reaches the residency query and the payload comes
    back labelled with a device the memory does not live on -- worse than an
    error, because a consumer trusts the label and dereferences on that device.
    """
    stage, query, attr = placement_fixture
    _seed(stage, query, attr)
    with pytest.raises(OvstageError) as excinfo:
        stage.read_attributes(
            query, [attr], OrdinalRange.latest(1),
            placement=Placement(device_type=_DL_CUDA, device_id=4096),
        ).wait()
    assert excinfo.value.code == ErrorCode.INVALID_ARGUMENT


# There is no Python case for an unsupported device *kind* on a read: Placement
# rejects a device_type outside the set ovstage serves before any call is made,
# which test_placement_rejects_out_of_range_fields already pins. The C suite
# covers the NOT_SUPPORTED status itself, where such a value can still be built.


def test_read_requires_a_placement_object(placement_fixture):
    """A bare (device_type, device_id) tuple is the easy mistake; catch it early."""
    stage, query, attr = placement_fixture
    _seed(stage, query, attr)
    with pytest.raises(TypeError):
        stage.read_attributes(query, [attr], OrdinalRange.latest(1), placement=(_DL_CPU, 0))
