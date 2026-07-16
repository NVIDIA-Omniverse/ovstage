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
from typing import List, Sequence

from . import bindings as _b

__all__ = ["PathDictionary", "OvxError"]

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
        self.code = int(code)
        self.message = message or ""
        super().__init__(f"ovx error {self.code}: {self.message}" if self.message else f"ovx error {self.code}")


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
        if result.status == _b.OVX_API_SUCCESS:
            return
        # Copy the message out, then hand the owned error string back before raising.
        message = str(result.error) if result.error else ""
        if result.error:
            self._vtable.release_error(self._context, result.error)
        raise OvxError(int(result.status), message)

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
        tokens = (_b.ovx_token_t * 1)(int(token))
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
        prim_paths = (_b.ovx_primpath_t * 1)(int(primpath))
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
            # processed but decodes to zero tokens (root / unresolved handle)
            # reports num_processed == 1, num_tokens == 0. Distinguish the two so
            # a deep path is not silently reported as "" (data loss).
            if num_processed.value != 0:
                if num_tokens.value == 0:
                    return ""
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
    def create_path_list(self, primpaths: Sequence[int]) -> int:
        paths = [int(p) for p in primpaths]
        arr = (_b.ovx_primpath_t * len(paths))(*paths)
        out = (_b.ovx_primpath_list_t * 1)()
        self._check(self._vtable.create_path_list_from_paths(self._context, arr, len(paths), out))
        return int(out[0])

    def create_path_list_from_strings(self, paths: Sequence[str]) -> int:
        arr, _refs = self._string_array(list(paths))
        out = (_b.ovx_primpath_list_t * 1)()
        self._check(self._vtable.create_path_list_from_strings(self._context, arr, len(arr), out))
        return int(out[0])

    def add_path_list_reference(self, path_list: int) -> None:
        """Increment a path list's refcount (pair with :meth:`destroy_path_list`)."""
        self._check(self._vtable.add_path_list_reference(self._context, int(path_list)))

    def destroy_path_list(self, path_list: int) -> None:
        """Release one reference on a path list (erases it at refcount zero)."""
        self._check(self._vtable.release_path_list_reference(self._context, int(path_list)))

    def path_list_count(self, path_list: int) -> int:
        count = ctypes.c_size_t()
        self._check(self._vtable.get_num_paths_from_path_list(self._context, int(path_list), ctypes.byref(count)))
        return int(count.value)

    def get_paths(self, path_list: int) -> List[int]:
        count = self.path_list_count(path_list)
        if count == 0:
            return []
        out = (_b.ovx_primpath_t * count)()
        retrieved = ctypes.c_size_t()
        self._check(
            self._vtable.get_paths_from_path_list(
                self._context, int(path_list), 0, count, out, ctypes.byref(retrieved)
            )
        )
        return [int(out[i]) for i in range(int(retrieved.value))]

    def get_path_strings(self, path_list: int) -> List[str]:
        return [self.path_to_string(p) for p in self.get_paths(path_list)]
