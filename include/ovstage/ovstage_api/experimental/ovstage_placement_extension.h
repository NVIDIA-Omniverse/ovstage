/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/**
 * @file ovstage_placement_extension.h
 * @brief Experimental device-placement extension ("ovstage.placement"):
 *        write, map and read on a specific device.
 *
 * @details
 * **Experimental.** Everything in this header -- ovstage_placement_t,
 * OVSTAGE_PLACEMENT_INIT, the extension table and its wrappers -- may change
 * between releases in source/API, ABI and semantics. Check availability at run
 * time: ovstage_query_placement_extension() and every wrapper below return
 * OVSTAGE_ERROR_NOT_FOUND when the implementation does not provide a table
 * this header can use. Once stable, placement stays an extension; it does not
 * move into ovstage_vtable_t.
 *
 * Versioning: the lookup accepts only `header.version` equal to
 * OVSTAGE_PLACEMENT_EXTENSION_VERSION, with `header.struct_size` at least
 * sizeof(ovstage_placement_extension_v1_t). An additive change -- entries
 * appended, nothing existing changed -- grows `struct_size` and keeps the
 * version. Any other change, including one to ovstage_placement_t, bumps the
 * version, so a caller built against an older header gets
 * OVSTAGE_ERROR_NOT_FOUND instead of a table it would misuse.
 *
 * Placement is reached through query_extension, not ovstage_vtable_t, and
 * `<ovstage/ovstage.h>` does not include this header. Include it explicitly:
 *
 *     #include <ovstage/ovstage_api/experimental/ovstage_placement_extension.h>
 */

#ifndef OVSTAGE_EXPERIMENTAL_PLACEMENT_EXTENSION_H
#define OVSTAGE_EXPERIMENTAL_PLACEMENT_EXTENSION_H

#include "../ovstage_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────────────────────────────────────────────────────────
 * Placement
 * ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Device placement — which device's copy of an attribute a write produces.
 *
 * Destination residency. This is the counterpart of ovstage_cuda_sync_t, which
 * describes the *source* handoff; the two are independent. A producer may hand
 * over a tensor on one device while asking that the write land on another.
 *
 * Placement selects a *mirror*; it does not pin a column to one device for its
 * lifetime. Stage storage keeps CPU and per-device copies and tracks which are
 * current, so a later write to the same attribute with a different placement is
 * an ordinary mirror update, not a conflict, and never an error.
 *
 * The guarantee placement buys is on the read side: after a write placed on
 * device D completes, a read of that attribute on device D is served from the
 * copy already current on D and performs no migration. It does not promise the
 * data is absent from other devices.
 *
 * One placement applies to an entire operation — it is a parameter of the slot,
 * not a per-attribute field, so a single operation cannot name two devices.
 * Placing two attributes on two devices takes two operations.
 *
 * Placement reaches the API through the "ovstage.placement" extension declared
 * below, rather than by widening the existing slots:
 * `write_attribute_on_device`, `write_attributes_on_device` and
 * `map_attribute_on_device` are the placed counterparts of `write_attribute`,
 * `write_attributes` and `map_attribute`. An unplaced call is byte-for-byte the
 * call it always was, and a NULL placement on a placed function reproduces it
 * exactly: the write follows the source tensor's device, the map stages on the
 * host.
 *
 * `read_attributes_on_device` takes a placement too, but it *selects* rather
 * than places: a read asks for data that already exists somewhere, so `device`
 * names which of an attribute's copies the returned groups point at. A column
 * already current there is pointed straight at; one current elsewhere is
 * materialized onto the requested device rather than the read failing. `stream`
 * carries that transfer, and `availability_event` is what makes it asynchronous.
 * `read_attributes` is unchanged, and a NULL placement on the placed read is
 * that call.
 *
 * The pointer does not have to outlive the call. Everything the operation needs
 * is copied out of the struct during the enqueue, so a caller may pass the
 * address of a local.
 *
 * Within a supplied struct, a zero-initialized `device` ({0, 0}) is also
 * "unspecified" — DLPack assigns no device type 0 (`kDLCPU == 1`), so that field
 * pattern is unambiguously "I did not ask".
 *
 * @remark A placement that agrees with the source tensor's device is honored
 *         directly. A **host payload placed on a CUDA device** is staged onto
 *         that device and scattered there, so the column becomes current on the
 *         GPU — this is the common case of producing data on the CPU for a GPU
 *         consumer. Staging currently costs a full device synchronization,
 *         because the scatter runs on the default stream and the staged buffer
 *         cannot be released until it drains; honoring `stream` removes that.
 *
 *         Every direction is staged, for fixed-size and ragged
 *         (`is_array == true`) payloads alike: host to CUDA, CUDA to a different
 *         CUDA device (peer), and CUDA back to the host. A ragged payload
 *         transports one tensor per row, so staging moves every row into one
 *         allocation and preserves the row structure. A strided host row is
 *         packed by its strides, so it lands with the values an unplaced write
 *         gives it; a row with a negative or zero (broadcast) stride is
 *         rejected with OVSTAGE_ERROR_NOT_SUPPORTED.
 *
 *         A staged write honors the payload's `ovstage_cuda_sync_t` before it
 *         first reads the payload, on the source tensor's device (`stream == 1`
 *         is that device's default stream).
 * @remark `stream` carries the op's device work for a fixed-size placed write:
 *         the scatter is launched on it. The write executes after the enqueue
 *         returns, so work enqueued on `stream` is ordered behind it only once
 *         `ovstage_wait_op` has reported the write complete -- a host-side
 *         handshake, not a wait for the GPU. See the safe sequences under
 *         `availability_event`.
 *
 *         One exception, and it is observable: a write that has to **stage**
 *         (its placement names a device other than the source's) drains that
 *         stream before releasing the staging buffer, because the buffer is
 *         scoped to the call. Such a write is therefore synchronous despite
 *         naming a stream. A placed write whose source already lives on the
 *         target device stages nothing and runs fully asynchronously.
 *
 *         `stream` is ignored for ragged (`is_array`) writes, whose device work
 *         stays on the default stream; their payload is still staged onto the
 *         placed device, only the scatter is not stream-ordered.
 *
 * @remark `availability_event` is how a caller learns that the memory the
 *         operation concerns is safe to dereference without the host waiting
 *         for the device work. It does not replace `ovstage_wait_op` (or a
 *         read's successful fetch), which is still the point after which the
 *         event has been recorded. It is a `cudaEvent_t` the **caller**
 *         creates and owns; ovstage records into it and never creates, destroys
 *         or hands back a CUDA object. That matches `stream` and
 *         `ovstage_cuda_sync_t`, which are also caller-owned.
 *
 *         It marks *availability* rather than this operation's completion, and
 *         the two are not the same instant -- on a placed read they are
 *         deliberately not the same instant (see below). Availability is also
 *         the one meaning that covers all three placed operations: a placed write
 *         makes its data available on the target device, a placed map's commit
 *         makes the values it committed available, and a placed read makes the
 *         column available.
 *
 *         It is the one outbound field here. `ovstage_cuda_sync_t` describes the
 *         *source* handoff and ovstage **waits** on it; `availability_event`
 *         describes the *destination* and ovstage **signals** it. A write may
 *         use both: wait for the producer, then signal this when the scatter is
 *         done.
 *
 *         **It is recorded exactly once for every accepted operation** (for a
 *         placed map, every commit; see the end of this remark), on `stream`,
 *         after the device work is launched -- and also when the operation
 *         turns out to need no device work at all, and when it fails.
 *         A caller can therefore always wait on it without risking a wait that
 *         never ends. The converse would be unusable: the caller cannot know
 *         which internal path an operation took. A call rejected synchronously
 *         -- an invalid `op_index` -- owes no record and leaves the event
 *         untouched, unless it was rejected for lack of memory.
 *
 *         **That makes the event fire mean "stop waiting", not "the data is
 *         good".** Because a failed operation records it too -- so that no
 *         caller waits forever on work that will never land -- the event on its
 *         own says nothing about whether the operation succeeded, and the name
 *         is not a promise that the memory holds valid data. The caller still
 *         checks the operation's status (`ovstage_wait_op`, or the fetch that
 *         hands back the payload). The event bounds the wait; the status
 *         qualifies what the memory is worth.
 *
 *         **Wait only after the submission boundary.** The record is issued
 *         when the operation executes, after the enqueue has returned; a wait
 *         issued earlier, on the event or on `stream`, observes an old or
 *         never-recorded event and orders nothing against this operation. The
 *         safe sequences, neither of which waits on the host for the GPU:
 *
 *         - Write, or map commit (unmap_group, unmap_attribute): enqueue;
 *           `ovstage_wait_op` returns OVSTAGE_OK for it; then wait on the
 *           event, or enqueue the downstream work on `stream`.
 *         - Read: enqueue; fetch_read_next hands back a group, which happens
 *           only after the record was issued; then make the consumer wait on
 *           the event (`cudaStreamWaitEvent`), or enqueue it on `stream`.
 *
 *         Event completion does not replace that status check: a failed
 *         operation records the event too. A default `stream` is the default
 *         stream of the device the record ran on; `stream == 2` is the
 *         per-thread stream of the thread that ran the operation, not
 *         necessarily the caller's, so there only the event orders the
 *         caller's work.
 *
 *         A record that CUDA refuses is reported, not swallowed: the event is
 *         left untouched and an otherwise successful operation reports
 *         OVSTAGE_ERROR_OP_FAILED with a diagnostic naming `availability_event`.
 *         That failure comes after the operation's effects and does not undo
 *         them: a write's entries stay committed at its ordinal, so under the
 *         STRICTLY_INCREASING admission policy a retry of them needs a
 *         later one, and a map commit's changes stay recorded (the session of
 *         an unmap_group stays open; an unmap_attribute still consumes its
 *         handle). A placed read fails instead of handing back its payload, and
 *         an operation that completes during the enqueue is rejected
 *         synchronously, after it ran. Nothing drains a write's device work on
 *         that path, and the untouched event does not cover it, so its
 *         client-managed source stays borrowed until, after `ovstage_wait_op`
 *         has returned, the caller has synchronized `stream` if it is a
 *         specific stream, or else the device the record was due to run on
 *         (for example `cudaDeviceSynchronize` with that device current;
 *         `stream == 2` names the per-thread stream of the thread that ran the
 *         write). A storage-managed source is still released once that work
 *         has finished.
 *
 *         It must be a live `cudaEvent_t`. ovstage probes the handle and
 *         rejects one CUDA recognizes as dead, but no API can validate an
 *         arbitrary integer as an event -- querying one dereferences it. Passing
 *         a value that was never an event is undefined behaviour, as it is for
 *         any handle a C API takes.
 *
 *         It must also belong to the device the record runs on. With a
 *         specific `stream` that is the stream's device. A default stream (0,
 *         1, or 2 for `cudaStreamPerThread`) is the default stream of `device`
 *         when that is a CUDA device; for a write whose `device` is unspecified
 *         and that has CUDA source tensors, of the device they live on;
 *         otherwise, a host `device` included, of the device current on the
 *         calling thread when the operation is enqueued (for a map, the
 *         map_attribute_on_device call). A write whose `device` is unspecified
 *         and that has CUDA sources on more than one device cannot be covered
 *         by one record on a default stream and is rejected with
 *         OVSTAGE_ERROR_INVALID_ARGUMENT.
 *
 *         **Lifetime differs from the rest of this struct.** The struct itself
 *         need not outlive the call, but the *event* must: it is recorded when
 *         the operation executes, which is after the enqueue returns, so
 *         destroying it before the record is issued is a use-after-free. The
 *         record has been issued, or refused, once `ovstage_wait_op` has
 *         returned OVSTAGE_OK or OVSTAGE_ERROR_OP_FAILED for the operation, or
 *         a read's fetch_read_next has handed back a group. A wait on the event
 *         or on `stream` before then establishes nothing.
 *
 *         Supplying one makes the operation genuinely asynchronous: ovstage
 *         otherwise drains the device work before reporting the op complete, so
 *         `ovstage_wait_op` alone implies the data has landed. With an
 *         availability event, `ovstage_wait_op` no longer implies that for the
 *         cases ovstage can leave running -- wait on the event. The exceptions
 *         are the ones listed above: a write that must stage drains its
 *         call-scoped buffer regardless, and a ragged write runs on the default
 *         stream, so for those the event is already signalled by the time the
 *         op completes. It is correct in every case; it only buys asynchrony in
 *         some.
 *
 *         For a write, the device work left running reads the source: a
 *         client-managed source (`ovstage_write_data_t::tensors`) stays
 *         borrowed until this event has completed, not merely until
 *         `ovstage_wait_op` returns, and a storage-managed one is released only
 *         once that work has finished. If the event could not be recorded, the
 *         borrow lasts until the synchronization described above instead.
 *
 *         **On a placed read the event is not that read's completion, and that
 *         is deliberate.** The read operation completes *first*: it hands back
 *         pointers, and the event marks the later moment those pointers became
 *         safe to dereference. It has to, because it covers more than the
 *         read's own work -- both any write ovstage has already run into the
 *         columns the read touches, including device work that write left
 *         running on whatever stream ovstage issued it on, and the read's own
 *         transfer, if one was needed. Some of what it waits for is therefore
 *         work the caller never issued, which is exactly why "when this
 *         operation finished" is the wrong way to read it. Once a group has been
 *         fetched, waiting on it is *sufficient*: a caller that then waits on
 *         the event, or enqueues its own work on `stream` behind it, may
 *         dereference the payload without knowing which route the read took or
 *         which stream any producing write used. ovstage orders the read behind
 *         those writes with events of its own; those are internal and are
 *         neither this event nor ever handed back.
 *
 *         The caveat above applies here with the most force: a read whose event
 *         has fired is a read that is done making the caller wait, not a read
 *         that returned a payload worth looking at. Check its status first.
 *
 *         A caller who supplies **no** availability event gets the same
 *         guarantee the other way round: ovstage does the waiting itself, so the
 *         operation does not report complete until both the earlier write and
 *         any transfer have landed, and the payload is ready the moment it is
 *         handed over.
 *
 *         The transform-hierarchy pass that ovstage runs for a read, query,
 *         seal or `ovstage_compute_hierarchy` first waits on the host for every
 *         write it still has in flight, so it derives transforms only from
 *         landed writes. A pass run outside ovstage on the same stage data, such
 *         as an attached renderer's own propagation, is not ordered behind those
 *         writes: wait on their events before starting one.
 *
 *         A placed map records it on each commit -- every unmap_group and
 *         unmap_attribute of the session -- once that commit has run. Its setup
 *         records it only if cancelled before it runs, so the event covers
 *         committed values, not the staging buffers fetch_map_next hands back.
 */
typedef struct ovstage_placement_t {
    DLDevice  device;              /**< Target mirror. `{0, 0}` = unspecified (follow the source tensor). */
    uintptr_t stream;              /**< 0/1 = legacy default stream, 2 = per-thread default stream, else a `cudaStream_t`. */
    uintptr_t availability_event;  /**< Caller-owned `cudaEvent_t` recorded when the memory is safe to use; 0 = none. */
} ovstage_placement_t;

/**
 * @brief Initializer for ovstage_placement_t: everything unspecified. Assign the
 *        fields you care about afterwards.
 *
 * `ovstage_placement_t placement = OVSTAGE_PLACEMENT_INIT;`
 */
#define OVSTAGE_PLACEMENT_INIT { { (DLDeviceType)0, 0 }, 0, 0 }

/* ───────────────────────────────────────────────────────────────────────────
 * Extension table
 * ─────────────────────────────────────────────────────────────────────────── */

/** Name to pass to query_extension for the device-placement extension. */
#define OVSTAGE_PLACEMENT_EXTENSION_NAME "ovstage.placement"
/** Version this header describes; the lookup accepts only this exact value. */
#define OVSTAGE_PLACEMENT_EXTENSION_VERSION 1u

/**
 * @brief Device-placement extension: placed write, map and read.
 *
 * Obtain it with ovstage_query_placement_extension(), below, or call
 * ovstage_write_attribute_on_device() and its siblings directly: they look
 * the table up and return OVSTAGE_ERROR_NOT_FOUND when the implementation does
 * not provide it. The table is owned by the implementation, immutable, and valid
 * for the lifetime of the process. Every function takes the implementation
 * context as its first argument, exactly like a vtable slot.
 */
typedef struct ovstage_placement_extension_v1_t
{
    ovstage_extension_header_t header;

    /* ───────────────────────────────────────────────────────────────────────
     * Write, map and read on a specific device
     *
     * Device-aware counterparts of write_attribute, write_attributes,
     * map_attribute and read_attributes. They live in this extension rather
     * than in ovstage_vtable_t, so the core vtable keeps exactly its released
     * layout and a caller can ask whether an implementation provides them
     * instead of assuming it does. A NULL placement makes each one exactly the
     * unplaced call it mirrors.
     *
     * read_attributes_on_device is a different kind of operation from the
     * other three: a write and a map *place* data, choosing where it should
     * land, while a read *selects* among copies that already exist and
     * materializes one only when the requested device has none. That is why
     * it is permissive where they are directive.
     * ─────────────────────────────────────────────────────────────────────── */

    /**
     * @brief Enqueue a write of attribute values, targeting a specific device.
     *
     * Identical to write_attribute except that `placement` selects which
     * device's copy of the attribute the write produces, and except for the
     * availability-event failure rule below. See ovstage_placement_t for the
     * placement contract and for the current implementation limits.
     *
     * If `placement->availability_event` cannot be recorded, the write still
     * applies and reports OVSTAGE_ERROR_OP_FAILED. It has consumed its ordinal,
     * so write_attribute's same-ordinal retry after a failure does not apply.
     *
     * @param instance Implementation context.
     * @param handle Query handle identifying target prims.
     * @param attribute Attribute to write.
     * @param ordinal Ordinal for this write (must be > write floor at execution time).
     * @param data Write payload (tensors + cuda_sync + mask/index_map).
     * @param prim_mode UPSERT (default) or INSERT.
     * @param placement Target device for this operation. NULL = unspecified
     *        (follow the source tensor's device), which makes this exactly
     *        write_attribute.
     * @return Enqueue result with status + op_index. Placement errors are
     *         provable during preflight and are returned synchronously with an
     *         invalid op_index.
     */
    ovstage_enqueue_result_t (*write_attribute_on_device)(
        ovstage_context_t*           instance,
        ovstage_query_handle_t       handle,
        ovx_string_or_token_t        attribute,
        ovstage_ordinal_t            ordinal,
        ovstage_write_data_t         data,
        ovstage_prim_mode_t          prim_mode,
        const ovstage_placement_t*   placement);

    /**
     * @brief Enqueue a multi-attribute write, targeting a specific device.
     *
     * Identical to write_attributes except that `placement` selects which
     * device's copy the operation produces, for every attribute in it. A single
     * operation cannot name two devices; use two operations.
     *
     * If `placement->availability_event` cannot be recorded, the write still
     * applies and reports OVSTAGE_ERROR_OP_FAILED. It has consumed its ordinal,
     * so write_attributes' same-ordinal retry after a failure does not apply.
     *
     * @param instance Implementation context.
     * @param handle Query handle identifying target prims.
     * @param writes Per-attribute write entries.
     * @param write_count Number of entries in `writes`.
     * @param ordinal Ordinal for this write (must be > write floor at execution time).
     * @param prim_mode UPSERT (default) or INSERT.
     * @param placement Target device for this operation. NULL = unspecified
     *        (follow the source tensor's device), which makes this exactly
     *        write_attributes.
     * @return Enqueue result with status + op_index.
     */
    ovstage_enqueue_result_t (*write_attributes_on_device)(
        ovstage_context_t*                 instance,
        ovstage_query_handle_t             handle,
        const ovstage_attribute_write_t*   writes,
        size_t                             write_count,
        ovstage_ordinal_t                  ordinal,
        ovstage_prim_mode_t                prim_mode,
        const ovstage_placement_t*         placement);

    /**
     * @brief Enqueue a map session whose staging buffers live on a device.
     *
     * Identical to map_attribute except that `placement` selects where the
     * writable staging buffers handed back by fetch_map_next are allocated, so
     * a CUDA kernel can fill them in place.
     *
     * @remark Staging is write-only and uninitialized; it does not contain the
     *         attribute's current values. Fill every selected element before
     *         committing a group. To preserve existing values, read them first
     *         and explicitly copy them into staging before applying edits.
     *         Release overlapping read groups before committing the map.
     *         CUDA staging is supported only for fixed-size attributes;
     *         array-valued CUDA maps return OVSTAGE_ERROR_NOT_SUPPORTED.
     *
     * @param instance Implementation context.
     * @param handle Query handle identifying target prims.
     * @param desc Map descriptor (attribute, dtype, semantic, prim_mode).
     * @param ordinal Ordinal for this map session.
     * @param element_sizes Per-prim element counts for array attributes, else NULL.
     * @param element_count Number of entries in `element_sizes`, else 0.
     * @param placement Device for the staging buffers. NULL = the host, which
     *        makes this exactly map_attribute.
     * @param out_map_handle Receives the reserved map handle.
     * @return Enqueue result with status + op_index.
     *
     * @remark A map's own device work -- the unmap scatter -- still runs on the
     *         mapped device's default stream, not on `placement->stream`. The
     *         stream is not inert, though: each commit -- unmap_group and
     *         unmap_attribute -- records `placement->availability_event` on it
     *         once that commit has run, so after `ovstage_wait_op` reports the
     *         commit complete a consumer can chain off that stream or the event.
     *         It covers the committed values, not the staging buffers
     *         fetch_map_next hands back. Because the scatter is host-drained
     *         before the mapping is released, recording there can only make the
     *         signal later than the work, never earlier.
     *
     *         Each unmap_group and unmap_attribute records it; one that cannot
     *         reports OVSTAGE_ERROR_OP_FAILED after its commit. An unmap_group
     *         then leaves the session open; an unmap_attribute still consumes
     *         the handle.
     */
    ovstage_enqueue_result_t (*map_attribute_on_device)(
        ovstage_context_t*           instance,
        ovstage_query_handle_t       handle,
        const ovstage_map_desc_t*    desc,
        ovstage_ordinal_t            ordinal,
        const size_t*                element_sizes,
        size_t                       element_count,
        const ovstage_placement_t*   placement,
        ovstage_map_handle_t*        out_map_handle);

    /**
     * @brief Enqueue a multi-attribute read served from a specific device.
     *
     * Identical to read_attributes except that `placement->device` selects which
     * of an attribute's copies the returned groups point at. The payload
     * `DLTensor`s report that device, so a CUDA read yields pointers a kernel can
     * consume without a host round-trip.
     *
     * Unlike the placed write and map functions, this one is **permissive**: when the
     * column is not current on the requested device it is made available there
     * rather than the read failing. That is the difference between placing and
     * selecting -- a write chooses where data should land, while a read asks for
     * data that already exists somewhere, and refusing it would leave the caller
     * no recourse but to read it elsewhere and copy it themselves, which is the
     * work this function is doing on their behalf.
     *
     * Zero-copy is preserved where it is available: if the column is already
     * current on `placement->device` the group points straight into that copy and
     * nothing is transferred. Only a column that is current elsewhere is
     * materialized onto the requested device, into storage owned by the group and
     * released with it by release_group.
     *
     * @param instance Implementation context.
     * @param handle Query handle identifying target prims.
     * @param attrs Attributes to read.
     * @param attr_count Number of entries in `attrs`.
     * @param range Ordinal range to read.
     * @param placement Device to serve the read from, the stream any transfer is
     *        issued on, and the availability event recorded when the payload
     *        becomes safe to dereference. NULL = the host, which makes this
     *        exactly read_attributes.
     * @param out_read_handle Receives the reserved read handle.
     * @return Enqueue result with status + op_index.
     *
     * @remark `placement->availability_event` is what makes a transferring read
     *         asynchronous, and the guarantee on it is that waiting on it is
     *         **sufficient**. fetch_read_next hands back a group only after the
     *         record was issued, so wait once a group is in hand; a wait issued
     *         earlier may observe an old or unrecorded event. Until it has been
     *         waited on -- or until the caller's own work is enqueued on
     *         `placement->stream` behind it -- the pointers in the group are
     *         **not** safe to dereference. Once it has, they are, because the
     *         event covers both halves of what the data depends on: any write
     *         ovstage has already run into the columns this read touches,
     *         including device work that write left running on whatever stream
     *         ovstage issued it on, and this read's own transfer, if one was
     *         needed. A caller therefore never has to
     *         know which route the read took, nor which stream a producing write
     *         used: a read served with no transfer at all records the event just
     *         the same, and ovstage orders the read behind those writes using
     *         events of its own, which are internal and never handed back.
     *
     *         **The read operation completes before the event fires, and that is
     *         deliberate.** The event is not this read's completion -- it is
     *         when the payload became available. It has to be, because half of
     *         what it waits for is a write the caller never issued. A read that
     *         reports complete has therefore said nothing about its own
     *         pointers; the event is what says they are safe.
     *
     *         **And the event firing means "stop waiting", not "the data is
     *         good".** It is recorded for every accepted operation including one
     *         that fails -- that rule is what keeps a caller from waiting
     *         forever -- so a fired event is not evidence the read succeeded.
     *         Check the operation's status before dereferencing what it handed
     *         back. A read whose event cannot be recorded fails instead of
     *         handing back its payload.
     *
     * @remark A caller who supplies **no** availability event gets the same
     *         guarantee without doing anything: ovstage does the waiting itself
     *         -- for the earlier write's device work and for this read's
     *         transfer -- before the operation reports complete, so the payload
     *         is ready as soon as fetch_read_next hands it over.
     *
     *         This is what read_attributes has always *promised*, but it is not
     *         what it previously did. An unplaced read takes the host copy of a
     *         column, and a write that left its scatter running had already
     *         marked the host copy stale, so the download that refreshed it was
     *         issued without ordering against the stream still writing. An
     *         unplaced read now waits for that write too, which costs it a host
     *         wait it did not previously pay -- and did not previously pay
     *         correctly.
     *
     * @remark The asynchrony is partial, and worth knowing before budgeting for
     *         it. Stage storage is pageable host memory, and a host-to-device
     *         copy out of pageable memory stages through an internal pinned
     *         buffer: that staging copy is paid *synchronously*, inside the
     *         call, and only the DMA that follows it overlaps. So an
     *         availability event removes the drain, not the copy. It also
     *         currently runs under the instance lock. A read already current on
     *         the requested device transfers nothing and has neither cost.
     */
    ovstage_enqueue_result_t (*read_attributes_on_device)(
        ovstage_context_t*           instance,
        ovstage_query_handle_t       handle,
        const ovx_token_t*           attrs,
        size_t                       attr_count,
        ovstage_ordinal_range_t      range,
        const ovstage_placement_t*   placement,
        ovstage_read_handle_t*       out_read_handle);
} ovstage_placement_extension_v1_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Wrappers
 * ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Look up the device-placement extension ("ovstage.placement").
 *
 * Returns OVSTAGE_OK and sets `*out_extension` when the implementation provides
 * the extension at exactly OVSTAGE_PLACEMENT_EXTENSION_VERSION, in a table at
 * least as large as ovstage_placement_extension_v1_t. Returns
 * OVSTAGE_ERROR_NOT_FOUND otherwise -- an implementation that predates the
 * extension or serves another version of it -- and
 * OVSTAGE_ERROR_INVALID_ARGUMENT for a NULL `out_extension`.
 *
 * This is the capability check. A caller that needs placement can fail cleanly
 * here instead of discovering its absence on the first placed call; the
 * ovstage_*_on_device wrappers below run the same check on every call.
 */
static inline ovstage_api_status_t ovstage_query_placement_extension(
    ovstage_instance_t*                       instance,
    const ovstage_placement_extension_v1_t**  out_extension)
{
    const void* table = NULL;
    const ovstage_placement_extension_v1_t* placement = NULL;
    ovstage_api_status_t status;
    if (!out_extension)
        return OVSTAGE_ERROR_INVALID_ARGUMENT;
    *out_extension = NULL;
    status = instance->vtable->query_extension(
        instance->context, OVSTAGE_PLACEMENT_EXTENSION_NAME, &table);
    if (status != OVSTAGE_OK)
        return status;
    placement = (const ovstage_placement_extension_v1_t*)table;
    /* Another version is incompatible by definition; a smaller table lacks
     * fields this header reads. */
    if (!placement || placement->header.version != OVSTAGE_PLACEMENT_EXTENSION_VERSION ||
        placement->header.struct_size < sizeof(ovstage_placement_extension_v1_t))
        return OVSTAGE_ERROR_NOT_FOUND;
    *out_extension = placement;
    return OVSTAGE_OK;
}

static inline ovstage_enqueue_result_t ovstage_write_attribute_on_device(
    ovstage_instance_t*           instance,
    ovstage_query_handle_t        handle,
    ovx_string_or_token_t         attribute,
    ovstage_ordinal_t             ordinal,
    ovstage_write_data_t          data,
    ovstage_prim_mode_t           prim_mode,
    const ovstage_placement_t*    placement)
{
    const ovstage_placement_extension_v1_t* ext = NULL;
    ovstage_enqueue_result_t rejected;
    rejected.status = ovstage_query_placement_extension(instance, &ext);
    rejected.op_index = OVSTAGE_INVALID_OP_ID;
    if (rejected.status != OVSTAGE_OK)
        return rejected;
    return ext->write_attribute_on_device(
        instance->context, handle, attribute, ordinal, data, prim_mode, placement);
}

static inline ovstage_enqueue_result_t ovstage_write_attributes_on_device(
    ovstage_instance_t*                 instance,
    ovstage_query_handle_t              handle,
    const ovstage_attribute_write_t*    writes,
    size_t                              write_count,
    ovstage_ordinal_t                   ordinal,
    ovstage_prim_mode_t                 prim_mode,
    const ovstage_placement_t*          placement)
{
    const ovstage_placement_extension_v1_t* ext = NULL;
    ovstage_enqueue_result_t rejected;
    rejected.status = ovstage_query_placement_extension(instance, &ext);
    rejected.op_index = OVSTAGE_INVALID_OP_ID;
    if (rejected.status != OVSTAGE_OK)
        return rejected;
    return ext->write_attributes_on_device(
        instance->context, handle, writes, write_count, ordinal, prim_mode, placement);
}

static inline ovstage_enqueue_result_t ovstage_read_attributes_on_device(
    ovstage_instance_t*           instance,
    ovstage_query_handle_t        handle,
    const ovx_token_t*            attrs,
    size_t                        attr_count,
    ovstage_ordinal_range_t       range,
    const ovstage_placement_t*    placement,
    ovstage_read_handle_t*        out_read_handle)
{
    const ovstage_placement_extension_v1_t* ext = NULL;
    ovstage_enqueue_result_t rejected;
    rejected.status = ovstage_query_placement_extension(instance, &ext);
    rejected.op_index = OVSTAGE_INVALID_OP_ID;
    if (rejected.status != OVSTAGE_OK)
    {
        if (out_read_handle)
            *out_read_handle = OVSTAGE_INVALID_READ_HANDLE;
        return rejected;
    }
    return ext->read_attributes_on_device(
        instance->context, handle, attrs, attr_count, range, placement, out_read_handle);
}

static inline ovstage_enqueue_result_t ovstage_map_attribute_on_device(
    ovstage_instance_t*           instance,
    ovstage_query_handle_t        handle,
    const ovstage_map_desc_t*     desc,
    ovstage_ordinal_t             ordinal,
    const size_t*                 element_sizes,
    size_t                        element_count,
    const ovstage_placement_t*    placement,
    ovstage_map_handle_t*         out_map_handle)
{
    const ovstage_placement_extension_v1_t* ext = NULL;
    ovstage_enqueue_result_t rejected;
    rejected.status = ovstage_query_placement_extension(instance, &ext);
    rejected.op_index = OVSTAGE_INVALID_OP_ID;
    if (rejected.status != OVSTAGE_OK)
    {
        if (out_map_handle)
            *out_map_handle = OVSTAGE_INVALID_MAP_HANDLE;
        return rejected;
    }
    return ext->map_attribute_on_device(
        instance->context, handle, desc, ordinal,
        element_sizes, element_count, placement, out_map_handle);
}

#ifdef __cplusplus
}
#endif

#endif /* OVSTAGE_EXPERIMENTAL_PLACEMENT_EXTENSION_H */
