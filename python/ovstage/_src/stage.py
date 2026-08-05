# Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

"""Pythonic ``Stage`` wrapper over the ovstage data-plane C API.

A :class:`Stage` owns an ``ovstage_instance_t``. Asynchronous calls return an
:class:`Operation` (with ``.wait()``); handle-reserving calls return lightweight
:class:`Query` / :class:`Read` / :class:`Map` / :class:`OrdinalQuery` objects
that also carry their reserving op. Errors surface as :class:`OvstageError`.
Tensor data flows as numpy arrays (CPU) via DLPack.
"""

import ctypes
import warnings
from typing import List, Optional, Sequence, Union

from . import bindings as _b
from .dlpack import DLDataType, DLTensor, make_dltensor
from .types import (
    AttributeSemantic,
    Filter,
    HierarchyComputationModel,
    HierarchyComputationModelDesc,
    HierarchyItem,
    HierarchyResult,
    MapGroup,
    Operation,
    OrdinalRange,
    OvstageError,
    PrimMode,
    QueryResult,
    ReadGroup,
    Scope,
    StageConfig,
    TIMEOUT_INFINITE,
    WriteDesc,
    check_ordinal,
    check_timeout,
)

__all__ = ["Stage", "Query", "Read", "Map", "OrdinalQuery", "Hierarchy"]

_TensorLike = Union[DLTensor, "object"]  # DLTensor or numpy array


def _handle(value) -> int:
    """Accept a handle wrapper (``.handle``) or a raw int handle."""
    if hasattr(value, "_ensure_active"):
        value._ensure_active()
    return int(getattr(value, "handle", value))


class Stage:
    """An ovstage instance: the unit of stage-data read/write/query.

    Args:
        name: Optional instance name used for diagnostics.
        config: Optional process configuration. Its runtime-default hierarchy
            model is captured by this instance and controls automatic transform
            updates.
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

    # ── lifecycle ──────────────────────────────────────────────────────────
    @staticmethod
    def _to_c_config(config: StageConfig) -> _b.ovstage_config_t:
        entries = []
        model = config.runtime_default_hierarchy_computation_model
        if model is not None:
            try:
                model = HierarchyComputationModel(model)
            except (TypeError, ValueError):
                raise ValueError(
                    "runtime_default_hierarchy_computation_model must be a "
                    f"HierarchyComputationModel, got {model!r}"
                ) from None
            if model == HierarchyComputationModel.INVALID:
                raise ValueError(
                    "runtime_default_hierarchy_computation_model must be CPU_INCREMENTAL, "
                    "GPU_INCREMENTAL, GPU_GLOBAL, or RUNTIME_DEFAULT"
                )
            entries.append(
                _b.ovstage_config_entry_uint64(
                    _b.OVSTAGE_CONFIG_RUNTIME_DEFAULT_HIERARCHY_COMPUTATION_MODEL,
                    int(model),
                )
            )
        return _b.ovstage_config_t(entries)

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

    def _wait_and_release(self, op_id: int, timeout: int) -> None:
        wait = _b.ovstage_op_wait_result_t()
        code = self._api.ovstage_wait_op(self._inst, op_id, timeout, ctypes.byref(wait))
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
        rc = self._api.ovstage_release_op(self._inst, op_id)
        if rc != _b.OVSTAGE_OK:
            raise OvstageError(rc, self._last_op_error(op_id))

    def wait_op_raw(self, op_id: int, timeout: int = TIMEOUT_INFINITE) -> int:
        """Wait without raising or releasing; returns the raw ``ovstage_api_status_t``."""
        wait = _b.ovstage_op_wait_result_t()
        return int(self._api.ovstage_wait_op(self._inst, op_id, check_timeout(timeout), ctypes.byref(wait)))

    def wait_op(self, op_id: int, timeout: int = TIMEOUT_INFINITE):
        """Low-level wait: returns ``(code, error_op_ids, lowest_pending_op_id)``.

        Does not release the op. Lets callers inspect the wait result struct
        directly (e.g. timeout's ``lowest_pending_op_id`` or a failed producer's
        ``error_op_ids``).
        """
        wait = _b.ovstage_op_wait_result_t()
        code = int(self._api.ovstage_wait_op(self._inst, op_id, check_timeout(timeout), ctypes.byref(wait)))
        error_op_ids = (
            [int(wait.error_op_ids[i]) for i in range(int(wait.error_op_id_count))]
            if wait.error_op_ids
            else []
        )
        return code, error_op_ids, int(wait.lowest_pending_op_id)

    def release_op(self, op_id: int) -> int:
        """Release op tracking state; returns the raw ``ovstage_api_status_t``."""
        return int(self._api.ovstage_release_op(self._inst, op_id))

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
            arr = (_b.ovx_token_t * len(attrs))(*[int(a) for a in attrs])
            keep.append(arr)
            attr_ptr = ctypes.cast(arr, ctypes.POINTER(_b.ovx_token_t))
            attr_count = len(attrs)
        handle = _b.ovstage_query_handle_t()
        res = self._api.ovstage_query(self._inst, filt_ref, attr_ptr, attr_count, ctypes.byref(handle))
        return Query(self, int(handle.value), Operation(self, res.status, res.op_index, keepalive=keep))

    def query_from_path_list(self, path_list: int) -> "Query":
        """Create a query handle from an interned prim path list (synchronous)."""
        handle = _b.ovstage_query_handle_t()
        self._check(self._api.ovstage_query_from_path_list(self._inst, int(path_list), ctypes.byref(handle)))
        return Query(self, int(handle.value), None)

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
        return Operation(self, res.status, res.op_index)

    # ── read ─────────────────────────────────────────────────────────────────
    def read_attributes(self, query, attrs: Sequence[int], ordinal_range: OrdinalRange) -> "Read":
        attr_list = [int(a) for a in attrs]
        arr = (_b.ovx_token_t * len(attr_list))(*attr_list)
        handle = _b.ovstage_read_handle_t()
        res = self._api.ovstage_read_attributes(
            self._inst, _handle(query), ctypes.cast(arr, ctypes.POINTER(_b.ovx_token_t)),
            len(attr_list), ordinal_range.to_c(), ctypes.byref(handle),
        )
        return Read(self, int(handle.value), Operation(self, res.status, res.op_index, keepalive=[arr]))

    def fetch_read_next(self, read, timeout: int = TIMEOUT_INFINITE) -> Optional[ReadGroup]:
        """Fetch the next read group, or ``None`` at end of iteration."""
        grp = _b.ovstage_read_group_t()
        code = self._api.ovstage_fetch_read_next(self._inst, _handle(read), check_timeout(timeout), ctypes.byref(grp))
        if code == _b.OVSTAGE_ERROR_END_OF_ITERATION:
            return None
        if code != _b.OVSTAGE_OK:
            raise OvstageError(code, self._last_error())
        return ReadGroup(grp)

    def release_group(self, group: ReadGroup) -> None:
        self._check(self._api.ovstage_release_group(self._inst, ctypes.byref(group.raw)))

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
        self, tensors, is_array, index_map, mask, count, cuda_event, cuda_stream, semantic=AttributeSemantic.NONE
    ) -> "tuple[_b.ovstage_write_data_t, list]":
        if not isinstance(is_array, bool):
            raise TypeError("is_array must be a bool")
        if index_map is not None and mask is not None:
            raise ValueError("index_map and mask are mutually exclusive; pass at most one")
        # wd.count is a uint32, so a negative wraps to a value near 2^32 and is
        # reported back as a count that exceeds the query -- naming neither the
        # sign nor the argument that carried it.
        if count is not None and count < 0:
            raise ValueError(f"count must not be negative; got {count}")
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
            # The runtime reads exactly `count` entries from index_map -- one
            # source row per logical element -- so a count wider than the map
            # would read past the buffer this binding owns. Only Python knows
            # the map's length, so the bound has to be enforced here.
            resolved_count = len(index_map) if count is None else count
            if resolved_count == 0:
                # 0 is the C contract's "every prim the query covers", which
                # cannot be what a map means: the runtime would leave it unread
                # and write the whole query. It rejects the pair, so fail here
                # while the empty argument that produced it is still visible.
                raise ValueError(
                    "index_map requires a non-zero count; an empty index_map (or count=0) addresses "
                    "no logical elements. Omit index_map to write every prim the query covers"
                )
            if resolved_count > len(index_map):
                raise ValueError(
                    f"index_map must hold one entry per logical element: count={resolved_count} "
                    f"exceeds len(index_map)={len(index_map)}. To address more of the query, "
                    "lengthen index_map (or use mask to select target prims)"
                )
            im = (ctypes.c_uint32 * len(index_map))(*[int(x) for x in index_map])
            keep.append(im)
            wd.index_map = ctypes.cast(im, ctypes.POINTER(ctypes.c_uint32))
            wd.count = resolved_count
        elif mask is not None:
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
            if count > len(mask) * 64:
                raise ValueError(
                    f"mask must hold at least {(count + 63) // 64} 64-bit word(s) to index "
                    f"{count} logical element(s); got {len(mask)}"
                )
            mk = (ctypes.c_uint64 * len(mask))(*[int(x) for x in mask])
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
        wd.semantic = int(semantic)
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
        index_map: Optional[Sequence[int]] = None,
        mask: Optional[Sequence[int]] = None,
        count: Optional[int] = None,
        cuda_event: Optional[int] = None,
        cuda_stream: Optional[int] = None,
    ) -> Operation:
        """Enqueue a copy-in write.

        ``is_array`` explicitly declares the logical attribute kind and is never
        inferred from tensor count, tensor shape, payload width, attribute name,
        semantic, or existing storage. ``tensors`` is a numpy array /
        :class:`DLTensor`, or a list thereof (one per source row for array
        attributes). Caller-owned tensors are kept alive on the returned
        :class:`Operation` until ``wait()``.

        Fixed-size writes (``is_array=False``) are normalized to the raw API's
        lane-canonical layout: reads and maps return ``ndim=1``, a leading
        dimension equal to the transported data-row count, and the complete
        per-row tuple width in ``dtype.lanes``. Logical prims select those rows
        directly or through the group's data index map. Compact convenience
        inputs such as a point array shaped ``(N, 3)`` or a matrix array shaped
        ``(N, 4, 4)`` are accepted, but their trailing shape is folded and not
        preserved. Without an index map, the leading dimension must equal the
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
        ``is_array=False`` and ``usd-schemas`` requires ``is_array=True``.
        Neither attribute is implicitly broadcast; use ``index_map`` when
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

        ``semantic`` is the :class:`AttributeSemantic` (or raw
        ``ovstage_attribute_semantic_t`` value) carried on the write. Geometric
        semantics (POINT/VECTOR/NORMAL/COLOR/QUATERNION/MATRIX/TEXTURE_COORDINATE)
        record a geometric role on the column; ID semantics select the
        corresponding ID storage type and require pre-interned id payloads
        (``TOKEN_ID`` / ``RELATIONSHIP_PATH_ID`` use ``dtype = (kDLUInt, 64, 1)``,
        ``CONNECTION_PATH_ID`` uses ``dtype = (kDLUInt, 64, 2)``). ``0`` (NONE)
        writes the payload without stamping a role / base type on the column.
        """
        # Keep the singular Python entry point aligned with the native API; the
        # native slot relays to the common plural implementation.
        wd, keep = self._build_write_data(
            tensors, is_array, index_map, mask, count, cuda_event, cuda_stream, semantic
        )
        sot = _b.make_string_or_token(attribute)
        keep.append(sot)
        res = self._api.ovstage_write_attribute(
            self._inst, _handle(query), sot, check_ordinal(ordinal), wd, int(prim_mode)
        )
        return Operation(self, res.status, res.op_index, keepalive=keep)

    def write_attributes(
        self,
        query,
        writes: Sequence[WriteDesc],
        ordinal: int,
        *,
        prim_mode: PrimMode = PrimMode.UPSERT,
    ) -> Operation:
        """Enqueue one copy-in operation for multiple attribute columns.

        Every write is validated before the operation is queued. The
        returned operation owns all ctypes/tensor keepalives until ``wait()``.
        Each fixed-size entry follows the same lane-canonical normalization as
        :meth:`write_attribute`.
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
                item.cuda_event, item.cuda_stream, item.semantic
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
        res = self._api.ovstage_write_attributes(
            self._inst, _handle(query), writes_ptr, len(items), check_ordinal(ordinal), int(prim_mode)
        )
        return Operation(self, res.status, res.op_index, keepalive=keep)

    def delete_attributes(self, query, attributes: Sequence[Union[int, str]], ordinal: int) -> Operation:
        """Enqueue a delete. Empty ``attributes`` deletes entire prims."""
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
    def clone(self, source_path: str, target_paths: Sequence[str], ordinal: int) -> None:
        """Clone the subtree under ``source_path`` to each path in ``target_paths`` (blocking).

        Data-plane peer of ovrtx's ``clone_usd`` (the ``_usd`` postfix is dropped).
        Like :meth:`write_attribute`, clone is ordinal-keyed: ``ordinal`` must be
        greater than the write floor or the op fails with a write-floor violation.
        The source must exist; each target must not already exist.
        """
        self.clone_async(source_path, target_paths, ordinal).wait()

    def clone_async(self, source_path: str, target_paths: Sequence[str], ordinal: int) -> Operation:
        """Enqueue a subtree clone of ``source_path`` to ``target_paths`` at ``ordinal`` (asynchronous)."""
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
        return Operation(self, res.status, res.op_index, keepalive=keep)

    # -- hierarchy ---------------------------------------------------------
    def get_hierarchy_computation_models(self) -> List[HierarchyComputationModelDesc]:
        """Return the hierarchy computation models supported by this backend."""
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
        """Compute hierarchy-derived data for ``input_ordinal`` (blocking)."""
        self.compute_hierarchy_async(input_ordinal, output_ordinal, model).wait()

    def compute_hierarchy_async(
        self,
        input_ordinal: int,
        output_ordinal: int,
        model: int = HierarchyComputationModel.DEFAULT_CPU,
    ) -> Operation:
        """Enqueue hierarchy-derived data computation for ``input_ordinal``."""
        fn = self._flat_symbol("ovstage_compute_hierarchy")
        res = fn(self._require_inst(), int(model), check_ordinal(input_ordinal), check_ordinal(output_ordinal))
        return Operation(self, res.status, res.op_index)

    def get_hierarchy(self, path_list: int, ordinal: int, relation: int) -> HierarchyResult:
        """Return parent/children/siblings for an ordered path list (blocking)."""
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
        res = fn(self._require_inst(), int(path_list), check_ordinal(ordinal), int(relation), ctypes.byref(handle))
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

    # -- map / unmap (zero-copy write) -------------------------------------
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
    ) -> "Map":
        """Reserve a zero-copy map session for an attribute.

        ``dtype`` is the element storage type used only when the column does not
        yet exist (the full per-element tuple width must be encoded in
        ``dtype.lanes``); it is ignored when the attribute already has a type.
        Fixed-size map groups expose the same lane-canonical layout as raw
        reads: ``ndim=1``, ``shape=(data_rows,)``, and the complete tuple width
        in ``dtype.lanes``. Use the group's ``data_row_index(local)`` to resolve
        a logical element through any data index map. Map groups do not
        reconstruct a convenience write shape such as ``(N, 4, 4)``.
        ``semantic`` is the :class:`AttributeSemantic` (or raw
        ``ovstage_attribute_semantic_t`` value) carried on the map. Geometric
        semantics record a geometric role on the column when the map creates it;
        ID semantics select the corresponding ID storage type and require
        pre-interned ids in the map buffer (``TOKEN_ID`` /
        ``RELATIONSHIP_PATH_ID`` use ``dtype = (kDLUInt, 64, 1)``,
        ``CONNECTION_PATH_ID`` uses ``dtype = (kDLUInt, 64, 2)``).
        ``element_sizes`` gives per-prim element counts for ragged columns.
        """
        keep: list = []
        sizes_ptr = None
        sizes_count = 0
        if element_sizes is not None:
            sizes = (ctypes.c_size_t * len(element_sizes))(*[int(s) for s in element_sizes])
            keep.append(sizes)
            sizes_ptr = ctypes.cast(sizes, ctypes.POINTER(ctypes.c_size_t))
            sizes_count = len(element_sizes)
        sot = _b.make_string_or_token(attribute)
        keep.append(sot)  # keeps sot._string_ref (the attribute string buffer) alive
        desc = _b.ovstage_map_desc_t()
        desc.attribute = sot
        if dtype is not None:
            desc.dtype = dtype
        desc.semantic = int(semantic)
        desc.prim_mode = int(prim_mode)
        keep.append(desc)
        handle = _b.ovstage_map_handle_t()
        res = self._api.ovstage_map_attribute(
            self._inst, _handle(query), ctypes.byref(desc), check_ordinal(ordinal),
            sizes_ptr, sizes_count, ctypes.byref(handle),
        )
        return Map(self, int(handle.value), Operation(self, res.status, res.op_index, keepalive=keep))

    def fetch_map_next(self, mapping, timeout: int = TIMEOUT_INFINITE) -> Optional[MapGroup]:
        grp = _b.ovstage_map_group_t()
        code = self._api.ovstage_fetch_map_next(self._inst, _handle(mapping), check_timeout(timeout), ctypes.byref(grp))
        if code == _b.OVSTAGE_ERROR_END_OF_ITERATION:
            return None
        if code != _b.OVSTAGE_OK:
            raise OvstageError(code, self._last_error())
        return MapGroup(grp)

    def unmap_group(
        self, mapping, group: MapGroup, cuda_event: Optional[int] = None, cuda_stream: Optional[int] = None
    ) -> Operation:
        sync = _b.ovstage_cuda_sync_t(cuda_stream or 0, cuda_event or 0)
        res = self._api.ovstage_unmap_group(self._inst, _handle(mapping), ctypes.byref(group.raw), sync)
        return Operation(self, res.status, res.op_index)

    def unmap_attribute(
        self, mapping, cuda_event: Optional[int] = None, cuda_stream: Optional[int] = None
    ) -> Operation:
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
        """
        desc = _b.ovstage_write_floor_desc_t()
        desc.ordinal = check_ordinal(ordinal)
        desc.scope = int(scope)
        keep: list = []
        if attributes:
            arr = (_b.ovx_token_t * len(attributes))(*[int(a) for a in attributes])
            keep.append(arr)
            desc.attributes = ctypes.cast(arr, ctypes.POINTER(_b.ovx_token_t))
            desc.attribute_count = len(attributes)
        res = self._api.ovstage_advance_write_floor(self._inst, ctypes.byref(desc))
        return Operation(self, res.status, res.op_index, keepalive=keep)

    def get_oldest_preserved_ordinal(self) -> "OrdinalQuery":
        handle = _b.ovstage_ordinal_query_handle_t()
        res = self._api.ovstage_get_oldest_preserved_ordinal(self._inst, ctypes.byref(handle))
        return OrdinalQuery(self, int(handle.value), Operation(self, res.status, res.op_index))

    def get_attribute_write_floor(self, attribute: Optional[Union[int, str]] = None) -> "OrdinalQuery":
        """Query a per-attribute write floor, or the global write floor if ``attribute`` is None."""
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
        """Return the instance's shared path-dictionary bundle (``{vtable, context}``).

        The dictionary is owned by ovstage and stays valid while at least one
        instance is alive; callers must not tear it down. Used by
        :class:`ovstage.PathDictionary`.
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
            q.wait()
            count = q.result().total_prim_count
        # release() runs on block exit

    If a handle is dropped without release, :meth:`__del__` issues a best-effort
    release and emits a :class:`ResourceWarning`; rely on the context manager (or
    an explicit ``release()``) rather than the finalizer. (:class:`Map` overrides
    this teardown with its own unmap-based lifecycle.)
    """

    def __init__(self, stage: Stage, handle: int, op: Optional[Operation]):
        self._stage = stage
        self.handle = int(handle)
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

    def wait(self, timeout: int = TIMEOUT_INFINITE) -> None:
        """Wait on the reserving op (no-op for synchronously-created handles)."""
        if self.op is not None:
            self.op.wait(timeout)

    def __int__(self) -> int:
        return self.handle

    def __enter__(self) -> "_HandleObject":
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
        warnings.warn(
            f"{type(self).__name__} (handle={self.handle}) was garbage-collected without "
            "release(); issuing a best-effort release to free the pinned handle. Use a "
            "'with' block or call release() explicitly.",
            ResourceWarning,
            stacklevel=1,
        )
        try:
            self.release()  # enqueue-only; the handle is reclaimed when the op runs
        except Exception:
            pass


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
    """A reserved zero-copy map (write) session.

    Reserve via :meth:`Stage.map_attribute`, iterate writable groups with
    :meth:`groups` / :meth:`fetch_next`, fill each group, and commit it with
    :meth:`unmap_group`. Finalize with :meth:`unmap`, which commits any
    remaining groups and releases the handle.

    An outstanding map pins session state (a reserved layout) and blocks
    overlapping writes, maps, and deletes on the same prims until it is
    unmapped. To guarantee release even when an error interrupts the fill loop,
    use the session as a context manager::

        with stage.map_attribute(query, attr, ordinal=o) as m:
            m.wait()
            for mg in m.groups():
                fill(mg)
                m.unmap_group(mg)

    Leaving the ``with`` block — normally or via an exception — calls
    :meth:`unmap`. The C API has no cancel: unmap always *commits* whatever the
    map buffers currently hold, so an exception mid-fill still persists the
    partially filled storage (there is no rollback). If a session is dropped
    without ever unmapping, :meth:`__del__` issues a best-effort unmap and emits
    a :class:`ResourceWarning`; rely on the context manager (or an explicit
    ``unmap()``) rather than the finalizer.
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

    def __enter__(self) -> "Map":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self._unmapped:
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
        warnings.warn(
            f"Map (handle={self.handle}) was garbage-collected without unmap(); "
            "issuing a best-effort unmap to release the pinned session. Use a "
            "'with' block or call unmap() explicitly.",
            ResourceWarning,
            stacklevel=1,
        )
        try:
            self.unmap()  # enqueue-only; the session releases when the op runs
        except Exception:
            pass


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
