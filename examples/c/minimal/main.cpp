// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
// property and proprietary rights in and to this material, related
// documentation and any modifications thereto. Any use, reproduction,
// disclosure or distribution of this material and related documentation
// without an express license agreement from NVIDIA CORPORATION or
// its affiliates is strictly prohibited.
//
// Minimal ovstage C example: create an instance, intern paths/tokens, write an
// attribute column, advance the write floor, read it back, and clone a subtree.
// Deliberately self-contained: the check/waitOp helpers other examples take
// from ../common/ovstage_example_utils.h are spelled out here because this file
// is the source of the snippets referenced by the ovstage skills -- keep the
// snippet markers intact.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

// [snippet:check-sync-error]
// Check a synchronous ovstage call: compare against OVSTAGE_OK and stringify
// failures with ovstage_get_error_string (vtable-dispatched, so it takes the
// instance). The examples fail fast -- print and exit; a real application
// would propagate the error instead.
static void check(ovstage_instance_t* stage, ovstage_api_status_t status, const char* what)
{
    if (status == OVSTAGE_OK)
        return;
    std::fprintf(stderr, "ovstage %s failed (code %u): %s\n", what, status,
                 stage ? ovstage_get_error_string(stage, status) : "(no instance)");
    std::exit(EXIT_FAILURE);
}
// [/snippet:check-sync-error]

// Check a path-dictionary call; the error string is dictionary-owned and must
// be released before exiting, or it leaks.
static void checkOvx(path_dictionary_instance_t* dict, ovx_api_result_t result, const char* what)
{
    if (result.status == OVX_API_SUCCESS)
        return;
    std::fprintf(stderr, "path dictionary %s failed (code %u)\n", what, (unsigned)result.status);
    if (dict && result.error.ptr)
        path_dictionary_release_error(dict, result.error);
    std::exit(EXIT_FAILURE);
}

// [snippet:enqueue-wait-error]
// Drive an async enqueue to completion. Enqueue success (OVSTAGE_OK) only
// means the op was accepted, so wait on the op id, report any per-op errors
// surfaced by the wait, and retire the op (ovstage_destroy_instance requires
// every op released first). The examples fail fast; a real application would
// propagate the errors instead.
static void waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq, const char* what)
{
    check(stage, enq.status, what);
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t code = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    for (size_t i = 0; i < wait.error_op_id_count; ++i)
    {
        const ovx_string_t e = ovstage_get_last_op_error(stage, wait.error_op_ids[i]);
        std::fprintf(stderr, "ovstage %s op %llu failed: %.*s\n", what,
                     (unsigned long long)wait.error_op_ids[i], (int)e.length, e.ptr ? e.ptr : "");
    }
    if (wait.error_op_id_count != 0)
        std::exit(EXIT_FAILURE);
    ovstage_release_op(stage, enq.op_index);
    check(stage, code, what);
}
// [/snippet:enqueue-wait-error]

int main()
{
    // ---- 1. instance + path dictionary: intern the attribute token ----
    ovstage_instance_desc_t desc{};
    desc.name = "example.minimal";
    ovstage_instance_t* stage = nullptr;
    ovstage_api_status_t status = ovstage_create_instance(&desc, &stage);
    check(nullptr, status, "create_instance");

    // [snippet:intern-and-resolve]
    // The path dictionary is owned by the instance (no app-side create/destroy):
    // obtain it, intern strings -> stable tokens, and resolve tokens back.
    path_dictionary_instance_t* dict = ovstage_get_path_dictionary(stage);
    if (!dict)
    {
        std::fprintf(stderr, "no path dictionary for instance\n");
        return EXIT_FAILURE;
    }

    ovx_string_t attrName = literal_to_ovx_string("temperature");
    ovx_token_t attr = OVX_INVALID_TOKEN;
    ovx_api_result_t ovxResult = path_dictionary_create_tokens_from_strings(dict, &attrName, 1, &attr);
    checkOvx(dict, ovxResult, "intern-token");

    ovx_string_t resolved{};
    ovxResult = path_dictionary_get_strings_from_tokens(dict, &attr, 1, &resolved);
    checkOvx(dict, ovxResult, "resolve-token");
    // [/snippet:intern-and-resolve]

    // [snippet:string-view-from-ovx-string]
    // ovx_string_t is a non-owning (ptr, length) view; wrap it in string_view
    // for zero-copy use. Copy it if it must outlive the dictionary.
    std::string_view name{ resolved.ptr, resolved.length };
    std::printf("attribute token %llu = '%.*s'\n", (unsigned long long)attr, (int)name.size(), name.data());
    // [/snippet:string-view-from-ovx-string]

    // ---- 2. path list + query over three prims ----
    // [snippet:path-list-query]
    // Build an immutable prim-path list from strings and open a query over it.
    const ovx_string_t paths[] = { literal_to_ovx_string("/World/A"), literal_to_ovx_string("/World/B"),
                                   literal_to_ovx_string("/World/C") };
    ovx_primpath_list_t pathList = OVX_INVALID_PRIMPATH_LIST;
    ovxResult = path_dictionary_create_path_list_from_strings(dict, paths, 3, &pathList);
    checkOvx(dict, ovxResult, "create-path-list");

    ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
    status = ovstage_query_from_path_list(stage, pathList, &query);
    check(stage, status, "query_from_path_list");
    // [/snippet:path-list-query]

    // [snippet:string-or-token-arg]
    // Attributes pass as ovx_string_or_token_t. We already hold an interned
    // token, so set it (token != 0) and leave the string empty to skip a lookup.
    ovx_string_or_token_t attrArg{ attr, {} };
    // [/snippet:string-or-token-arg]

    // ---- 3. write one float per prim, seal ordinal 1, read it back ----
    // [snippet:minimal-write-read]
    // Write one float per prim into the "temperature" column (UPSERT creates
    // the prims on first write), seal it by advancing the write floor to
    // ordinal 1, then read the column back.
    float values[] = { 1.0f, 2.0f, 3.0f };
    int64_t shape[] = { 3 };
    int64_t strides[] = { 1 };
    DLTensor tensor{};
    tensor.data = values;
    tensor.device = { kDLCPU, 0 };
    tensor.ndim = 1;
    tensor.dtype = { kDLFloat, 32, 1 }; // {code, bits, lanes}
    tensor.shape = shape;
    tensor.strides = strides;

    ovstage_write_data_t write{};
    write.tensors = &tensor;
    write.tensor_count = 1;
    write.is_array = false;

    ovstage_enqueue_result_t enq =
        ovstage_write_attribute(stage, query, attrArg, /*ordinal*/ 1, write, OVSTAGE_PRIM_MODE_UPSERT);
    waitOp(stage, enq, "write_attribute");

    ovstage_write_floor_desc_t writeFloor{};
    writeFloor.ordinal = 1;
    writeFloor.scope = OVSTAGE_SCOPE_ALL;
    enq = ovstage_advance_write_floor(stage, &writeFloor);
    waitOp(stage, enq, "advance_write_floor");

    ovstage_ordinal_range_t range{};
    range.end_ordinal = 1;
    range.has_start_ordinal = false;
    ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
    enq = ovstage_read_attributes(stage, query, &attr, 1, range, &read);
    waitOp(stage, enq, "read_attributes");

    ovstage_read_group_t group{};
    status = ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group);
    check(stage, status, "fetch_read_next");
    if (group.data.tensor_count != 1 || !group.data.tensors[0].data)
    {
        std::fprintf(stderr, "unexpected read layout\n");
        return EXIT_FAILURE;
    }
    const float* out = static_cast<const float*>(group.data.tensors[0].data);
    std::printf("read back ordinal %llu: %.1f %.1f %.1f\n", (unsigned long long)group.ordinal, out[0], out[1], out[2]);
    ovstage_release_group(stage, &group); // the tensor data is only valid until the group is released
    // [/snippet:minimal-write-read]

    // ---- 4. clone the subtree under one prim to two new targets ----
    // [snippet:clone-subtree-multienv]
    // One call clones the subtree under a source prim to several new targets --
    // the multi-environment pattern (N copies of a prototype, e.g. one scene
    // per RL environment). Clone is an ordinal-keyed write: pick an ordinal
    // above the write floor (ordinal 1 was sealed above, so clone at 2). The
    // source must exist; each target must not already exist.
    const ovx_string_t cloneTargets[] = { literal_to_ovx_string("/World/A_env0"),
                                          literal_to_ovx_string("/World/A_env1") };
    enq = ovstage_clone(stage, literal_to_ovx_string("/World/A"), cloneTargets, 2, /*ordinal*/ 2);
    waitOp(stage, enq, "clone");
    writeFloor.ordinal = 2; // seal the clones so they're readable
    enq = ovstage_advance_write_floor(stage, &writeFloor);
    waitOp(stage, enq, "advance_write_floor");
    std::printf("cloned /World/A -> A_env0, A_env1\n");
    // [/snippet:clone-subtree-multienv]

    // Release every handle, then destroy: ovstage_destroy_instance requires
    // all ops and handles released first.
    enq = ovstage_release_read(stage, read);
    waitOp(stage, enq, "release_read");
    enq = ovstage_release_query(stage, query);
    waitOp(stage, enq, "release_query");
    path_dictionary_release_path_list_reference(dict, pathList);
    ovstage_destroy_instance(stage);
    return 0;
}
