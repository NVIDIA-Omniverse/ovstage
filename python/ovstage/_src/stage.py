# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Pythonic ``Stage`` wrapper over the ovstage data-plane C API.

A :class:`Stage` owns an ``ovstage_instance_t``. Asynchronous calls return an
:class:`Operation` (whose ``.wait()`` returns it); handle-reserving calls return lightweight
:class:`Query` / :class:`Read` / :class:`Map` / :class:`OrdinalQuery` objects
that also carry their reserving op. Errors surface as :class:`OvstageError`.
Tensor data flows as numpy arrays (CPU) via DLPack.
"""

import ctypes
from typing import List, Optional, Sequence, TypeVar, Union

from . import bindings as _b
from .dlpack import DLDataType, DLTensor, make_dltensor
from .types import (
    AttributeSemantic,
    CloneOperation,
    CloneResult,
    Filter,
    HierarchyComputationModel,
    HierarchyComputationModelDesc,
    HierarchyItem,
    HierarchyResult,
    MapGroup,
    Operation,
    OrdinalAdmissionPolicy,
    OrdinalRange,
    OvstageError,
    Placement,
    PrimMode,
    AttributeMode,
    QueryResult,
    ReadGroup,
    Scope,
    StageConfig,
    TIMEOUT_INFINITE,
    WriteDesc,
    _warn_dropped_resource,
    check_element_sizes,
    check_enum,
    check_enum_member,
    check_handle,
    check_index_map,
    check_mask,
    check_ordinal,
    check_timeout,
    check_token_sequence,
    check_write_count,
)

__all__ = ["Stage", "Query", "Read", "Map", "OrdinalQuery", "Hierarchy"]

_TensorLike = Union[DLTensor, "object"]  # DLTensor or numpy array

# Stands in for ``typing.Self`` (Python 3.11+; the package floor is 3.10 with no
# typing_extensions dependency): ``Read.wait()`` is typed as returning ``Read``,
# ``Map.wait()`` as ``Map``, and so on. String bound — the class is defined below.
_HandleT = TypeVar("_HandleT", bound="_HandleObject")


def _handle(value) -> int:
    """Accept a handle wrapper (``.handle``) or a raw int handle."""
    if hasattr(value, "_ensure_active"):
        value._ensure_active()
    return check_handle(getattr(value, "handle", value), "handle")


def _require_placement(placement: Placement) -> Placement:
    """Reject a non-Placement before it reaches ctypes.

    A bare ``(device_type, device_id)`` tuple is the easy mistake here, and it
    would otherwise fail deep in ctypes with an opaque message instead of at the
    call the caller wrote.
    """
    if not isinstance(placement, Placement):
        raise TypeError(
            f"placement must be an ovstage.Placement, got {type(placement).__name__}; "
            "use Placement(...) or Placement.from_dlpack_device(...)"
        )
    return placement


class Stage:
    """An ovstage instance: the unit of stage-data read/write/query.

    Args:
        name: Optional instance name used for diagnostics.
        config: Optional process configuration. Its runtime-default hierarchy
            model controls automatic transform updates; its ordinal admission
            policy controls mutation ordering. Both are captured by this instance.
    """

    def __init__(self, name: Optional[str] = None, config: Optional[StageConfig] = None):
        if config is not None and not isinstance(config, StageConfig):
            raise TypeError(f"config must be a StageConfig or None, got {type(config).__name__}")

        self._lib = _b.load()
        # Data-plane calls dispatch through the instance vtable; _api forwards
        # instance->context to the resolved slot. Flat symbols (create/destroy,
        # ovstage_instancing_*, ovstage_population_*) are still called directly
        # on self._lib.
        self._api = _b.instance_api(self._lib, self._invalid_instance_error)
        self._inst = _b.ovstage_instance_p()
        self._holds_process_ref = False
        self._name_bytes = name.encode("utf-8") if name is not None else None
        desc = _b.ovstage_instance_desc_t()
        desc.name = self._name_bytes

        if config is not None:
            native_config = self._to_c_config(config)
            code = self._lib.ovstage_initialize(ctypes.byref(native_config))
            if code != _b.OVSTAGE_OK:
                raise OvstageError(code, self._last_error())
            self._holds_process_ref = True

        code = self._lib.ovstage_create_instance(ctypes.byref(desc), ctypes.byref(self._inst))
        if code != _b.OVSTAGE_OK:
            message = self._last_error()
            if self._holds_process_ref:
                self._lib.ovstage_shutdown()
                self._holds_process_ref = False
            raise OvstageError(code, message)
        # Ownership of caller-created path lists, keyed by handle (see PathList).
        # All PathDictionary wrappers over this Stage use the same dictionary
        # and registry, so releasing through one wrapper updates ownership for
        # every wrapper and prevents double release.
        self._path_list_refs = {}

    # ── lifecycle ──────────────────────────────────────────────────────────
    @staticmethod
    def _to_c_config(config: StageConfig) -> _b.ovstage_config_t:
        entries = []
        model = config.runtime_default_hierarchy_computation_model
        if model is not None:
            model = check_enum_member(
                model,
                HierarchyComputationModel,
                "runtime_default_hierarchy_computation_model",
            )
            if model == HierarchyComputationModel.INVALID:
                raise ValueError(
                    "runtime_default_hierarchy_computation_model must be CPU_INCREMENTAL, "
                    "GPU_INCREMENTAL, GPU_GLOBAL, or RUNTIME_DEFAULT"
                )
            entries.append(
                _b.ovstage_config_entry_uint64(
                    _b.OVSTAGE_CONFIG_RUNTIME_DEFAULT_HIERARCHY_COMPUTATION_MODEL,
                    model,
                )
            )
        policy = config.runtime_default_ordinal_admission_policy
        if policy is not None:
            policy = check_enum_member(
                policy,
                OrdinalAdmissionPolicy,
                "runtime_default_ordinal_admission_policy",
            )
            entries.append(
                _b.ovstage_config_entry_uint64(
                    _b.OVSTAGE_CONFIG_RUNTIME_DEFAULT_ORDINAL_ADMISSION_POLICY,
                    policy,
                )
            )
        return _b.ovstage_config_t(entries)

    @staticmethod
    def _from_c_config(config: _b.ovstage_config_t) -> StageConfig:
        model = None
        policy = None
        for i in range(config.entry_count):
            entry = config.entries[i]
            if (
                entry.key_type == _b.OVSTAGE_CONFIG_KEY_TYPE_UINT64
                and entry.key.uint64_key == _b.OVSTAGE_CONFIG_RUNTIME_DEFAULT_HIERARCHY_COMPUTATION_MODEL
            ):
                model = HierarchyComputationModel(entry.value.uint_value)
            elif (
                entry.key_type == _b.OVSTAGE_CONFIG_KEY_TYPE_UINT64
                and entry.key.uint64_key == _b.OVSTAGE_CONFIG_RUNTIME_DEFAULT_ORDINAL_ADMISSION_POLICY
            ):
                policy = OrdinalAdmissionPolicy(entry.value.uint_value)
        return StageConfig(
            runtime_default_hierarchy_computation_model=model,
            runtime_default_ordinal_admission_policy=policy,
        )

    @property
    def config(self) -> StageConfig:
        """Get a detached copy of the original process configuration entries.

        Omitted settings remain ``None`` and selectors remain selectors; this
        snapshot does not expand them into the resolved runtime defaults.
        """
        native_config = _b.ovstage_config_t([])
        code = self._lib.ovstage_get_config(self._require_inst(), ctypes.byref(native_config))
        if code != _b.OVSTAGE_OK:
            raise OvstageError(code, self._last_error())
        return self._from_c_config(native_config)

    @staticmethod
    def get_version() -> tuple:
        _b.load()
        return _b.library_version()

    def destroy(self) -> None:
        if self._inst:
            # ovstage_population's per-stage state (including any held USD-runtime
            # reference) is released automatically by ovstage_destroy_instance —
            # no manual detach is required (see ovstage_population.h lifecycle).
            code = self._lib.ovstage_destroy_instance(self._inst)
            if code != _b.OVSTAGE_OK:
                raise OvstageError(code, self._last_error())
            self._inst = _b.ovstage_instance_p()

        if self._holds_process_ref:
            code = self._lib.ovstage_shutdown()
            self._holds_process_ref = False
            if code != _b.OVSTAGE_OK:
                raise OvstageError(code, self._last_error())

    def __enter__(self) -> "Stage":
        return self

    def __exit__(self, *exc) -> None:
        self.destroy()

    def __del__(self):
        try:
            self.destroy()
        except Exception:
            pass

    # ── error / op plumbing ─────────────────────────────────────────────────
    def _last_error(self) -> str:
        # get_last_error is a thread-local free function (no instance), so it is
        # readable even when create failed and there is no bundle to dispatch on.
        return str(self._api.ovstage_get_last_error())

    def _last_op_error(self, op_id: int) -> str:
        # Returns an ovx_string_t ({NULL, 0} when the op id is unknown / did not
        # fail); ovx_string_t.__str__ yields "" for the empty view. A synchronous
        # rejection carries OVSTAGE_INVALID_OP_ID and records its detail in the
        # thread-local last-error instead (see ovstage_get_last_error in
        # ovstage_api.h). Fall back to the thread-local slot ONLY for the
        # invalid-id case: for a valid op an empty per-op message means the op
        # did not fail, and the thread-local text may be a stale, unrelated
        # earlier error that must not be attributed to it.
        if op_id != _b.OVSTAGE_INVALID_OP_ID:
            return str(self._api.ovstage_get_last_op_error(self._inst, op_id))
        return self._last_error()

    def _check(self, code: int) -> None:
        if code != _b.OVSTAGE_OK:
            raise OvstageError(code, self._last_error())

    def _flat_symbol(self, name: str):
        if not hasattr(self._lib, name):
            raise OvstageError(_b.OVSTAGE_ERROR_NOT_SUPPORTED, f"libovstage does not export {name}")
        return getattr(self._lib, name)

    @staticmethod
    def _invalid_instance_error(symbol: str) -> OvstageError:
        return OvstageError(_b.OVSTAGE_ERROR_INVALID_HANDLE, f"{symbol} called after Stage was destroyed")

    def _require_inst(self):
        if not self._inst:
            raise self._invalid_instance_error("flat ovstage API")
        return self._inst

    def _wait_and_release(self, op_id: int, timeout: int, on_complete=None) -> None:
        """Wait for ``op_id``, run ``on_complete`` while the completed op is still live, release it."""
        wait = _b.ovstage_op_wait_result_t()
        code = self._api.ovstage_wait_op(self._inst, op_id, timeout, ctypes.byref(wait))
        if code == _b.OVSTAGE_ERROR_TIMEOUT:
            # Not ready is not a terminal result: the op is still pending, and the
            # C contract allows release_op only once it has completed (releasing a
            # pending op is rejected and would orphan its tracking state). Leave
            # it tracked so a later wait can observe it.
            raise OvstageError(
                code,
                f"operation {op_id} is still pending (lowest pending op id {int(wait.lowest_pending_op_id)})",
            )
        # Ops that ran and failed -- the waited op or a failed producer in its
        # dependency chain -- are reported through wait.error_op_ids, and the
        # wait itself may still return OVSTAGE_OK (see wait_op in
        # ovstage_api.h). Mirror the C examples' checkWait: any reported
        # per-op error is a failure. The first failed id carries the
        # root-cause message; the waited op's own slot is empty when a
        # producer failed for it.
        error_count = int(wait.error_op_id_count) if wait.error_op_ids else 0
        if code != _b.OVSTAGE_OK or error_count != 0:
            failed_id = int(wait.error_op_ids[0]) if error_count else op_id
            msg = self._last_op_error(failed_id)
            self._api.ovstage_release_op(self._inst, op_id)  # best effort
            raise OvstageError(code if code != _b.OVSTAGE_OK else _b.OVSTAGE_ERROR_OP_FAILED, msg)
        if on_complete is not None:
            try:
                on_complete()
            except Exception:
                self._api.ovstage_release_op(self._inst, op_id)  # best effort
                raise
        rc = self._api.ovstage_release_op(self._inst, op_id)
        if rc != _b.OVSTAGE_OK:
            raise OvstageError(rc, self._last_op_error(op_id))

    def wait_op_raw(self, op_id: int, timeout: int = TIMEOUT_INFINITE) -> int:
        """Wait without raising or releasing; returns the raw ``ovstage_api_status_t``."""
        wait = _b.ovstage_op_wait_result_t()
        return int(
            self._api.ovstage_wait_op(
                self._inst, check_handle(op_id, "op_id"), check_timeout(timeout), ctypes.byref(wait)
            )
        )

    def wait_op(self, op_id: int, timeout: int = TIMEOUT_INFINITE):
        """Low-level wait: returns ``(code, error_op_ids, lowest_pending_op_id)``.

        Does not release the op. Lets callers inspect the wait result struct
        directly (e.g. timeout's ``lowest_pending_op_id`` or a failed producer's
        ``error_op_ids``).
        """
        wait = _b.ovstage_op_wait_result_t()
        code = int(
            self._api.ovstage_wait_op(
                self._inst, check_handle(op_id, "op_id"), check_timeout(timeout), ctypes.byref(wait)
            )
        )
        error_op_ids = (
            [int(wait.error_op_ids[i]) for i in range(int(wait.error_op_id_count))]
            if wait.error_op_ids
            else []
        )
        return code, error_op_ids, int(wait.lowest_pending_op_id)

    def release_op(self, op_id: int) -> int:
        """Release op tracking state; returns the raw ``ovstage_api_status_t``."""
        return int(self._api.ovstage_release_op(self._inst, check_handle(op_id, "op_id")))

    # ── query ───────────────────────────────────────────────────────────────
    def query(
        self,
        filter: Optional[Filter] = None,
        attrs: Optional[Sequence[int]] = None,
    ) -> "Query":
        """Enqueue a filter query. ``attrs`` scopes attribute discovery (tokens)."""
        keep: list = []
        filt_ref = None
        if filter is not None:
            cfilt, ka = filter.to_c()
            keep.append(cfilt)
            keep.extend(ka)
            filt_ref = ctypes.byref(cfilt)
        attr_ptr = None
        attr_count = 0
        if attrs:
            tokens = check_token_sequence(attrs, "attrs entries")
            arr = (_b.ovx_token_t * len(tokens))(*tokens)
            keep.append(arr)
            attr_ptr = ctypes.cast(arr, ctypes.POINTER(_b.ovx_token_t))
            attr_count = len(tokens)
        handle = _b.ovstage_query_handle_t()
        res = self._api.ovstage_query(self._inst, filt_ref, attr_ptr, attr_count, ctypes.byref(handle))
        return Query(self, int(handle.value), Operation(self, res.status, res.op_index, keepalive=keep))

    def query_from_path_list(self, path_list: int) -> "Query":
        """Create a query handle from an interned prim path list (synchronous).

        The handle wraps a **caller-owned** list (see ``query_from_path_list`` in
        ``ovstage_api.h``), so keep your list alive for as long as the query and
        release it when you are done::

            with paths.create_path_list_from_strings(paths_) as plist:
                with stage.query_from_path_list(plist) as query:
                    ...

        When given a :class:`PathList`, the returned :class:`Query` holds a
        reference to it, so the list cannot be finalized while the query is live.
        That is a safety net, not a transfer of ownership: releasing the query does
        not release your list. Passing a freshly created list inline —
        ``stage.query_from_path_list(paths.create_path_list_from_strings(...))`` —
        leaves no handle to release, so the list is eventually reclaimed by
        :meth:`PathList.__del__` with a :class:`ResourceWarning`. Bind it.
        """
        handle = _b.ovstage_query_handle_t()
        self._check(
            self._api.ovstage_query_from_path_list(
                self._inst, check_handle(path_list, "path_list"), ctypes.byref(handle)
            )
        )
        query = Query(self, int(handle.value), None)
        # Keepalive only — the reference stays the caller's to release.
        query._path_list = path_list
        return query

    def fetch_query_result(self, query, timeout: int = TIMEOUT_INFINITE) -> QueryResult:
        """Fetch (and release) a query result, copying out its scalar summary."""
        res = _b.ovstage_query_result_t()
        self._check(
            self._api.ovstage_fetch_query_result(self._inst, _handle(query), check_timeout(timeout), ctypes.byref(res))
        )
        attributes = [int(res.attributes[i]) for i in range(int(res.attribute_count))]
        out = QueryResult(
            attributes=attributes,
            total_prim_count=int(res.total_prim_count),
            all_handle=int(res.all_handle),
        )
        self._check(self._api.ovstage_release_query_result(self._inst, ctypes.byref(res)))
        return out

    def release_query(self, query) -> Operation:
        claimed = isinstance(query, _HandleObject)
        handle = query._claim_release() if claimed else _handle(query)
        try:
            res = self._api.ovstage_release_query(self._inst, handle)
        except Exception:
            if claimed:
                query._rollback_release()
            raise
        if res.status != _b.OVSTAGE_OK and claimed:
            query._rollback_release()
        # The release is enqueue-only and the query wraps a caller-owned list, so
        # carry the keepalive on the op: a caller who holds the returned Operation
        # (the documented `q.release().wait()` form) cannot let the list be
        # finalized while the release is still pending. A caller who discards the
        # op -- including the __del__ safety net -- gets no such protection; the
        # shipped implementation does not read the list after this point, but the
        # header does not promise that, so keep the list alive yourself if you
        # release without waiting.
        keepalive = getattr(query, "_path_list", None)
        return Operation(self, res.status, res.op_index, keepalive=[keepalive] if keepalive is not None else None)

    # ── read ─────────────────────────────────────────────────────────────────
    def read_attributes(
        self,
        query,
        attrs: Sequence[int],
        ordinal_range: OrdinalRange,
        *,
        placement: Optional[Placement] = None,
    ) -> "Read":
        """Enqueue a multi-attribute read; iterate groups with :meth:`Read.groups`.

        ``placement`` selects which of an attribute's copies the returned groups
        point at, so the payload ``DLTensor``\\ s report that device and a CUDA
        read yields pointers a kernel can consume without a host round-trip.
        ``None`` is the host, and is byte-for-byte the unplaced call.

        Unlike a placed write or map, a placed read is *permissive*: a column
        that is not current on ``placement.device_type``/``device_id`` is made
        available there rather than the read failing. Zero-copy survives where it
        is available -- a column already current on that device is pointed at
        directly and nothing is transferred. A column materialized onto the
        device lives in storage the group owns and is freed with
        :meth:`Stage.release_group`.

        ``placement.availability_event`` is what makes a transferring read
        asynchronous, and it changes when the payload may be touched. Without one
        the transfer is drained before the operation completes, so the data is
        ready as soon as :meth:`Read.fetch_next` hands the group over -- the
        behaviour an unplaced read already has. **With one, the pointers in the
        returned groups are not safe to dereference until the caller has waited
        on that event** (or has enqueued their own work on ``placement.stream``
        behind it). Wait once :meth:`Read.fetch_next` has returned a group: the
        event is recorded before any group is handed back, and a wait issued
        earlier may observe an old or unrecorded event. The read operation
        completes *before* the event fires, by design:
        the event marks when the column is available to dereference, not when
        this op finished, and what it waits out can include device work the
        caller never issued -- a write still in flight ahead of the read. A read
        served without a transfer records the event too, so a caller never has
        to know which case they got, and a wait on it never hangs.
        """
        attr_list = check_token_sequence(attrs, "attrs entries")
        arr = (_b.ovx_token_t * len(attr_list))(*attr_list)
        keep: list = [arr]
        handle = _b.ovstage_read_handle_t()
        attrs_ptr = ctypes.cast(arr, ctypes.POINTER(_b.ovx_token_t))
        if placement is None:
            res = self._api.ovstage_read_attributes(
                self._inst, _handle(query), attrs_ptr,
                len(attr_list), ordinal_range.to_c(), ctypes.byref(handle),
            )
        else:
            # Like the placed map slot, this one takes the placement *before* the
            # out-handle. Keep the struct alive on the op: the native side copies
            # it during enqueue, but the keepalive costs nothing and removes any
            # question about when that read happens.
            c_placement = _require_placement(placement)._to_c()
            keep.append(c_placement)
            res = self._api.ovstage_read_attributes_on_device(
                self._inst, _handle(query), attrs_ptr,
                len(attr_list), ordinal_range.to_c(), ctypes.byref(c_placement), ctypes.byref(handle),
            )
        return Read(self, int(handle.value), Operation(self, res.status, res.op_index, keepalive=keep))

    def fetch_read_next(self, read, timeout: int = TIMEOUT_INFINITE) -> Optional[ReadGroup]:
        """Fetch the next read group, or ``None`` at end of iteration."""
        grp = _b.ovstage_read_group_t()
        code = self._api.ovstage_fetch_read_next(self._inst, _handle(read), check_timeout(timeout), ctypes.byref(grp))
        if code == _b.OVSTAGE_ERROR_END_OF_ITERATION:
            return None
        if code != _b.OVSTAGE_OK:
            raise OvstageError(code, self._last_error())
        return ReadGroup(grp, self)

    def release_group(self, group: ReadGroup) -> None:
        """Release a read group's pinned storage.

        A group released twice would free storage the instance has already
        reclaimed, so a :class:`ReadGroup` raises instead — this is what keeps an
        explicit release plus a later finalizer from double-releasing.

        Release state is tracked on every :class:`ReadGroup`, including one a
        caller constructed themselves (no owning stage). Such a group has no
        finalizer, but it still needs the flag: without it a second
        ``release_group`` would reach the C API, and :meth:`ReadGroup.tensor` /
        :meth:`ReadGroup.prim_index` would keep reading storage that has been
        handed back.
        """
        claimed = isinstance(group, ReadGroup)
        if claimed:
            group._claim_release()
        try:
            self._check(self._api.ovstage_release_group(self._inst, ctypes.byref(group.raw)))
        except Exception:
            if claimed:
                group._rollback_release()  # nothing was reclaimed; stay releasable
            raise

    def release_read(self, read) -> Operation:
        claimed = isinstance(read, _HandleObject)
        handle = read._claim_release() if claimed else _handle(read)
        try:
            res = self._api.ovstage_release_read(self._inst, handle)
        except Exception:
            if claimed:
                read._rollback_release()
            raise
        if res.status != _b.OVSTAGE_OK and claimed:
            read._rollback_release()
        return Operation(self, res.status, res.op_index)

    # ── write / delete ────────────────────────────────────────────────────────
    def _build_write_data(
        self, tensors, is_array, index_map, mask, count, cuda_event, cuda_stream, semantic=AttributeSemantic.NONE,
        attribute_mode=AttributeMode.REJECT,
    ) -> "tuple[_b.ovstage_write_data_t, list]":
        if not isinstance(is_array, bool):
            raise TypeError("is_array must be a bool")
        if index_map is not None and mask is not None:
            raise ValueError("index_map and mask are mutually exclusive; pass at most one")
        # Normalize count before anything derives a bound from it. wd.count is a
        # uint32 and ctypes wraps instead of raising, so a negative came back as
        # a count near 2^32 that "exceeds the query" -- naming neither the sign
        # nor the argument that carried it -- and 2**32 became a silent
        # whole-query write. Guard here so the checks below see the real value.
        if count is not None:
            count = check_write_count(count)
        keep: list = []
        items = tensors if isinstance(tensors, (list, tuple)) else [tensors]
        if not items:
            raise ValueError("tensors must not be empty")
        dl_tensors = []
        for item in items:
            t = item if isinstance(item, DLTensor) else make_dltensor(item)
            dl_tensors.append(t)
            keep.append(t)  # holds t._array / t._shape_storage alive
        arr = (DLTensor * len(dl_tensors))(*dl_tensors)
        keep.append(arr)
        wd = _b.ovstage_write_data_t()
        wd.tensors = ctypes.cast(arr, ctypes.POINTER(DLTensor))
        wd.tensor_count = len(dl_tensors)
        wd.is_array = is_array
        if index_map is not None:
            # Entries are uint32 source rows and ctypes would wrap an
            # out-of-range one into a valid-looking row, so check and materialize
            # them once here; the bounds below and the C array share the result.
            indices = check_index_map(index_map)
            # The runtime reads exactly `count` entries from index_map -- one
            # source row per logical element -- so a count wider than the map
            # would read past the buffer this binding owns. Only Python knows
            # the map's length, so the bound has to be enforced here.
            resolved_count = len(indices) if count is None else count
            if resolved_count == 0:
                # 0 is the C contract's "every prim the query covers", which
                # cannot be what a map means: the runtime would leave it unread
                # and write the whole query. It rejects the pair, so fail here
                # while the empty argument that produced it is still visible.
                raise ValueError(
                    "index_map requires a non-zero count; an empty index_map (or count=0) addresses "
                    "no logical elements. Omit index_map to write every prim the query covers"
                )
            if resolved_count > len(indices):
                raise ValueError(
                    f"index_map must hold one entry per logical element: count={resolved_count} "
                    f"exceeds len(index_map)={len(indices)}. To address more of the query, "
                    "lengthen index_map (or use mask to select target prims)"
                )
            im = (ctypes.c_uint32 * len(indices))(*indices)
            keep.append(im)
            wd.index_map = ctypes.cast(im, ctypes.POINTER(ctypes.c_uint32))
            wd.count = resolved_count
        elif mask is not None:
            # Words are uint64; ctypes would wrap a negative one into an
            # all-bits-set word that selects every element.
            words = check_mask(mask)
            # The C contract requires a non-zero count whenever mask is set, and
            # the mask words cannot imply one: they are a bitset whose length is
            # a word count, not an element count. Defaulting to 0 here would
            # build a payload the runtime rejects with no way for the caller to
            # see why, so ask for the count instead.
            if count is None:
                raise ValueError(
                    "mask requires an explicit count (the number of logical elements the mask indexes)"
                )
            if count == 0:
                raise ValueError(
                    "mask requires a non-zero count; count=0 is the contract's 'every prim the query "
                    "covers', which would leave the mask unread"
                )
            # The runtime tests one bit per logical element, reading
            # ceil(count / 64) words, so a short mask reads past the buffer.
            if count > len(words) * 64:
                raise ValueError(
                    f"mask must hold at least {(count + 63) // 64} 64-bit word(s) to index "
                    f"{count} logical element(s); got {len(words)}"
                )
            mk = (ctypes.c_uint64 * len(words))(*words)
            keep.append(mk)
            wd.mask = ctypes.cast(mk, ctypes.POINTER(ctypes.c_uint64))
            wd.count = count
        elif count is not None:
            if count == 0:
                # The one count that silently does something else: 0 is the C
                # sentinel for the full query, so a caller passing len() of an
                # empty selection would write every prim instead of none.
                raise ValueError(
                    "count=0 does not address zero elements: it is the contract's 'every prim the "
                    "query covers'. Omit count to write the whole query"
                )
            wd.count = count
        # Populate {stream, wait_event}; unset fields stay 0 (no sync) — no
        # implicit default-stream pinning.
        if cuda_stream is not None:
            wd.cuda_sync.stream = cuda_stream
        if cuda_event is not None:
            wd.cuda_sync.wait_event = cuda_event
        # Range-only on purpose: semantic is documented to accept a raw
        # ovstage_attribute_semantic_t, so a value newer than the compiled-in
        # enum must still reach the library. Changes only together with the
        # map_attribute site below.
        wd.semantic = check_enum(semantic, "semantic")
        wd.attribute_mode = check_enum_member(attribute_mode, AttributeMode, "attribute_mode")
        return wd, keep

    def write_attribute(
        self,
        query,
        attribute: Union[int, str],
        ordinal: int,
        tensors,
        *,
        is_array: bool,
        prim_mode: PrimMode = PrimMode.UPSERT,
        semantic: int = 0,
        attribute_mode: AttributeMode = AttributeMode.REJECT,
        index_map: Optional[Sequence[int]] = None,
        mask: Optional[Sequence[int]] = None,
        count: Optional[int] = None,
        cuda_event: Optional[int] = None,
        cuda_stream: Optional[int] = None,
        placement: Optional[Placement] = None,
    ) -> Operation:
        """Enqueue a copy-in write.

        ``is_array`` explicitly declares the logical attribute kind and is never
        inferred from tensor count, tensor shape, payload width, attribute name,
        semantic, or existing storage. ``tensors`` is a numpy array /
        :class:`DLTensor`, or a list thereof (one per source row for array
        attributes). Caller-owned tensors are kept alive on the returned
        :class:`Operation` until ``wait()`` -- which is long enough only while
        ``wait()`` implies the device work drained. A ``placement`` carrying an
        ``availability_event`` can leave that work running past it; see
        :class:`Placement` for what the caller must then hold a reference to.

        Writing a different schema to an existing prim/name fails unless
        ``attribute_mode`` is :attr:`AttributeMode.FORCE`. Then a target prim whose
        existing schema differs has it replaced. A ``NONE`` semantic keeps the existing
        role on a prim whose storage is unchanged; on a prim whose storage is replaced
        the written schema has no role. The replacement is admitted once at ``ordinal`` and recorded there as the
        end of the previous schema on the replaced prims together with the written
        value; it is rejected while a replaced prim has an outstanding read or the
        attribute an overlapping map, leaving the previous schema in place. A write that
        fails after the previous schema has ended on a prim leaves that end recorded at
        ``ordinal`` and the prim without a value; a corrected retry for that prim needs a
        later ordinal. Other prims keep their schema, and reserved
        metadata attributes accept only :attr:`AttributeMode.REJECT`. A change-range
        read that includes ``ordinal`` returns, for each replaced prim, a tombstone group
        for the previous schema and a value group in the written schema, in unspecified
        order; a latest read returns only the written schema.

        Aliases of one stored column share ordinal admission, change tracking, and
        overlap checks on intersecting prim paths.
        The ordinal must exceed applicable write floors. With the
        ``OrdinalAdmissionPolicy.STRICTLY_INCREASING``, it must also be newer
        than retained committed changes to each affected cell and applicable
        prim lifecycle. ``NONDECREASING`` allows equal unsealed ordinals but
        rejects older intersecting history; ``FLOOR_ONLY`` skips retained-history
        and separate prim-lifecycle admission. Disjoint cells may share an ordinal.
        These policy choices also apply to map commits, clone and delete, each
        with its own applicable floors and lifecycle targets.
        Execution checks ordinal admission before changing stage data. Call
        ``wait()`` on the returned operation to observe admission and completion
        errors; enqueue acceptance or dropping the operation does not establish success.

        Fixed-size writes (``is_array=False``) are normalized internally.
        Raw reads return scalar-lane tensors with type-aware component axes
        (for example, ``(N, 3)`` for points and ``(N, 4, 4)`` for matrices);
        maps expose the same typed shapes. Logical prims select rows directly or
        through the group's data index map. The exact input shape is not
        retained. Without an index map, the leading dimension must equal the
        logical element count; a flat ``(N * L,)`` array is not inferred as
        ``N`` rows of width ``L``. This normalization does not describe
        array/ragged attributes.

        Array element types are never inferred from tensor shape. If a
        non-NumPy DLPack producer exposes a vector element as a trailing component
        axis (for example Warp ``vec3f`` as ``(N, 3)``, ``lanes=1``), first call
        :func:`make_dltensor` with the explicit lane dtype. That helper permits
        only a validated, compact trailing-axis fold, so ``point3f[]`` can remain
        zero-copy without ambiguously reinterpreting scalar arrays.

        Reserved metadata uses the same contract: ``usd-prim-type`` requires
        ``is_array=False``; ``usd-schemas`` and ``usd-ancestor-types`` require ``is_array=True``.
        None is implicitly broadcast; use ``index_map`` when
        target rows intentionally share source data.

        ``count`` is the number of logical elements the write addresses — the
        leading ``count`` prims of the query, in query order. With neither
        ``index_map`` nor ``mask`` it defaults to the query's prim count. When
        given it must be positive: ``0`` is the C contract's spelling of "the
        whole query", so passing it — including as ``len()`` of an empty
        selection — raises ``ValueError`` rather than writing nothing.
        ``index_map`` and ``mask`` are mutually exclusive and both refine that
        logical element axis:

        * ``index_map[i]`` is the **source row** logical element ``i`` reads from,
          not the prim being written. Use it to gather, reorder, or broadcast
          rows (``index_map=[0, 0]`` writes one source row to two prims). Every
          entry must be less than the transported row count, which for a
          fixed-size write is ``shape[0]``. The map holds one entry per logical
          element, so ``count`` defaults to ``len(index_map)`` and may not
          exceed it — to address more of the query, lengthen the map rather than
          raising ``count``.
        * ``mask`` is a bitmask over the same logical element axis selecting
          which prims are written; unselected prims are left untouched. It has
          no default ``count``: supply one, along with enough 64-bit words to
          cover it (``ceil(count / 64)``).

        To write a subset of a query's prims, use ``mask``; ``index_map`` selects
        source data, not targets.

        All three carry the width of the C fields behind them: ``count`` and
        every ``index_map`` entry must fit in ``uint32`` and each ``mask`` word
        in ``uint64``. An out-of-range value raises :class:`ValueError` rather
        than wrapping into the field — ``count=2**32`` is rejected, not silently
        turned into the whole-query ``0``.

        ``semantic`` is the :class:`AttributeSemantic` (or raw
        ``ovstage_attribute_semantic_t`` value) carried on the write. Geometric
        semantics (POINT/VECTOR/NORMAL/COLOR/QUATERNION/MATRIX/FRAME/TEXTURE_COORDINATE)
        and TIME_CODE record a role on the column; ID semantics select the
        corresponding ID storage type and require pre-interned id payloads
        (``TOKEN_ID`` / ``RELATIONSHIP_PATH_ID`` use ``dtype = (kDLUInt, 64, 1)``,
        ``CONNECTION_PATH_ID`` uses ``dtype = (kDLUInt, 64, 2)``). ``0`` (NONE)
        writes the payload without stamping a role / base type on the column.

        Like :meth:`write_attributes`, this operation is not transactional. A
        successfully applied entry commits its values, any new prims or
        attributes, and their change records at the submitted ordinal. A failed
        entry is not recorded as a completed write, although changes made before
        the failure can remain visible to a ``HAS`` query. Because this operation
        has only one entry, a failed application leaves the submitted ordinal
        available; a corrected same-ordinal retry remains admissible subject to
        current stage state. The exception is a :attr:`AttributeMode.FORCE` write
        that ended a prim's previous schema before failing: that end stays recorded,
        so a retry for that prim needs a later ordinal. INSERT treats a prim left by a failed write as an
        existing target and can reject it with
        ``ErrorCode.PRIM_NOT_FOUND``; UPSERT can repair it. Recoverable allocation
        and size failures follow the normal :class:`OvstageError` path. Only
        failure to record an already-applied entry's prim, attribute and ordinal
        changes terminates the process; Python cannot catch that as an exception.
        """
        # Keep the singular Python entry point aligned with the native API; the
        # native slot relays to the common plural implementation.
        wd, keep = self._build_write_data(
            tensors, is_array, index_map, mask, count, cuda_event, cuda_stream, semantic, attribute_mode
        )
        sot = _b.make_string_or_token(attribute)
        keep.append(sot)
        checked_ordinal = check_ordinal(ordinal)
        checked_prim_mode = check_enum_member(prim_mode, PrimMode, "prim_mode")
        if placement is None:
            res = self._api.ovstage_write_attribute(
                self._inst, _handle(query), sot, checked_ordinal, wd, checked_prim_mode
            )
        else:
            # Keep the placement struct alive until the op completes: the native
            # side reads it during enqueue, but the keepalive costs nothing and
            # removes any question about when that read happens.
            c_placement = _require_placement(placement)._to_c()
            keep.append(c_placement)
            res = self._api.ovstage_write_attribute_on_device(
                self._inst, _handle(query), sot, checked_ordinal, wd, checked_prim_mode,
                ctypes.byref(c_placement),
            )
        return Operation(self, res.status, res.op_index, keepalive=keep)

    def write_attributes(
        self,
        query,
        writes: Sequence[WriteDesc],
        ordinal: int,
        *,
        prim_mode: PrimMode = PrimMode.UPSERT,
        placement: Optional[Placement] = None,
    ) -> Operation:
        """Enqueue one copy-in operation for multiple attribute columns.

        Every write undergoes common preflight before the operation is queued.
        Native preflight failures, including known write-floor violations, are
        recorded in the returned operation's ``status`` and raised by ``wait()``.
        Python argument validation raises immediately. Successful enqueue can
        still be followed by ordinal-admission failure, also reported by ``wait()``.
        Admission follows the policy described by :meth:`write_attribute` and
        is checked for the complete batch before any entry changes stage data.
        The returned operation owns all ctypes/tensor keepalives until ``wait()``.
        Each fixed-size entry follows the same read/map normalization as
        :meth:`write_attribute`.

        Compatible aliases of one stored column may appear together only when
        their selected prim coverage is disjoint.

        The operation is not transactional. If a later entry fails, ``wait()``
        reports failure for the operation, while entries whose application
        completed successfully remain committed, including any new prims or
        attributes and their change records at the submitted ordinal. The failed
        entry is not recorded as a completed write, although changes made before
        the failure can remain visible to a ``HAS`` query. Preparing storage alone
        does not count as a completed write or record changes at the submitted
        ordinal. Reserved metadata may complete before ordinary attribute values;
        it remains committed if a later entry fails.

        The API exposes no per-entry completion mask, so repair a whole batch that
        may have completed an entry with a newer ordinal: the
        ``STRICTLY_INCREASING`` policy rejects rewriting completed entries at
        their recorded ordinal. If no entry completed,
        including when the sole entry of a one-entry call did not complete, the
        ordinal remains available and a corrected same-ordinal retry remains
        admissible subject to current stage state, except for a prim whose previous
        schema an :attr:`AttributeMode.FORCE` entry ended before the failure: that end
        stays recorded, so its retry needs a later ordinal. Recoverable allocation or size
        failures follow the normal :class:`OvstageError` path while the stage and
        its ordinal change records can remain consistent with completed entries.

        A failed write can leave a prim visible to queries without recording its
        creation at the submitted ordinal. INSERT treats that prim as an existing
        target and rejects it with ``ErrorCode.PRIM_NOT_FOUND``. A later successful
        UPSERT commits its attribute changes and their ordinal records but does
        not record a prim-creation event for that already-present prim.

        Only failure to record an already-applied entry's prim, attribute and
        ordinal changes terminates the process; Python cannot catch that as an
        exception.
        """
        items = list(writes)
        keep: list = []
        write_array = (_b.ovstage_attribute_write_t * len(items))()
        keep.append(write_array)

        for index, item in enumerate(items):
            if not isinstance(item, WriteDesc):
                raise TypeError("writes must contain WriteDesc instances")
            wd, data_keep = self._build_write_data(
                item.tensors, item.is_array, item.index_map, item.mask, item.count,
                item.cuda_event, item.cuda_stream, item.semantic, item.attribute_mode
            )
            sot = _b.make_string_or_token(item.attribute)
            write_array[index].attribute = sot
            write_array[index].data = wd
            keep.append(sot)
            keep.extend(data_keep)

        writes_ptr = (
            ctypes.cast(write_array, ctypes.POINTER(_b.ovstage_attribute_write_t))
            if items
            else None
        )
        checked_ordinal = check_ordinal(ordinal)
        checked_prim_mode = check_enum_member(prim_mode, PrimMode, "prim_mode")
        if placement is None:
            res = self._api.ovstage_write_attributes(
                self._inst, _handle(query), writes_ptr, len(items), checked_ordinal, checked_prim_mode,
            )
        else:
            c_placement = _require_placement(placement)._to_c()
            keep.append(c_placement)
            res = self._api.ovstage_write_attributes_on_device(
                self._inst, _handle(query), writes_ptr, len(items), checked_ordinal, checked_prim_mode,
                ctypes.byref(c_placement),
            )
        return Operation(self, res.status, res.op_index, keepalive=keep)

    def delete_attributes(self, query, attributes: Sequence[Union[int, str]], ordinal: int) -> Operation:
        """Enqueue a delete. Empty ``attributes`` deletes entire prims.

        The ordinal must exceed applicable floors. The admission policy described
        by :meth:`write_attribute` also checks retained changes to deleted cells
        and, for whole-prim deletion, the affected prim lifecycles. ``FLOOR_ONLY``
        skips those history and separate lifecycle checks. Call ``wait()`` on the
        returned operation to observe failures discovered after enqueue.
        """
        attrs = list(attributes)
        keep: list = []
        attr_ptr = None
        if attrs:
            arr = (_b.ovx_string_or_token_t * len(attrs))()
            for i, a in enumerate(attrs):
                sot = _b.make_string_or_token(a)
                arr[i] = sot
                keep.append(sot)
            keep.append(arr)
            attr_ptr = ctypes.cast(arr, ctypes.POINTER(_b.ovx_string_or_token_t))
        res = self._api.ovstage_delete_attributes(self._inst, _handle(query), attr_ptr, len(attrs), check_ordinal(ordinal))
        return Operation(self, res.status, res.op_index, keepalive=keep)

    # ── clone (subtree clone) ───────────────────────────────────────────────
    def clone(self, source_path: str, target_paths: Sequence[str], ordinal: int) -> CloneResult:
        """Clone the subtree under ``source_path`` to each path in ``target_paths`` (blocking).

        Data-plane peer of ovrtx's ``clone_usd`` (the ``_usd`` postfix is dropped).
        The ordinal must exceed the global write floor and the effective floors
        of the source subtree's attributes. For nonempty targets, it must also
        exceed the effective floors of every reproduced destination attribute,
        including copied prim type, applied schema and ancestor type metadata.
        These floor checks run at execution time under every admission policy.

        Independently of the source floor checks, the configured history policy
        checks retained destination-cell changes and every derived target prim's
        lifecycle, including the effective ``usd-path`` floor. The
        ``STRICTLY_INCREASING`` policy requires a newer ordinal than intersecting
        retained changes; ``NONDECREASING`` allows equality but rejects older
        ordinals. ``FLOOR_ONLY`` skips retained-history and separate lifecycle
        admission. With no targets, only the source/global floor checks apply.
        Copied cells record changes at ``ordinal``, so later customization needs
        a newer ordinal under the strict policy; ``NONDECREASING`` permits the
        same ordinal while it remains above applicable floors. Disjoint target
        cells may share an ordinal.

        The source must exist; each target must not already exist, and a target
        path may appear only once, independently of the admission policy. Returns
        the clone's :class:`CloneResult`. Execution failure raises
        :class:`OvstageError`; use :meth:`fetch_clone_result` with the low-level
        async path to inspect the clone's underlying error code.
        """
        return self.clone_async(source_path, target_paths, ordinal).wait().result()

    def clone_async(self, source_path: str, target_paths: Sequence[str], ordinal: int) -> CloneOperation:
        """Enqueue a subtree clone of ``source_path`` to ``target_paths`` at ``ordinal`` (asynchronous).

        Returns a :class:`CloneOperation` — an :class:`Operation` whose
        :meth:`~CloneOperation.result` yields the :class:`CloneResult` once the
        clone has completed (``op.wait().result()``).
        The source, target and ordinal requirements are those of :meth:`clone`.
        Enqueue success does not guarantee execution success: call ``wait()``
        to observe completion errors.
        """
        targets = list(target_paths)
        src = _b.ovx_string_t(source_path)
        keep: list = [src]
        tgt_ptr = None
        if targets:
            arr = (_b.ovx_string_t * len(targets))()
            for i, t in enumerate(targets):
                s = _b.ovx_string_t(t)
                arr[i] = s
                keep.append(s)
            keep.append(arr)
            tgt_ptr = ctypes.cast(arr, ctypes.POINTER(_b.ovx_string_t))
        res = self._lib.ovstage_clone(self._require_inst(), src, tgt_ptr, len(targets), check_ordinal(ordinal))
        return CloneOperation(self, res.status, res.op_index, keepalive=keep)

    def fetch_clone_result(self, op_or_id, timeout: int = TIMEOUT_INFINITE) -> CloneResult:
        """Fetch a clone's completion report by op id (low-level ``ovstage_fetch_clone_result``).

        The report is per-op state that lives until the op is released, so this is
        the C-shaped path — ``Stage.wait_op`` / ``fetch_clone_result`` /
        ``Stage.release_op`` — and the way to observe a failed clone's own error
        code (``wait()`` reports every failure as ``OP_FAILED``). After
        :meth:`CloneOperation.wait`, which releases the op, it raises
        ``INVALID_HANDLE``; use :meth:`CloneOperation.result` there instead.

        :param op_or_id: the :class:`CloneOperation` or its integer op id.
        :param timeout: max nanoseconds to wait for completion; ``0`` polls.
        :raises OvstageError: ``TIMEOUT`` while the clone is pending (it stays
            waitable), ``INVALID_HANDLE`` for an id that is not a live clone op, or
            the clone's own failure code with its message.
        """
        op_id = check_handle(getattr(op_or_id, "op_id", op_or_id), "op_id")
        fn = self._flat_symbol("ovstage_fetch_clone_result")
        raw = _b.ovstage_clone_result_t()
        code = fn(self._require_inst(), op_id, check_timeout(timeout), ctypes.byref(raw))
        if code != _b.OVSTAGE_OK:
            # Synchronous rejections record their detail in the thread-local slot;
            # a clone that ran and failed left its message on the op.
            synchronous = (_b.OVSTAGE_ERROR_INVALID_ARGUMENT, _b.OVSTAGE_ERROR_INVALID_HANDLE, _b.OVSTAGE_ERROR_TIMEOUT)
            raise OvstageError(code, self._last_error() if code in synchronous else self._last_op_error(op_id))
        return CloneResult(
            prims_cloned=int(raw.prims_cloned),
            target_count=int(raw.target_count),
            source_prim_count=int(raw.source_prim_count),
            ordinal=int(raw.ordinal),
        )

    # -- hierarchy ---------------------------------------------------------
    def get_hierarchy_computation_models(self) -> List[HierarchyComputationModelDesc]:
        """Return the hierarchy computation models supported by this runtime."""
        fn = self._flat_symbol("ovstage_get_hierarchy_computation_models")
        models = ctypes.POINTER(_b.ovstage_hierarchy_computation_model_desc_t)()
        count = ctypes.c_size_t()
        self._check(fn(self._require_inst(), ctypes.byref(models), ctypes.byref(count)))
        return [
            HierarchyComputationModelDesc(
                model_id=int(models[i].model_id),
                name=str(models[i].name),
                description=str(models[i].description),
            )
            for i in range(int(count.value))
        ]

    def compute_hierarchy(
        self,
        input_ordinal: int,
        output_ordinal: int,
        model: int = HierarchyComputationModel.DEFAULT_CPU,
    ) -> None:
        """Compute hierarchy-derived world transforms (blocking).

        Computation uses the currently connected hierarchy. Children may compute
        as roots while their parents remain unavailable.

        The computation uses the latest committed inputs. It records the
        rewritten ``omni:fabric:worldMatrix`` rows at ``output_ordinal``, also
        when the column is already sealed there. Compute before you seal when
        consumers need a complete snapshot at that ordinal. The current
        implementation ignores ``input_ordinal``.
        """
        self.compute_hierarchy_async(input_ordinal, output_ordinal, model).wait()

    def compute_hierarchy_async(
        self,
        input_ordinal: int,
        output_ordinal: int,
        model: int = HierarchyComputationModel.DEFAULT_CPU,
    ) -> Operation:
        """Enqueue hierarchy-derived world transform computation.

        The computation uses the latest committed inputs. It records the
        rewritten ``omni:fabric:worldMatrix`` rows at ``output_ordinal``, also
        when the column is already sealed there. Compute before you seal when
        consumers need a complete snapshot at that ordinal. The current
        implementation ignores ``input_ordinal``.
        """
        fn = self._flat_symbol("ovstage_compute_hierarchy")
        res = fn(
            # Range-only on purpose: model ids are discovered at runtime via
            # get_hierarchy_computation_models, so they must not be checked
            # against the enum this wheel happens to ship.
            self._require_inst(), check_enum(model, "model"),
            check_ordinal(input_ordinal), check_ordinal(output_ordinal),
        )
        return Operation(self, res.status, res.op_index)

    def get_hierarchy(self, path_list: int, ordinal: int, relation: int) -> HierarchyResult:
        """Return parent/children/siblings for an ordered path list (blocking).

        This reads the current committed hierarchy when the operation
        executes. ``ordinal`` is echoed as a caller-supplied tag; it does not
        select a historical hierarchy snapshot.
        """
        hierarchy = self.get_hierarchy_async(path_list, ordinal, relation)
        try:
            hierarchy.wait()
            result = hierarchy.result()
        except Exception:
            if hierarchy.handle:
                try:
                    hierarchy.release().wait()
                except Exception:
                    pass
            raise
        else:
            hierarchy.release().wait()
            return result

    def get_hierarchy_async(self, path_list: int, ordinal: int, relation: int) -> "Hierarchy":
        """Enqueue a parent/children/siblings lookup for an ordered path list."""
        fn = self._flat_symbol("ovstage_get_hierarchy")
        handle = _b.ovstage_hierarchy_handle_t()
        res = fn(
            self._require_inst(), check_handle(path_list, "path_list"), check_ordinal(ordinal),
            # Range-only on purpose: test_get_hierarchy_validates_relation_and_release
            # pins that the *runtime* rejects an unknown relation, returning
            # INVALID_ARGUMENT with a zero handle. Pre-rejecting here would make
            # that native path unreachable from Python. Same treatment as `model`.
            check_enum(relation, "relation"), ctypes.byref(handle),
        )
        return Hierarchy(self, int(handle.value), Operation(self, res.status, res.op_index))

    @staticmethod
    def _hierarchy_path(path: _b.ovx_string_or_token_t) -> str:
        if path.token:
            return str(int(path.token))
        return str(path.string)

    @staticmethod
    def _copy_hierarchy_result(raw: _b.ovstage_hierarchy_result_t) -> HierarchyResult:
        items: List[HierarchyItem] = []
        input_count = int(raw.input_count)
        total_path_count = int(raw.path_count)
        for item_index in range(input_count):
            raw_item = raw.items[item_index]
            offset = int(raw_item.path_offset)
            count = int(raw_item.path_count)
            if offset > total_path_count or count > total_path_count - offset:
                raise OvstageError(_b.OVSTAGE_ERROR_INTERNAL, "hierarchy result path range is invalid")
            paths = tuple(Stage._hierarchy_path(raw.paths[offset + i]) for i in range(count))
            items.append(HierarchyItem(status=int(raw_item.status), paths=paths))
        return HierarchyResult(ordinal=int(raw.ordinal), items=items)

    def fetch_hierarchy_result(self, hierarchy) -> HierarchyResult:
        """Fetch, copy, and release a completed hierarchy lookup result payload."""
        raw = _b.ovstage_hierarchy_result_t()
        fn_fetch = self._flat_symbol("ovstage_fetch_hierarchy_result")
        fn_release = self._flat_symbol("ovstage_release_hierarchy_result")
        self._check(fn_fetch(self._require_inst(), _handle(hierarchy), ctypes.byref(raw)))
        try:
            result = self._copy_hierarchy_result(raw)
        finally:
            release_code = fn_release(self._require_inst(), ctypes.byref(raw))
        self._check(release_code)
        return result

    def release_hierarchy(self, hierarchy) -> Operation:
        claimed = isinstance(hierarchy, _HandleObject)
        handle = hierarchy._claim_release() if claimed else _handle(hierarchy)
        fn = self._flat_symbol("ovstage_release_hierarchy")
        try:
            res = fn(self._require_inst(), handle)
        except Exception:
            if claimed:
                hierarchy._rollback_release()
            raise
        if res.status != _b.OVSTAGE_OK and claimed:
            hierarchy._rollback_release()
        return Operation(self, res.status, res.op_index)

    # -- map / unmap (mapped writes) ---------------------------------------
    def map_attribute(
        self,
        query,
        attribute: Union[int, str],
        ordinal: int,
        *,
        prim_mode: PrimMode = PrimMode.UPSERT,
        dtype: Optional[DLDataType] = None,
        semantic: int = 0,
        element_sizes: Optional[Sequence[int]] = None,
        attribute_mode: AttributeMode = AttributeMode.REJECT,
        placement: Optional[Placement] = None,
    ) -> "Map":
        """Reserve a map session for writing an attribute.

        ``dtype`` specifies the element storage type, with the complete tuple
        width in ``dtype.lanes``. It is required for a new attribute. If omitted,
        the existing schema must be unambiguous for the target prims. A requested
        dtype that conflicts with existing storage, or semantic that conflicts
        with an authored role on a target prim/name, is rejected; delete that
        attribute before mapping it with a new schema, or pass
        ``attribute_mode=AttributeMode.FORCE`` with ``dtype`` to replace that schema
        when each group commits, under the same rules as a forcing write, including
        when a commit fails after the previous schema has ended. Setup and each commit
        are rejected while a prim they would replace has an outstanding read. An
        omitted semantic
        adopts an unambiguous compatible existing identity. Specify dtype and
        semantic when the target prims have multiple same-name identities.
        Fixed-size map groups expose the same scalar-lane layout as reads:
        scalar lanes with shape ``(data_rows,)`` for scalars,
        ``(data_rows, C)`` for tuples, or ``(data_rows, S, S)`` for square
        matrices. Use the group's ``data_row_index(local)`` to resolve
        a logical element through any data index map. Shapes follow the stored
        type and semantic rather than the original convenience write shape.
        ``semantic`` is the :class:`AttributeSemantic` (or raw
        ``ovstage_attribute_semantic_t`` value) carried on the map. Geometric
        semantics and TIME_CODE record a role on the column when unmap
        succeeds; ID semantics select the corresponding ID storage type and
        require pre-interned ids in the map buffer (``TOKEN_ID`` /
        ``RELATIONSHIP_PATH_ID`` use ``dtype = (kDLUInt, 64, 1)``,
        ``CONNECTION_PATH_ID`` uses ``dtype = (kDLUInt, 64, 2)``).
        ``element_sizes`` gives per-prim element counts for ragged columns.

        Setup and publication use the admission policy described by
        :meth:`write_attribute`, against the affected cells and applicable prim
        lifecycles. Every policy retains the applicable write-floor checks.
        A successful enqueue can still fail ordinal admission during setup,
        before writable groups are made available. Observe setup failure through
        the returned map's ``wait()`` or ``fetch_next()``.
        Successful setup only makes writable staging buffers available. It does
        not change stage values, create prims or attributes, modify schemas or
        hierarchy, or record value or prim-lifecycle changes at the session's
        ordinal. These effects commit only for groups whose unmap succeeds.
        Commit rechecks the current layout, type/semantic compatibility, prim
        creation policy, ordinal admission and read overlap, so a ready map can
        still fail to commit. See :class:`Map`
        for failure and finalization rules.
        """
        keep: list = []
        sizes_ptr = None
        sizes_count = 0
        if element_sizes is not None:
            entries = check_element_sizes(element_sizes)
            sizes = (ctypes.c_size_t * len(entries))(*entries)
            keep.append(sizes)
            sizes_ptr = ctypes.cast(sizes, ctypes.POINTER(ctypes.c_size_t))
            # Length of the materialized list: it is the array the runtime reads,
            # so the count it is paired with should be derived from the same thing.
            sizes_count = len(entries)
        sot = _b.make_string_or_token(attribute)
        keep.append(sot)  # keeps sot._string_ref (the attribute string buffer) alive
        desc = _b.ovstage_map_desc_t()
        desc.attribute = sot
        if dtype is not None:
            desc.dtype = dtype
        # Range-only on purpose -- see the write_attribute site.
        desc.semantic = check_enum(semantic, "semantic")
        desc.prim_mode = check_enum_member(prim_mode, PrimMode, "prim_mode")
        desc.attribute_mode = check_enum_member(attribute_mode, AttributeMode, "attribute_mode")
        keep.append(desc)
        handle = _b.ovstage_map_handle_t()
        if placement is None:
            res = self._api.ovstage_map_attribute(
                self._inst, _handle(query), ctypes.byref(desc), check_ordinal(ordinal),
                sizes_ptr, sizes_count, ctypes.byref(handle),
            )
        else:
            # The placed map takes the placement *before* out_map_handle.
            c_placement = _require_placement(placement)._to_c()
            keep.append(c_placement)
            res = self._api.ovstage_map_attribute_on_device(
                self._inst, _handle(query), ctypes.byref(desc), check_ordinal(ordinal),
                sizes_ptr, sizes_count, ctypes.byref(c_placement), ctypes.byref(handle),
            )
        return Map(self, int(handle.value), Operation(self, res.status, res.op_index, keepalive=keep))

    def fetch_map_next(self, mapping, timeout: int = TIMEOUT_INFINITE) -> Optional[MapGroup]:
        grp = _b.ovstage_map_group_t()
        code = self._api.ovstage_fetch_map_next(self._inst, _handle(mapping), check_timeout(timeout), ctypes.byref(grp))
        if code == _b.OVSTAGE_ERROR_END_OF_ITERATION:
            return None
        if code != _b.OVSTAGE_OK:
            raise OvstageError(code, self._last_error())
        return MapGroup(grp, mapping=mapping if isinstance(mapping, Map) else None)

    def unmap_group(
        self, mapping, group: MapGroup, cuda_event: Optional[int] = None, cuda_stream: Optional[int] = None
    ) -> Operation:
        """Enqueue a group commit; its changes take effect once on success.

        An accepted commit that fails makes the session terminal. Finalize it
        with :meth:`unmap_attribute`; retry requires a new session. Previously
        committed groups remain committed. See :class:`Map` for failure rules.
        The exception is a session mapped with a :class:`Placement` carrying an
        ``availability_event``: an otherwise successful commit whose event CUDA
        refuses to record fails with ``OP_FAILED`` after committing, its changes
        stay recorded, and the session stays open, so later commits and final
        unmap proceed as usual.

        Unplaced maps use CPU storage: leave ``cuda_event`` / ``cuda_stream``
        unset for those maps. A CPU commit with payload rejects a non-zero
        value as an accepted, terminal failure. For CUDA-placed maps, these
        arguments supply the source handoff before the commit reads mapped
        values. A cleanup-only final unmap -- every group already committed --
        does not inspect the handoff.
        """
        sync = _b.ovstage_cuda_sync_t(cuda_stream or 0, cuda_event or 0)
        handle = _handle(mapping)
        group._claim_unmap()
        try:
            res = self._api.ovstage_unmap_group(self._inst, handle, ctypes.byref(group.raw), sync)
        except Exception:
            group._rollback_unmap()
            raise
        if res.status != _b.OVSTAGE_OK:
            group._rollback_unmap()
        return Operation(self, res.status, res.op_index)

    def unmap_attribute(
        self, mapping, cuda_event: Optional[int] = None, cuda_stream: Optional[int] = None
    ) -> Operation:
        """Enqueue remaining commits and release the map handle.

        An accepted finalization consumes the handle even if execution fails.
        If all groups committed earlier, it only releases the session, without
        reapplying changes or repeating commit admission checks. If an accepted
        group commit failed, it releases the session and reports the retained
        failure without committing remaining data; a group commit that failed
        only because its ``availability_event`` could not be recorded is not
        such a failure (see :meth:`unmap_group`). An otherwise successful
        finalization whose own event cannot be recorded fails with
        ``OP_FAILED`` after committing the remaining groups, whose changes stay
        recorded.

        Unplaced maps use CPU storage: leave ``cuda_event`` / ``cuda_stream``
        unset for those maps. A CPU commit with payload rejects a non-zero
        value as an accepted, terminal failure. For CUDA-placed maps, these
        arguments supply the source handoff before the commit reads mapped
        values. A cleanup-only final unmap -- every group already committed --
        does not inspect the handoff.
        """
        claimed = isinstance(mapping, Map)
        handle = mapping._claim_unmap() if claimed else _handle(mapping)
        sync = _b.ovstage_cuda_sync_t(cuda_stream or 0, cuda_event or 0)
        try:
            res = self._api.ovstage_unmap_attribute(self._inst, handle, sync)
        except Exception:
            if claimed:
                mapping._rollback_unmap()
            raise
        if res.status != _b.OVSTAGE_OK and claimed:
            mapping._rollback_unmap()
        return Operation(self, res.status, res.op_index)

    # ── ordinal management / write floor ────────────────────────────────────
    def advance_write_floor(
        self,
        ordinal: int,
        scope: Scope = Scope.ALL,
        attributes: Optional[Sequence[int]] = None,
    ) -> Operation:
        """Advance the write floor; ``scope`` selects the affected attributes (see :class:`Scope`).

        Advances clamp to the current value, so a non-monotonic ordinal is a no-op
        rather than an error.

        Attribute aliases advance one shared floor.
        """
        desc = _b.ovstage_write_floor_desc_t()
        desc.ordinal = check_ordinal(ordinal)
        desc.scope = check_enum_member(scope, Scope, "scope")
        keep: list = []
        if attributes:
            tokens = check_token_sequence(attributes, "attributes entries")
            arr = (_b.ovx_token_t * len(tokens))(*tokens)
            keep.append(arr)
            desc.attributes = ctypes.cast(arr, ctypes.POINTER(_b.ovx_token_t))
            # Length of the materialized list, for the same reason as the array
            # above it: this count describes `arr`, so it is derived from `arr`.
            desc.attribute_count = len(tokens)
        res = self._api.ovstage_advance_write_floor(self._inst, ctypes.byref(desc))
        return Operation(self, res.status, res.op_index, keepalive=keep)

    def get_oldest_preserved_ordinal(self) -> "OrdinalQuery":
        handle = _b.ovstage_ordinal_query_handle_t()
        res = self._api.ovstage_get_oldest_preserved_ordinal(self._inst, ctypes.byref(handle))
        return OrdinalQuery(self, int(handle.value), Operation(self, res.status, res.op_index))

    def get_attribute_write_floor(self, attribute: Optional[Union[int, str]] = None) -> "OrdinalQuery":
        """Query a per-attribute write floor, or the global write floor if ``attribute`` is ``None``.

        Aliases of one stored column report the same floor.
        """
        sot = _b.ovx_string_or_token_t() if attribute is None else _b.make_string_or_token(attribute)
        handle = _b.ovstage_ordinal_query_handle_t()
        res = self._api.ovstage_get_attribute_write_floor(self._inst, sot, ctypes.byref(handle))
        return OrdinalQuery(self, int(handle.value), Operation(self, res.status, res.op_index))

    def fetch_ordinal(self, ordinal_query, timeout: int = TIMEOUT_INFINITE) -> int:
        value = _b.ovstage_ordinal_t()
        self._check(
            self._api.ovstage_fetch_ordinal(self._inst, _handle(ordinal_query), check_timeout(timeout), ctypes.byref(value))
        )
        return int(value.value)

    def release_ordinal_query(self, ordinal_query) -> Operation:
        claimed = isinstance(ordinal_query, _HandleObject)
        handle = ordinal_query._claim_release() if claimed else _handle(ordinal_query)
        try:
            res = self._api.ovstage_release_ordinal_query(self._inst, handle)
        except Exception:
            if claimed:
                ordinal_query._rollback_release()
            raise
        if res.status != _b.OVSTAGE_OK and claimed:
            ordinal_query._rollback_release()
        return Operation(self, res.status, res.op_index)

    # ── resources ────────────────────────────────────────────────────────────
    def get_path_dictionary(self) -> "_b.path_dictionary_instance_p":
        """Return the instance's path-dictionary bundle (``{vtable, context}``).

        The dictionary is owned by this instance and stays valid while it is
        alive; list references do not extend that lifetime. Callers must not
        tear the dictionary down. This is the raw bundle
        the binding dispatches through, **not** a :class:`ovstage.PathDictionary`
        — to work with tokens, paths, and path lists from Python, construct
        ``PathDictionary(stage)``, which calls this for you.
        """
        pd = self._api.ovstage_get_path_dictionary(self._inst)
        if not pd:
            raise OvstageError(_b.OVSTAGE_ERROR_NOT_SUPPORTED, "no path dictionary available for this instance")
        return pd

class _HandleObject:
    """Common base for handle wrappers carrying their reserving op.

    :class:`Query`, :class:`Read`, and :class:`OrdinalQuery` reserve a C-side
    handle that must be released with :meth:`release`; its resources (attribute
    discovery lists, borrowed prim-path-list references, internal refcounts) stay
    pinned until then. Use the handle as a context manager to guarantee release
    even when an error interrupts processing::

        with stage.query(filter=f) as q:
            count = q.wait().result().total_prim_count
        # release() runs on block exit

    If a handle is dropped without release, :meth:`__del__` issues a best-effort
    release and emits a :class:`ResourceWarning`; rely on the context manager (or
    an explicit ``release()``) rather than the finalizer. (:class:`Map` overrides
    this teardown with its own unmap-based lifecycle.)
    """

    def __init__(self, stage: Stage, handle: int, op: Optional[Operation]):
        self._stage = stage
        self.handle = check_handle(handle, "handle")
        self.op = op
        self._released = False

    def _ensure_active(self) -> None:
        """Reject use of a handle whose C-side reservation has been released.

        After :meth:`release` the handle is stale; passing it back to the C API
        would target a freed / recycled reservation. Raise a clear error instead.
        """
        if self._released:
            raise OvstageError(
                _b.OVSTAGE_ERROR_INVALID_HANDLE,
                f"{type(self).__name__} (handle={self.handle}) has been released; "
                "its C-side handle is no longer valid",
            )

    def _claim_release(self) -> int:
        self._ensure_active()
        self._released = True
        return self.handle

    def _rollback_release(self) -> None:
        self._released = False

    def wait(self: _HandleT, timeout: int = TIMEOUT_INFINITE) -> _HandleT:
        """Wait on the reserving op and return this handle.

        Returning the handle lets the follow-on call chain — ``group =
        read.wait().fetch_next()`` — inside the handle's ``with`` block (chaining
        on the temporary drops the handle unreleased). Waiting is also what
        releases the reserving op, so it is not redundant with the fetch's own
        wait. A synchronously-created handle has no reserving op: the call is a
        no-op that still returns the handle. See :meth:`Operation.wait` for the
        timeout and error contract.
        """
        if self.op is not None:
            self.op.wait(timeout)
        return self

    def __int__(self) -> int:
        return self.handle

    def __enter__(self: _HandleT) -> _HandleT:
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self._released or not self.handle:
            return
        op = self.release()  # subclass hits the matching release_* slot
        if exc_type is None:
            op.wait()  # surface release errors on the normal exit path
        else:
            # An exception is already propagating; release best-effort so it
            # does not mask the original error.
            try:
                op.wait()
            except Exception:
                pass

    def __del__(self):
        # Safety net only. Skip if already released, if the reservation never
        # succeeded (invalid handle, so nothing to reclaim), or if the owning
        # instance is gone (e.g. interpreter shutdown), where dispatch is unsafe.
        if self._released or not self.handle or not getattr(self._stage, "_inst", None):
            return
        # Release BEFORE warning: warnings.warn raises when the caller escalates
        # ResourceWarning (-W error::ResourceWarning), so warning first would skip
        # the release for exactly the users who asked to be strict about resources.
        try:
            self.release()  # enqueue-only; the handle is reclaimed when the op runs
        except Exception:
            pass
        _warn_dropped_resource(
            f"{type(self).__name__} (handle={self.handle}) was garbage-collected without "
            "release(); issued a best-effort release to free the pinned handle. Use a "
            "'with' block or call release() explicitly."
        )


class Query(_HandleObject):
    def result(self, timeout: int = TIMEOUT_INFINITE) -> QueryResult:
        """Fetch (and release) the query result.

        :param timeout: max nanoseconds to wait for the result;
            ``TIMEOUT_INFINITE`` (default) blocks, ``0`` polls.
        :raises TypeError: if ``timeout`` is not an integer (e.g. ``None``).
        :raises ValueError: if ``timeout`` is negative or does not fit in uint64.
        """
        return self._stage.fetch_query_result(self, timeout)

    def release(self) -> Operation:
        return self._stage.release_query(self)


class Read(_HandleObject):
    def fetch_next(self, timeout: int = TIMEOUT_INFINITE) -> Optional[ReadGroup]:
        """Fetch the next read group, or ``None`` at end of iteration.

        :param timeout: max nanoseconds to wait for the next group;
            ``TIMEOUT_INFINITE`` (default) blocks, ``0`` polls.
        :raises TypeError: if ``timeout`` is not an integer (e.g. ``None``).
        :raises ValueError: if ``timeout`` is negative or does not fit in uint64.
        :raises OvstageError: with ``ErrorCode.TIMEOUT`` if no group is ready in time.
        """
        return self._stage.fetch_read_next(self, timeout)

    def groups(self, timeout: int = TIMEOUT_INFINITE):
        """Generator over read groups. The caller must release each group."""
        while True:
            group = self._stage.fetch_read_next(self, timeout)
            if group is None:
                return
            yield group

    def release(self) -> Operation:
        return self._stage.release_read(self)


class Map(_HandleObject):
    """A reserved map session for writing an attribute.

    Reserve via :meth:`Stage.map_attribute`, iterate writable groups with
    :meth:`groups` / :meth:`fetch_next`, fill each group, and commit it with
    :meth:`unmap_group`. Finalize with :meth:`unmap`, which commits any
    remaining groups and releases the handle.

    Setup only prepares writable staging buffers; :meth:`Stage.map_attribute`
    describes the effects deferred until successful commit. An outstanding map
    blocks overlapping operations on the same canonical attribute and prims.
    Commit rechecks admission against current state, so successful setup does
    not guarantee a successful commit.

    Fetched groups retain this session, preventing garbage collection from
    finalizing it while a group is still in use. Accepted group or final unmap
    submissions invalidate the affected groups immediately. Previously exported
    arrays and DLPack views must no longer be accessed after submission.

    Each successful group commits its values, any prim or attribute creation,
    schema or hierarchy changes, and ordinal change records once. A failed group
    does not record these as completed changes and does not undo groups committed
    earlier. Map commits are not transactional: a failed commit may have changed
    some or all stored rows, and those changes can remain observable. No rollback
    or atomicity is guaranteed.

    An accepted group commit that fails during execution makes the session
    terminal. Its groups cannot be retried. Final :meth:`unmap` releases the
    session and reports the retained failure without committing remaining data.
    Recovery requires a new session or another write operation. The exception
    is a commit that fails only because a placement's ``availability_event``
    could not be recorded: it has committed, and the session stays open (see
    :meth:`Stage.unmap_group`). If all groups
    committed successfully, final unmap only releases the session; it neither
    reapplies changes nor repeats commit admission checks. An accepted final
    unmap consumes the handle even when execution fails. To finalize on exit,
    use the session as a context manager::

        with stage.map_attribute(query, attr, ordinal=o) as m:
            for mg in m.wait().groups():
                fill(mg)
                m.unmap_group(mg).wait()

    Leaving the ``with`` block — normally or via an exception — calls
    :meth:`unmap`. The C API has no cancel: for a session without a prior accepted
    commit failure, final unmap attempts to commit whatever the remaining buffers
    hold. An exception mid-fill can therefore commit partially filled buffers if
    finalization succeeds. If a session is dropped without unmapping,
    :meth:`__del__` issues a best-effort unmap and a
    :class:`ResourceWarning`; it does not wait for or report asynchronous commit
    failures. Normal context-manager exit waits and reports errors; explicit
    ``unmap().wait()`` also observes completion.
    """

    def __init__(self, stage: Stage, handle: int, op: Optional[Operation]):
        super().__init__(stage, handle, op)
        self._unmapped = False

    def _ensure_active(self) -> None:
        super()._ensure_active()
        if self._unmapped:
            raise OvstageError(
                _b.OVSTAGE_ERROR_INVALID_HANDLE,
                f"Map (handle={self.handle}) has been unmapped; its C-side handle is no longer valid",
            )

    def _claim_unmap(self) -> int:
        self._ensure_active()
        self._unmapped = True
        return self.handle

    def _rollback_unmap(self) -> None:
        self._unmapped = False

    def fetch_next(self, timeout: int = TIMEOUT_INFINITE) -> Optional[MapGroup]:
        """Fetch the next writable map group, or ``None`` at end of iteration.

        :param timeout: max nanoseconds to wait for the next group;
            ``TIMEOUT_INFINITE`` (default) blocks, ``0`` polls.
        :raises TypeError: if ``timeout`` is not an integer (e.g. ``None``).
        :raises ValueError: if ``timeout`` is negative or does not fit in uint64.
        :raises OvstageError: with ``ErrorCode.TIMEOUT`` if no group is ready in time.
        """
        return self._stage.fetch_map_next(self, timeout)

    def groups(self, timeout: int = TIMEOUT_INFINITE):
        while True:
            group = self._stage.fetch_map_next(self, timeout)
            if group is None:
                return
            yield group

    def unmap_group(
        self, group: MapGroup, cuda_event: Optional[int] = None, cuda_stream: Optional[int] = None
    ) -> Operation:
        return self._stage.unmap_group(self, group, cuda_event, cuda_stream)

    def unmap(self, cuda_event: Optional[int] = None, cuda_stream: Optional[int] = None) -> Operation:
        """Commit remaining groups and release the map handle (see class docstring)."""
        return self._stage.unmap_attribute(self, cuda_event, cuda_stream)

    def __exit__(self, exc_type, exc, tb) -> None:
        # `not self.handle` matches _HandleObject.__exit__ and Map.__del__: a
        # synchronously rejected map_attribute returns Map(handle=0), and
        # unmapping handle 0 fails with INVALID_ARGUMENT, masking the real setup
        # error. The failed unmap also rolls _unmapped back to False, so without
        # this guard an explicit unmap() leaves the bogus call to repeat here.
        if self._unmapped or not self.handle:
            return
        op = self.unmap()
        if exc_type is None:
            op.wait()  # surface commit errors on the normal exit path
        else:
            # An exception is already propagating; release the session
            # best-effort so it does not mask the original error.
            try:
                op.wait()
            except Exception:
                pass

    def __del__(self):
        # Safety net only. Skip if already unmapped, if the reservation never
        # succeeded (invalid handle, so no session to release), or if the owning
        # instance is gone (e.g. interpreter shutdown), where dispatch is unsafe.
        if self._unmapped or not self.handle or not getattr(self._stage, "_inst", None):
            return
        # Unmap BEFORE warning, for the same reason as _HandleObject.__del__:
        # warnings.warn raises under -W error::ResourceWarning, and warning first
        # would strand the pinned session for the users who asked to be strict.
        try:
            self.unmap()  # enqueue-only; the session releases when the op runs
        except Exception:
            pass
        _warn_dropped_resource(
            f"Map (handle={self.handle}) was garbage-collected without unmap(); "
            "issued a best-effort unmap to release the pinned session. Use a "
            "'with' block or call unmap() explicitly."
        )


class OrdinalQuery(_HandleObject):
    def fetch(self, timeout: int = TIMEOUT_INFINITE) -> int:
        """Fetch the queried ordinal value.

        :param timeout: max nanoseconds to wait for the value;
            ``TIMEOUT_INFINITE`` (default) blocks, ``0`` polls.
        :raises TypeError: if ``timeout`` is not an integer (e.g. ``None``).
        :raises ValueError: if ``timeout`` is negative or does not fit in uint64.
        """
        return self._stage.fetch_ordinal(self, timeout)

    def release(self) -> Operation:
        return self._stage.release_ordinal_query(self)


class Hierarchy(_HandleObject):
    def result(self) -> HierarchyResult:
        return self._stage.fetch_hierarchy_result(self)

    def release(self) -> Operation:
        return self._stage.release_hierarchy(self)
