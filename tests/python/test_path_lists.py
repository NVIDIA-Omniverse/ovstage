# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

"""Path-list reference ownership (:class:`ovstage.PathList`).

Path lists are refcounted C-side and a plain ``int`` has no finalizer, so a
per-iteration ``create_path_list_*`` that is never destroyed leaks one reference
per iteration for the life of the dictionary. ``create_path_list*`` returns a
``PathList`` — an ``int`` subclass that is the handle and also releases it.
"""

import copy
import gc
import json
import pickle

import pytest

PATHS = ["/World/PathList/A", "/World/PathList/B"]


@pytest.fixture
def paths(ovstage_mod, stage):
    with ovstage_mod.PathDictionary(stage) as pd:
        yield pd


def test_path_list_is_the_int_handle(ovstage_mod, paths):
    """PathList must stay usable anywhere a bare handle was accepted before."""
    plist = paths.create_path_list_from_strings(PATHS)
    try:
        assert isinstance(plist, int)
        assert isinstance(plist, ovstage_mod.PathList)
        # Passes through the C API unchanged — the value IS the handle.
        assert paths.path_list_count(plist) == len(PATHS)
        assert int(plist) == plist and plist > 0
    finally:
        paths.destroy_path_list(plist)


def test_formats_exactly_like_the_int_it_replaced(paths):
    """Swapping int -> PathList must not rewrite anyone's log output.

    int has no __str__ of its own (it inherits object.__str__, which defers to
    __repr__), so an informative __repr__ silently changes str() and f-strings
    unless __str__ is bound to int.__repr__.
    """
    plist = paths.create_path_list_from_strings(PATHS)
    try:
        handle = int(plist)
        assert str(plist) == str(handle)
        assert f"{plist}" == f"{handle}"
        assert f"{plist:d}" == f"{handle:d}"
        assert "%s" % plist == "%s" % handle
        assert json.dumps(plist) == json.dumps(handle)
        assert plist == handle and hash(plist) == hash(handle)
        # repr stays informative — that is the one deliberate difference.
        assert repr(plist).startswith("PathList(")
    finally:
        plist.release()


def test_copies_degrade_to_a_plain_non_owning_handle(paths):
    """A copy is not an owner: copying must not mint a second releaser."""
    plist = paths.create_path_list_from_strings(PATHS)
    try:
        handle = int(plist)
        for made in (copy.copy(plist), copy.deepcopy(plist), pickle.loads(pickle.dumps(plist))):
            assert type(made) is int  # not a PathList — no second release
            assert made == handle
        # Copying a structure that holds a handle keeps working, as it did for int.
        assert copy.deepcopy({"paths": [plist]})["paths"][0] == handle
    finally:
        plist.release()


def test_context_manager_releases_the_create_reference(paths):
    # [snippet:path-list-context-manager]
    with paths.create_path_list_from_strings(PATHS) as plist:
        assert paths.path_list_count(plist) == len(PATHS)
    # the create reference is released on block exit
    # [/snippet:path-list-context-manager]
    assert plist.released


def test_context_manager_releases_when_the_body_raises(paths):
    sentinel = RuntimeError("body failed")
    with pytest.raises(RuntimeError) as excinfo:
        with paths.create_path_list_from_strings(PATHS) as plist:
            raise sentinel
    # The original error propagates; the reference is still released.
    assert excinfo.value is sentinel
    assert plist.released


def test_explicit_release_marks_it_released(paths):
    plist = paths.create_path_list_from_strings(PATHS)
    assert not plist.released
    plist.release()
    assert plist.released


def test_double_release_is_rejected(ovstage_mod, paths):
    """A second release would drop a reference this object no longer owns."""
    plist = paths.create_path_list_from_strings(PATHS)
    paths.destroy_path_list(plist)
    with pytest.raises(ovstage_mod.OvxError):
        paths.destroy_path_list(plist)
    with pytest.raises(ovstage_mod.OvxError):
        plist.release()


def test_added_reference_is_the_callers_to_release(ovstage_mod, paths):
    """add_path_list_reference pairs with one extra destroy, as documented.

    The added reference is not tracked against the PathList, so the list survives
    the object's release and the caller drops it separately.
    """
    plist = paths.create_path_list_from_strings(PATHS)
    handle = int(plist)
    paths.add_path_list_reference(plist)
    paths.destroy_path_list(plist)  # releases the create reference
    assert plist.released
    assert paths.path_list_count(handle) == len(PATHS)  # the added one keeps it alive
    paths.destroy_path_list(handle)  # the caller releases their own
    with pytest.raises(ovstage_mod.OvxError):
        paths.path_list_count(handle)


def test_releasing_an_added_reference_by_handle_keeps_the_owner_intact(ovstage_mod, paths):
    """Releasing an added reference through the plain int must not steal the claim.

    The C API cannot tell the references apart, so a handle-form release is
    attributed to an added reference while any are outstanding. Otherwise it would
    clear the owner flag and strand the create reference permanently — no warning,
    and release() raising afterwards.
    """
    plist = paths.create_path_list_from_strings(PATHS)
    handle = int(plist)
    paths.add_path_list_reference(plist)
    paths.destroy_path_list(handle)  # releases the ADDED reference
    assert not plist.released  # the owner's claim survives
    assert paths.path_list_count(handle) == len(PATHS)
    plist.release()  # and the owner can still release normally
    assert plist.released
    with pytest.raises(ovstage_mod.OvxError):
        paths.path_list_count(handle)


def test_destroy_by_object_releases_an_added_reference_once_its_own_is_gone(ovstage_mod, paths):
    """destroy_path_list(plist) must stay the symmetric partner of add(plist).

    Once the object's own reference is released, passing the object again should
    release one of the caller's added references rather than raising and forcing
    them to know to write destroy_path_list(int(plist)).
    """
    plist = paths.create_path_list_from_strings(PATHS)
    handle = int(plist)
    paths.add_path_list_reference(plist)
    paths.destroy_path_list(plist)  # the object's own reference
    assert plist.released
    paths.destroy_path_list(plist)  # the added one, same call shape
    with pytest.raises(ovstage_mod.OvxError):
        paths.path_list_count(handle)


def test_finalizer_does_not_revoke_a_handed_off_reference(paths):
    """Dropping the PathList must not reclaim a reference the caller added.

    The path-dictionary contract sanctions holding a bare handle beyond the object
    it came from; reclaiming it would erase a list out from under a live borrow.
    """
    plist = paths.create_path_list_from_strings(PATHS)
    handle = int(plist)
    paths.add_path_list_reference(plist)  # handed off to a longer-lived consumer
    with pytest.warns(ResourceWarning):
        del plist
        gc.collect()
    assert paths.path_list_count(handle) == len(PATHS)  # borrow survived
    paths.destroy_path_list(handle)


def test_context_manager_leaves_a_caller_added_reference(paths):
    """`with` scopes the create reference only; added ones stay the caller's."""
    with paths.create_path_list_from_strings(PATHS) as plist:
        paths.add_path_list_reference(plist)
        handle = int(plist)
    assert plist.released  # the create reference went on block exit
    assert paths.path_list_count(handle) == len(PATHS)  # the added one did not
    paths.destroy_path_list(handle)


def test_dropped_path_list_warns_and_releases(paths):
    """The finalizer is a safety net: it reclaims the leak and reports it."""
    with pytest.warns(ResourceWarning, match="garbage-collected"):
        paths.create_path_list_from_strings(PATHS)
        gc.collect()


def test_released_path_list_does_not_warn(recwarn, paths):
    """No false ResourceWarning on the correct explicit-release path."""
    plist = paths.create_path_list_from_strings(PATHS)
    paths.destroy_path_list(plist)
    del plist
    gc.collect()
    assert not [w for w in recwarn.list if issubclass(w.category, ResourceWarning)]


def test_outliving_a_closed_shared_dictionary_still_releases(ovstage_mod, stage):
    """Closing a PathDictionary(stage) wrapper does not kill the C dictionary.

    It is owned by the Stage, so a list outstanding when the wrapper closes is a
    live leak for the rest of the Stage's life — it must still be reclaimed and
    reported, not silently written off as "died with the dictionary".
    """
    with ovstage_mod.PathDictionary(stage) as pd:
        plist = pd.create_path_list_from_strings(PATHS)
        handle = int(plist)
    assert not plist.released  # the wrapper closed; the reference did not
    with pytest.warns(ResourceWarning, match="garbage-collected"):
        del plist
        gc.collect()
    # The reference really went back: the list is now unknown to the dictionary.
    with ovstage_mod.PathDictionary(stage) as pd2, pytest.raises(ovstage_mod.OvxError):
        pd2.path_list_count(handle)


def test_outliving_its_own_stage_is_silent(ovstage_mod, recwarn):
    """A standalone dictionary owns its Stage, so destroy() really does kill the
    list — nothing to reclaim, and a warning would be un-actionable noise."""
    pd = ovstage_mod.PathDictionary()
    plist = pd.create_path_list_from_strings(PATHS)
    pd.destroy()  # owns its private Stage -> the C dictionary goes with it
    del pd, plist
    gc.collect()
    assert not [w for w in recwarn.list if issubclass(w.category, ResourceWarning)]


def test_release_through_a_plain_int_copy_clears_the_owner(ovstage_mod, paths):
    """Ownership is keyed by handle, so a copy cannot desync the owner's claim.

    copy/deepcopy/pickle deliberately degrade to a plain int; releasing through
    one must clear the owning PathList's claim, or its finalizer would release a
    second time on an already-freed list.
    """
    plist = paths.create_path_list_from_strings(PATHS)
    registry = copy.deepcopy({"paths": [plist]})
    assert type(registry["paths"][0]) is int
    paths.destroy_path_list(registry["paths"][0])  # released via the plain int
    assert plist.released  # the owner's count followed it down
    with pytest.raises(ovstage_mod.OvxError):
        paths.destroy_path_list(plist)  # no second release
    del plist
    gc.collect()  # finalizer must not release again either


def test_query_keeps_an_inline_path_list_alive_then_reports_it(ovstage_mod, stage, paths):
    """The Query reference is a safety net, not a transfer of ownership.

    A list passed inline would otherwise be finalized the moment the call returns,
    erasing it under a live query; the Query holding it prevents that. But nothing
    took over the release, so once the query is gone the list is reclaimed by the
    finalizer *with* a ResourceWarning — which is why the docs tell callers to bind
    the list rather than pass it inline.
    """
    plist = paths.create_path_list_from_strings(PATHS)
    handle = int(plist)
    query = stage.query_from_path_list(plist)
    del plist  # inline form: the caller keeps no handle of their own
    gc.collect()
    # The query holds it, so the list is still intact and unreleased.
    assert paths.path_list_count(handle) == len(PATHS)

    query.release().wait()
    with pytest.warns(ResourceWarning, match="garbage-collected"):
        del query
        gc.collect()
    # Reclaimed, but by the leak-report path rather than an explicit release.
    with pytest.raises(ovstage_mod.OvxError):
        paths.path_list_count(handle)


def test_bound_path_list_and_query_release_without_warning(recwarn, stage, paths):
    """The documented form: bind the list, release both, no report."""
    with paths.create_path_list_from_strings(PATHS) as plist:
        with stage.query_from_path_list(plist) as query:
            assert paths.path_list_count(plist) == len(PATHS)
    del query, plist
    gc.collect()
    assert not [w for w in recwarn.list if issubclass(w.category, ResourceWarning)]


def test_registry_does_not_grow_on_the_correct_path(paths, stage):
    """A create/destroy cycle must not leave a permanent entry behind.

    Per-frame create/destroy would otherwise add one dead key per frame for the
    life of the Stage, and a recycled handle would then be rejected as already
    released.
    """
    before = len(stage._path_list_refs)
    for _ in range(50):
        plist = paths.create_path_list_from_strings(PATHS)
        paths.destroy_path_list(plist)
        del plist
    gc.collect()
    assert len(stage._path_list_refs) == before

    # And the with-block form prunes too.
    for _ in range(10):
        with paths.create_path_list_from_strings(PATHS):
            pass
    gc.collect()
    assert len(stage._path_list_refs) == before


def test_ownership_is_shared_across_wrappers_over_one_stage(ovstage_mod, recwarn, stage):
    """The C dictionary is per-Stage; PathDictionary wrappers are not.

    PathDictionary(stage) is the documented shared form and ovstage.instancing
    builds one per call, so releasing through a second wrapper must clear the
    first one's claim — otherwise the finalizer double-releases a handle the C
    side may already have recycled.
    """
    with ovstage_mod.PathDictionary(stage) as pd_a, ovstage_mod.PathDictionary(stage) as pd_b:
        plist = pd_a.create_path_list_from_strings(PATHS)
        pd_b.destroy_path_list(int(plist))  # released through the other wrapper
        assert plist.released
        del plist
        gc.collect()
    assert not [w for w in recwarn.list if issubclass(w.category, ResourceWarning)]


def test_loop_reuse_creates_one_reference(paths):
    """Pattern 15: create the list once outside a per-frame loop and reuse it."""
    # [snippet:path-list-loop-reuse]
    with paths.create_path_list_from_strings(PATHS) as plist:
        for _ in range(64):
            assert paths.path_list_count(plist) == len(PATHS)
    # [/snippet:path-list-loop-reuse]
    assert plist.released
