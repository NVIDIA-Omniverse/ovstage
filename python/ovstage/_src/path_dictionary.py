# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Pythonic wrapper for the ovstage path-dictionary interning API.

The path dictionary interns strings (tokens), prim paths, and ordered prim-path
lists shared across ovstage / ovrtx / application code. It is owned by an ovstage
instance and obtained through the ``get_path_dictionary`` vtable slot (see
``ovstage_api.h`` and ``ovx/path_dictionary/path_dictionary.h``); in the current
implementation all live instances share one refcounted dictionary,
so handles minted through any instance are usable from the others.

Handle-lifetime regimes:
    * tokens / prim paths are dict-lifetime (no per-handle release);
    * path lists are refcounted — ``create_path_list*`` returns one reference
      that the caller releases via :meth:`PathDictionary.destroy_path_list`.
"""

import ctypes
import weakref
from typing import List, Sequence

from . import bindings as _b
from .types import _warn_dropped_resource, check_handle, check_int, check_token, check_token_sequence

__all__ = ["PathDictionary", "PathList", "OvxError"]

# Initial path depth resolved in a single ``path_to_string`` decomposition pass.
# ``path_to_string`` grows the buffer and retries for deeper paths, up to
# ``_MAX_TOKEN_DECOMPOSE_BUFFER``, so depth is not silently capped at this value.
_TOKEN_DECOMPOSE_BUFFER = 64
# Safety ceiling on the retry growth (SdfPath depth is realistically far below
# this); guards against an unbounded loop if the C API never reports progress.
_MAX_TOKEN_DECOMPOSE_BUFFER = 1 << 16


class OvxError(RuntimeError):
    """Raised on a non-success ``ovx_api_result_t`` from a path-dictionary call."""

    def __init__(self, code: int, message: str = ""):
        self.code = check_int(code, "code")
        self.message = message or ""
        super().__init__(f"ovx error {self.code}: {self.message}" if self.message else f"ovx error {self.code}")


def _check_pd_result(pd, result: "_b.ovx_api_result_t") -> None:
    """Raise :class:`OvxError` for a failed call, releasing the owned error string.

    Takes the raw ``path_dictionary_instance_t*`` rather than a
    :class:`PathDictionary` so a :class:`PathList` can still release itself after
    its wrapper is closed.
    """
    if result.status == _b.OVX_API_SUCCESS:
        return
    message = str(result.error) if result.error else ""
    if result.error:
        pd.contents.vtable.contents.release_error(pd.contents.context, result.error)
    raise OvxError(int(result.status), message)


def _release_path_list_reference(pd, handle: int) -> None:
    """Drop one C reference on ``handle`` through a raw dictionary pointer."""
    vtable = pd.contents.vtable.contents
    _check_pd_result(pd, vtable.release_path_list_reference(pd.contents.context, handle))


# Ownership entry layout: [create_reference_outstanding, caller_added_count].
_CREATED, _ADDED = 0, 1


def _prune(registry: dict, handle: int, owner: List[int]) -> None:
    """Drop ``handle``'s entry once nothing about it is worth remembering.

    Pruning matters: an entry left behind on the *correct* create/destroy path
    would accumulate one dead key per cycle for the life of the ``Stage``, and
    once the C side recycled that handle the stale entry would reject the next
    legitimate release as already-released. The identity check keeps one owner's
    teardown from evicting a newer list that reused the handle.
    """
    if owner[_CREATED] <= 0 and owner[_ADDED] <= 0 and registry.get(handle) is owner:
        del registry[handle]


def _disown(registry: dict, handle: int, owner: List[int]) -> None:
    """Mark the create reference for ``handle`` released."""
    owner[_CREATED] = 0
    _prune(registry, handle, owner)


class PathList(int):
    """One owned reference to an interned path list (``ovx_primpath_list_t``).

    This *is* the handle — it subclasses :class:`int`, so it passes into
    :meth:`Stage.query_from_path_list`, :meth:`PathDictionary.path_list_count`,
    and every other slot that takes a path list unchanged, and
    ``isinstance(handle, int)`` still holds.

    What it adds is lifetime management. Path lists are refcounted C-side, and
    dropping a bare ``int`` does not decrement that refcount — the reference
    leaks for the life of the dictionary. Use it as a context manager so the
    reference is released even when an error interrupts processing::

        with paths.create_path_list_from_strings(["/World/A"]) as plist:
            stage.write_attribute(query, attr, ordinal=0, tensors=data).wait()
        # the create reference is released on block exit

    Explicit :meth:`release` (or :meth:`PathDictionary.destroy_path_list`) is
    equally fine. If that reference is still outstanding when the object is
    garbage-collected, :meth:`__del__` releases it and emits a
    :class:`ResourceWarning`; treat that as a bug report, not a strategy — in a
    per-frame loop, create the list once outside the loop and reuse it.

    It owns exactly one reference: the one ``create_path_list*`` minted. Anything
    added later with :meth:`PathDictionary.add_path_list_reference` is yours to
    release, and dropping this object never revokes it.

    A query built by :meth:`Stage.query_from_path_list` wraps a caller-owned list,
    so keep this object alive for as long as that query. The returned
    :class:`~ovstage.Query` holds a reference so the list cannot be finalized
    under a live query, but releasing the query does not release your list —
    bind it and release it yourself rather than passing a freshly created list
    inline, which leaves nothing to release and ends in the warning above.

    Only ``create_path_list*`` mints a :class:`PathList`. Borrowed lists handed
    back by read groups and query results stay plain ``int``, because the caller
    does not own a reference on them and must not release one.

    Ownership is tracked by handle on the backing :class:`Stage` — not by object
    identity, and not per wrapper — so releasing through a plain ``int`` copy of
    the handle (see :meth:`__copy__`) or through a different
    :class:`PathDictionary` over the same stage still clears this object's claim.

    That bookkeeping is unsynchronized, like the rest of the binding's handle
    state: releasing one handle from two threads at once can desync it. The C
    dictionary's *interning* is thread-safe, but these Python-side release
    decisions are not — own a given handle from one thread.

    The finalizer stays silent only once the C dictionary itself is gone — that
    is, when the :class:`Stage` backing it was destroyed. Closing a
    ``PathDictionary(stage)`` wrapper does *not* qualify: the shared dictionary
    outlives it, so a list outstanding at that point is still a live leak and is
    still reclaimed and reported.
    """

    def __new__(cls, path_list: int, dictionary: "PathDictionary", owner: List[int]) -> "PathList":
        self = super().__new__(cls, check_handle(path_list, "path_list"))
        # Weak, so a live path list cannot keep the dictionary (and its backing
        # Stage) alive.
        self._dictionary = weakref.ref(dictionary)
        # Single-element box shared with the Stage's registry, keyed by handle: 1
        # while this object still owns the reference create_path_list* minted, 0
        # once it is released. Keyed by handle rather than object identity so a
        # release routed through a *plain int* copy (see __copy__/__reduce__) or
        # through a different PathDictionary wrapper over the same Stage clears
        # the same flag; otherwise the finalizer would release a second time on a
        # handle the C side may already have recycled.
        self._owner = owner
        self._registry = dictionary._registry()
        # The C dictionary is kept alive by the backing Stage, NOT by this Python
        # wrapper: PathDictionary.destroy() only tears the C side down when it owns
        # that Stage. Capture both so the finalizer can tell "the list really died"
        # from "the wrapper was merely closed" -- with the documented
        # PathDictionary(stage) form the latter leaves the list very much alive.
        self._pd = dictionary._pd
        stage = dictionary._stage
        self._stage = weakref.ref(stage) if stage is not None else None
        return self

    @property
    def released(self) -> bool:
        """Whether the reference this object owns has been released."""
        return self._owner[_CREATED] <= 0

    def _c_dictionary_alive(self) -> bool:
        """Whether the C path dictionary behind this handle still exists.

        Tied to the backing :class:`Stage`, which owns the shared dictionary — not
        to the :class:`PathDictionary` wrapper, which may be closed while the
        dictionary (and this list) live on.
        """
        if self._stage is None or not self._pd:
            return False
        stage = self._stage()
        return stage is not None and bool(getattr(stage, "_inst", None))

    def _require_reference(self) -> None:
        """Reject a release that would drop a reference this object does not hold.

        Guards the explicit-release-then-finalizer path: without it, a list
        released with ``destroy_path_list`` and then garbage-collected would
        decrement the C refcount twice and free a list other holders still use.
        """
        if self.released:
            raise OvxError(
                _b.OVX_API_ERROR,
                f"path list {int(self)} has already been released; "
                "its reference is no longer owned by this object",
            )

    def _release_owned(self) -> None:
        """Drop the create reference, whether or not the wrapper is still open."""
        self._require_reference()
        dictionary = self._dictionary()
        if dictionary is not None and dictionary._pd:
            dictionary.destroy_path_list(self)  # keeps the registry in step
            return
        # The wrapper is gone or closed but the C dictionary outlived it (the
        # shared-Stage form). Release through the pointer captured at construction
        # rather than leaking the reference.
        if not self._c_dictionary_alive():
            raise OvxError(
                _b.OVX_API_ERROR,
                f"path list {int(self)} outlived the Stage backing its dictionary; "
                "the reference died with it",
            )
        _release_path_list_reference(self._pd, int(self))
        _disown(self._registry, int(self), self._owner)

    def release(self) -> None:
        """Release this object's reference (see :meth:`PathDictionary.destroy_path_list`)."""
        self._release_owned()

    def __enter__(self) -> "PathList":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        # Release only the create reference this block scopes. References the body
        # added with add_path_list_reference are the caller's, are not tracked
        # here, and survive both this exit and the finalizer -- so a bare handle
        # handed to a longer-lived consumer keeps working.
        if self.released:
            return
        if exc_type is None:
            self.release()  # surface release errors on the normal exit path
        else:
            # An exception is already propagating; do not mask it.
            try:
                self.release()
            except Exception:
                pass

    def __repr__(self) -> str:
        return f"PathList({int(self)}, {'released' if self.released else 'owned'})"

    # Formatting stays byte-identical to the bare ``int`` this replaced: defining
    # __repr__ alone would also change str()/f-strings, because int has no __str__
    # of its own — it inherits object.__str__, which defers to __repr__ — silently
    # rewriting callers' log output. Bind the digits-producing int.__repr__ here.
    __str__ = int.__repr__

    # A copy is not an owner. Copying/pickling yields the plain int handle rather
    # than a second object that would release the same reference twice — and it
    # keeps handles usable inside structures that get copied or serialized, which
    # a bare int always supported.
    def __copy__(self) -> int:
        return int(self)

    def __deepcopy__(self, memo) -> int:
        return int(self)

    def __reduce__(self):
        return (int, (int(self),))

    def __del__(self):
        # Safety net only. Skip when nothing is outstanding, or once the C
        # dictionary itself is gone (its Stage destroyed, including at interpreter
        # shutdown) -- then every handle it minted died with it, so there is
        # nothing to reclaim and a warning would be un-actionable teardown noise.
        # Note this deliberately does NOT key on the PathDictionary wrapper being
        # closed: with the shared-Stage form the C dictionary outlives it, and a
        # list outstanding then is a live leak that must still be reclaimed.
        try:
            if self.released or not self._c_dictionary_alive():
                return
        except Exception:
            return
        # Release BEFORE warning: warnings.warn raises when the caller escalates
        # ResourceWarning (-W error::ResourceWarning), so warning first would skip
        # the release for exactly the users who asked to be strict about resources.
        #
        # Exactly one reference is reclaimed: the one create_path_list* minted.
        # References the caller added with add_path_list_reference are sanctioned
        # by the path-dictionary contract as handles they may hold independently,
        # so reclaiming those would revoke a live borrow out from under them.
        try:
            self._release_owned()
        except Exception:
            pass  # report what we found regardless of how far the release got
        _warn_dropped_resource(
            f"PathList({int(self)}) was garbage-collected without release(); issued a "
            "best-effort release of the reference create_path_list* minted. Use a 'with' "
            "block or call release() explicitly, and reuse one list across a per-frame loop."
        )


class PathDictionary:
    """A shared interning dictionary for tokens, prim paths, and path lists.

    The dictionary is owned by an ovstage instance. Pass an existing
    :class:`~ovstage.Stage` to share its dictionary, or omit ``stage`` to manage
    a private backing instance whose lifetime is tied to this object.
    """

    def __init__(self, stage=None):
        from .stage import Stage  # local import avoids a module import cycle

        if stage is None:
            self._stage = Stage(name="ovstage-path-dictionary")
            self._owns_stage = True
        else:
            self._stage = stage
            self._owns_stage = False
        self._pd = self._stage.get_path_dictionary()

    # ── lifecycle ──────────────────────────────────────────────────────────
    def destroy(self) -> None:
        # The dictionary is owned by the instance; dropping the backing instance
        # (only when we created it) releases our hold on the shared dictionary.
        if self._owns_stage and self._stage is not None:
            self._stage.destroy()
        self._stage = None
        self._pd = None

    def __enter__(self) -> "PathDictionary":
        return self

    def __exit__(self, *_exc) -> None:
        self.destroy()

    def __del__(self):
        try:
            self.destroy()
        except Exception:
            pass

    # ── internals ───────────────────────────────────────────────────────────
    def _ensure_alive(self) -> None:
        if self._pd is None:
            raise OvxError(_b.OVX_API_ERROR, "PathDictionary has been destroyed")
        if self._stage is not None and hasattr(self._stage, "_inst") and not self._stage._inst:
            raise OvxError(_b.OVX_API_ERROR, "PathDictionary backing Stage has been destroyed")

    @property
    def _vtable(self):
        self._ensure_alive()
        return self._pd.contents.vtable.contents

    @property
    def _context(self):
        self._ensure_alive()
        return self._pd.contents.context

    def _check(self, result: "_b.ovx_api_result_t") -> None:
        # Copies the message out, then hands the owned error string back.
        _check_pd_result(self._pd, result)

    def _registry(self) -> dict:
        """Path-list ownership for this Stage, shared by every wrapper over it.

        Holds only the small owner boxes, never :class:`PathList` objects, so
        tracking can never pin a wrapper (or its handles) alive.
        """
        return self._stage._path_list_refs

    def _track(self, handle: int) -> "PathList":
        """Register the create reference for ``handle`` and wrap it."""
        registry = self._registry()
        owner = registry.get(handle)
        if owner is None:
            owner = [1, 0]
            registry[handle] = owner
        else:
            # Only reachable if the C side handed back a handle it still has
            # tracked. Share the one entry rather than minting a second, so two
            # wrappers cannot both release the same reference.
            owner[_CREATED] = 1
        return PathList(handle, self, owner)

    @staticmethod
    def _string_array(values: Sequence[str]):
        """Build an ``ovx_string_t[]`` plus a keepalive list for its backing buffers.

        Each value must be a real ``str``. Coercion would mint a
        wrong-but-successful handle (``None`` would intern as ``"/None"``, an
        ``int`` as its digits).
        """
        arr = (_b.ovx_string_t * len(values))()
        refs = []  # keep each ovx_string_t's _bytes buffer alive across the call
        for i, value in enumerate(values):
            if not isinstance(value, str):
                raise TypeError(f"expected str, got {type(value).__name__}: {value!r}")
            s = _b.ovx_string_t(value)
            arr[i] = s
            refs.append(s)
        return arr, refs

    # ── tokens ───────────────────────────────────────────────────────────────
    def intern_token(self, string: str) -> int:
        arr, _refs = self._string_array([string])
        out = (_b.ovx_token_t * 1)()
        self._check(self._vtable.create_tokens_from_strings(self._context, arr, 1, out))
        return int(out[0])

    def token_to_string(self, token: int) -> str:
        tokens = (_b.ovx_token_t * 1)(check_token(token, "token"))
        out = (_b.ovx_string_t * 1)()
        self._check(self._vtable.get_strings_from_tokens(self._context, tokens, 1, out))
        return str(out[0])

    # ── prim paths ────────────────────────────────────────────────────────────
    def intern_path(self, path: str) -> int:
        arr, _refs = self._string_array([path])
        out = (_b.ovx_primpath_t * 1)()
        self._check(self._vtable.create_paths_from_strings(self._context, arr, 1, out))
        return int(out[0])

    def path_to_string(self, primpath: int) -> str:
        """Resolve a prim-path handle to its path string.

        Raises :class:`OvxError` if the handle is invalid, unknown, or expired.
        The absolute root resolves to ``"/"``, matching ``SdfPath``; a valid
        handle never resolves to the empty string, so ``""`` is exclusively the
        shape no successful decode produces.
        """
        prim_paths = (_b.ovx_primpath_t * 1)(check_handle(primpath, "primpath"))
        buffer_size = _TOKEN_DECOMPOSE_BUFFER
        while True:
            token_buffer = (_b.ovx_token_t * buffer_size)()
            tokens_out = ctypes.POINTER(_b.ovx_token_t)()
            num_tokens = ctypes.c_size_t(0)
            num_processed = ctypes.c_size_t(0)
            self._check(
                self._vtable.get_tokens_from_paths(
                    self._context, prim_paths, 1, token_buffer, buffer_size,
                    ctypes.byref(tokens_out), ctypes.byref(num_tokens), ctypes.byref(num_processed),
                )
            )
            # The C API breaks *before* processing a path whose token count does
            # not fit the buffer, leaving num_processed == 0. A path that was
            # processed but decodes to zero tokens (the valid root path) reports
            # num_processed == 1, num_tokens == 0. Distinguish the two so
            # a deep path is not silently reported as "" (data loss).
            if num_processed.value != 0:
                if num_tokens.value == 0:
                    # Zero tokens is the absolute root. Join it to "/" to match
                    # USD (which has no empty spelling -- "" is USD's invalid
                    # path) and the native decoder behind this call, so no
                    # valid handle ever decodes to the empty string.
                    return "/"
                return "".join(
                    "/" + self.token_to_string(tokens_out[i]) for i in range(int(num_tokens.value))
                )
            # Buffer too small for this path's depth: grow and retry.
            buffer_size *= 2
            if buffer_size > _MAX_TOKEN_DECOMPOSE_BUFFER:
                raise OvxError(
                    _b.OVX_API_ERROR,
                    f"path token depth exceeds decompose ceiling of {_MAX_TOKEN_DECOMPOSE_BUFFER}",
                )

    # ── path lists ─────────────────────────────────────────────────────────────
    def create_path_list(self, primpaths: Sequence[int]) -> "PathList":
        """Build a path list from interned prim paths, owning one reference.

        The returned :class:`PathList` is the handle (an ``int`` subclass) and
        releases that reference on ``with``-exit, :meth:`PathList.release`, or
        :meth:`destroy_path_list`.
        """
        paths = check_token_sequence(primpaths, "primpaths entries")
        arr = (_b.ovx_primpath_t * len(paths))(*paths)
        out = (_b.ovx_primpath_list_t * 1)()
        self._check(self._vtable.create_path_list_from_paths(self._context, arr, len(paths), out))
        return self._track(int(out[0]))

    def create_path_list_from_strings(self, paths: Sequence[str]) -> "PathList":
        """Build a path list from path strings, owning one reference.

        See :meth:`create_path_list` for the returned handle's lifetime.
        """
        arr, _refs = self._string_array(list(paths))
        out = (_b.ovx_primpath_list_t * 1)()
        self._check(self._vtable.create_path_list_from_strings(self._context, arr, len(arr), out))
        return self._track(int(out[0]))

    def add_path_list_reference(self, path_list: int) -> None:
        """Increment a path list's refcount (pair with :meth:`destroy_path_list`).

        The added reference is yours alone: dropping the :class:`PathList` never
        revokes it, because the finalizer reclaims only the reference
        ``create_path_list*`` minted. The path-dictionary contract sanctions
        holding a bare handle beyond the object you added it through — release it
        with a matching :meth:`destroy_path_list`.

        The count is still recorded, so a later ``destroy_path_list`` through a
        plain ``int`` is attributed to one of *these* references rather than
        silently consuming the owner's claim.
        """
        handle = check_handle(path_list, "path_list")
        self._check(self._vtable.add_path_list_reference(self._context, handle))
        owner = self._registry().get(handle)
        if owner is not None:
            owner[_ADDED] += 1

    def destroy_path_list(self, path_list: int) -> None:
        """Release one reference on a path list (erases it at refcount zero).

        Which reference this drops is attributed as follows, because the C API
        counts references without distinguishing them:

        * passing the :class:`PathList` releases *its* reference, clearing the
          owner flag so the finalizer will not release it a second time;
        * passing a plain ``int`` releases one you added with
          :meth:`add_path_list_reference` when any are outstanding, and only falls
          through to the owner's reference when none are. Otherwise releasing an
          added reference through the handle would steal the owner's claim and
          strand the create reference for good.

        Ownership is keyed by handle on the :class:`Stage`, so this holds whether
        the call goes through the ``PathList``, a plain ``int`` copy of the handle,
        or a different :class:`PathDictionary` wrapper over the same stage. A
        handle no wrapper minted — a borrowed list from a read result, or one
        already fully released — is untracked and passes straight through to the C
        API, exactly as before.
        """
        handle = check_handle(path_list, "path_list")
        by_object = isinstance(path_list, PathList)
        registry = self._registry()
        owner = registry.get(handle)
        if by_object and owner is not None and owner[_CREATED] <= 0 and owner[_ADDED] <= 0:
            # Nothing tracked left to give. Clearer than the C invalid-handle error.
            path_list._require_reference()
        self._check(self._vtable.release_path_list_reference(self._context, handle))
        # Only after the C call succeeds — a failed release dropped nothing.
        if owner is None:
            return
        if by_object and owner[_CREATED] > 0:
            _disown(registry, handle, owner)
        elif owner[_ADDED] > 0:
            # Either the handle form, or the object form once its own reference is
            # already gone — releasing an added reference through the object is the
            # symmetric partner of add_path_list_reference(plist) and must work.
            owner[_ADDED] -= 1
            _prune(registry, handle, owner)
        else:
            _disown(registry, handle, owner)

    def path_list_count(self, path_list: int) -> int:
        count = ctypes.c_size_t()
        self._check(
            self._vtable.get_num_paths_from_path_list(
                self._context, check_handle(path_list, "path_list"), ctypes.byref(count)
            )
        )
        return int(count.value)

    def get_paths(self, path_list: int) -> List[int]:
        count = self.path_list_count(path_list)
        if count == 0:
            return []
        out = (_b.ovx_primpath_t * count)()
        retrieved = ctypes.c_size_t()
        self._check(
            self._vtable.get_paths_from_path_list(
                self._context, check_handle(path_list, "path_list"), 0, count, out, ctypes.byref(retrieved)
            )
        )
        return [int(out[i]) for i in range(int(retrieved.value))]

    def get_path_strings(self, path_list: int) -> List[str]:
        return [self.path_to_string(p) for p in self.get_paths(path_list)]
