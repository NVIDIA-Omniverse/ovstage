# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Read-group release ownership (:class:`ovstage.ReadGroup`).

A group's pinned storage is reclaimed only by ``release_group`` — releasing the
owning ``Read`` does not reclaim it. An unreleased group therefore stays pinned
for the life of the ``Stage``, and its outstanding-read coverage keeps failing
later writes to the same attribute and prims. ``ReadGroup`` is a context manager
with a finalizer safety net so a dropped group cannot brick subsequent writes.
"""

import gc
import warnings

import numpy as np
import pytest

from ovstage import OrdinalRange

PRIMS = ["/World/ReadGroup/A", "/World/ReadGroup/B"]


@pytest.fixture
def paths(ovstage_mod, stage):
    with ovstage_mod.PathDictionary(stage) as pd:
        yield pd


@pytest.fixture
def scene(ovstage_mod, stage):
    """A sealed one-column scene plus its query, torn down in reverse order."""
    with ovstage_mod.PathDictionary(stage) as paths:
        attr = paths.intern_token("test:read-group")
        with paths.create_path_list_from_strings(PRIMS) as plist:
            query = stage.query_from_path_list(plist)
            try:
                stage.write_attribute(
                    query, attr, ordinal=1,
                    tensors=np.array([1.0, 2.0], np.float32), is_array=False,
                ).wait()
                stage.advance_write_floor(ordinal=1).wait()
                yield stage, query, attr
            finally:
                stage.release_query(query).wait()


def _fetch_group(stage, query, attr):
    read = stage.read_attributes(query, [attr], OrdinalRange.latest(1))
    read.wait()
    group = read.fetch_next()
    assert group is not None
    return read, group


def _write(stage, query, attr, ordinal):
    stage.write_attribute(
        query, attr, ordinal=ordinal,
        tensors=np.full(len(PRIMS), float(ordinal), np.float32), is_array=False,
    ).wait()


def test_context_manager_releases_the_group(scene):
    stage, query, attr = scene
    read, group = _fetch_group(stage, query, attr)
    # [snippet:read-group-context-manager]
    # The group's pinned storage is released on block exit, so a later write to
    # the same prims is not blocked by a stale outstanding-read coverage record.
    with group:
        values = np.array(group.array(0))  # copy out; the view dies with the group
    # [/snippet:read-group-context-manager]
    np.testing.assert_allclose(values, [1.0, 2.0])
    assert group.released
    read.release().wait()


def test_context_manager_releases_when_the_body_raises(scene):
    stage, query, attr = scene
    read, group = _fetch_group(stage, query, attr)
    sentinel = RuntimeError("processing failed")
    with pytest.raises(RuntimeError) as excinfo:
        with group:
            raise sentinel
    assert excinfo.value is sentinel  # original error propagates
    assert group.released
    read.release().wait()


def test_double_release_is_rejected(ovstage_mod, scene):
    """A second release would free storage the instance already reclaimed."""
    stage, query, attr = scene
    read, group = _fetch_group(stage, query, attr)
    stage.release_group(group)
    with pytest.raises(ovstage_mod.OvstageError):
        stage.release_group(group)
    with pytest.raises(ovstage_mod.OvstageError):
        group.release()
    read.release().wait()


def test_dropped_group_warns_and_releases(scene):
    stage, query, attr = scene
    read, group = _fetch_group(stage, query, attr)
    with pytest.warns(ResourceWarning, match="garbage-collected"):
        del group
        gc.collect()
    read.release().wait()


def test_released_group_does_not_warn(recwarn, scene):
    stage, query, attr = scene
    read, group = _fetch_group(stage, query, attr)
    stage.release_group(group)
    del group
    gc.collect()
    assert not [w for w in recwarn.list if issubclass(w.category, ResourceWarning)]
    read.release().wait()


def test_dropped_group_no_longer_blocks_later_writes(scene):
    """The reported failure: a leaked group used to brick writes permanently.

    release_read does not reclaim a group, so before the finalizer existed the
    group's outstanding-read coverage survived for the life of the Stage and every
    later write to those prims failed with "overlapping outstanding read".
    """
    stage, query, attr = scene
    read, group = _fetch_group(stage, query, attr)
    read.release().wait()  # release the READ but not the GROUP
    del group              # dropped without release -> finalizer reclaims it
    gc.collect()

    for ordinal in (2, 3):
        _write(stage, query, attr, ordinal)  # raises if the stale pin survived


def _drop_read_group(stage, query, attr):
    """Drop an unreleased ReadGroup. release_group is synchronous."""
    read, group = _fetch_group(stage, query, attr)
    read.release().wait()
    del read, group
    return 6


def _drop_map(stage, query, attr):
    """Drop an unmapped Map session.

    Returns the session's own ordinal for the probe write: Map.__del__ enqueues
    the unmap without waiting, and only *same-ordinal* ops are ordered against
    each other. Probing at a different ordinal would race the pending unmap and
    fail intermittently.
    """
    session = stage.map_attribute(query, attr, ordinal=7)
    session.wait()
    del session
    return 7


@pytest.mark.parametrize("drop", [_drop_read_group, _drop_map], ids=["read_group", "map"])
def test_release_still_happens_when_resourcewarning_is_an_error(scene, drop):
    """Every finalizer must survive -W error::ResourceWarning.

    warnings.warn raises under that filter, so a finalizer that warns before
    releasing skips the release for exactly the users who asked to be strict about
    resources. Map overrides _HandleObject.__del__, so it needs its own coverage:
    a stranded map session pins the same prims and blocks later writes just as a
    stranded read group does.
    """
    stage, query, attr = scene
    with warnings.catch_warnings():
        warnings.simplefilter("error", ResourceWarning)
        probe_ordinal = drop(stage, query, attr)
        gc.collect()
    # Raises if the escalated warning skipped the release and left the pin.
    _write(stage, query, attr, probe_ordinal)


def test_query_outlives_its_source_path_list(ovstage_mod, stage, paths):
    """Canary: the shipped implementation copies the paths into its own list.

    This pins observed behaviour that goes **beyond** the header contract, not the
    contract itself. ``query_from_path_list`` in ``ovstage_api.h`` documents only a
    "caller-owned, interned prim path list" and never promises a copy, which is why
    the Python docs tell callers to keep their list alive for as long as the query
    and why ``release_query`` still carries the list on its op.

    A failure here does not necessarily mean a bug: it means the implementation
    stopped copying, so the keepalives become load-bearing rather than belt-and-
    braces. Either restore the copy, or promote the copy semantics into the header
    and relax the Python docs — do not "fix" it by deleting the keepalives.
    """
    plist = paths.create_path_list_from_strings(PRIMS)
    handle = int(plist)
    # Pass the plain int so the Query holds no keepalive, isolating what the C side
    # does on its own from what the binding does for the caller.
    query = stage.query_from_path_list(handle)
    paths.destroy_path_list(plist)
    del plist
    gc.collect()
    with pytest.raises(ovstage_mod.OvxError):
        paths.path_list_count(handle)  # source list fully gone
    stage.write_attribute(
        query, paths.intern_token("t:after"), ordinal=9,
        tensors=np.array([1.0, 2.0], np.float32), is_array=False,
    ).wait()  # query still fully usable
    query.release().wait()


def test_release_is_tracked_on_a_caller_constructed_group(ovstage_mod, scene):
    """Release state must be tracked even without an owning stage.

    ReadGroup is public, so a caller can construct one over a raw group. Such a
    group has no finalizer, but skipping the release claim for it would let a
    second release_group reach the C API and leave the accessors reading storage
    that has already been handed back.
    """
    stage, query, attr = scene
    read, group = _fetch_group(stage, query, attr)
    detached = ovstage_mod.ReadGroup(group.raw)  # no owning stage
    assert not detached.released

    stage.release_group(detached)
    assert detached.released
    with pytest.raises(ovstage_mod.OvstageError):
        stage.release_group(detached)  # must not reach the C API a second time
    for call in (lambda: detached.tensor(0), lambda: detached.prim_index(0)):
        with pytest.raises(ovstage_mod.OvstageError):
            call()

    group._claim_release()  # same storage — keep teardown from releasing it again
    read.release().wait()


def test_accessors_reject_a_released_group(ovstage_mod, scene):
    """Reading through a released group would dereference dangling pointers."""
    stage, query, attr = scene
    read, group = _fetch_group(stage, query, attr)
    stage.release_group(group)
    for call in (lambda: group.array(0), lambda: group.tensor(0), lambda: group.prim_index(0)):
        with pytest.raises(ovstage_mod.OvstageError):
            call()
    read.release().wait()


def test_group_survives_until_released(scene):
    """The finalizer must not fire while the group is still referenced."""
    stage, query, attr = scene
    read, group = _fetch_group(stage, query, attr)
    try:
        assert not group.released
        for _ in range(3):
            gc.collect()
            np.testing.assert_allclose(np.array(group.array(0)), [1.0, 2.0])
        assert not group.released
    finally:
        stage.release_group(group)
        read.release().wait()
