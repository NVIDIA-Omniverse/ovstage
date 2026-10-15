// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Public ovstage change-feed test: what a per-tick consumer sees from
// ordinal-range reads on a whole-stage filter query it retains across ticks.
// A target-only tick (sealed SCOPE_ALL or SCOPE_INCLUDE) yields an empty
// structural feed, an INCLUDE seal moves only the written column's floor, and a
// retained query keeps its membership while a fresh one sees a new prim.
// CPU-only; no population bridge: the structural built-ins (usd-prim-type,
// usd-schemas) are written through the public write API, as a replication
// layer would. This file is the tested source of the empty-change-range-c,
// include-seal-change-range-c and frozen-vs-fresh-query-c snippets referenced
// by the application-flow skill. Keep the snippet markers intact.

#include <ovstage/ovstage.h>
#include <ovx/path_dictionary/path_dictionary.h>
#include <ovx/path_dictionary/path_dictionary_utils.h>
#include <dlpack/dlpack.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <tuple>
#include <vector>

namespace
{

ovx_string_t str(const char* s)
{
    return ovx_string_t{ s, std::strlen(s) };
}

// Drive an async enqueue to completion: check the enqueue status, wait on the
// op id, then report any per-op errors. Returns true iff the op completed
// error-free.
bool waitOp(ovstage_instance_t* stage, ovstage_enqueue_result_t enq, const char* what)
{
    if (enq.status != OVSTAGE_OK)
    {
        ADD_FAILURE() << what << " enqueue rejected (code " << enq.status << ")";
        return false;
    }
    ovstage_op_wait_result_t wait{};
    const ovstage_api_status_t werr = ovstage_wait_op(stage, enq.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
    for (size_t i = 0; i < wait.error_op_id_count; ++i)
    {
        const ovx_string_t e = ovstage_get_last_op_error(stage, wait.error_op_ids[i]);
        ADD_FAILURE() << what << " op failed: "
                      << std::string(e.ptr ? e.ptr : "", e.ptr ? e.length : 0);
    }
    ovstage_release_op(stage, enq.op_index);
    if (werr != OVSTAGE_OK)
        ADD_FAILURE() << what << " wait failed (code " << werr << ")";
    return werr == OVSTAGE_OK && wait.error_op_id_count == 0;
}

// One structural change row: (attribute, prim path, token value). Plain uint64
// handles, so rows sort and compare directly.
using TokenRow = std::array<uint64_t, 3>;
// One value row: (attribute, prim path, float value).
using FloatRow = std::tuple<ovx_token_t, ovx_primpath_t, float>;

const char* const kControlPaths[] = { "/World/Controls/Joint0", "/World/Controls/Joint1",
                                      "/World/Controls/Joint2" };
const char* const kBackgroundPaths[] = { "/World/Background/Cube0", "/World/Background/Cube1" };
const char* const kNewControlPaths[] = { "/World/Controls/Joint3" };

// Baseline tick 1 -- three typed control prims (Xform + PhysicsRigidBodyAPI)
// carrying two float columns, plus two unrelated typed prims (Cube +
// MaterialBindingAPI) in a second attribute layout -- sealed for every column,
// and the consumer's whole-stage query, created once and retained across ticks.
class ChangeFeedTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ovstage_instance_desc_t desc{};
        desc.name = "test.ovstage.change-feed";
        ASSERT_EQ(ovstage_create_instance(&desc, &stage_), OVSTAGE_OK);
        dict_ = ovstage_get_path_dictionary(stage_);
        ASSERT_NE(dict_, nullptr);

        target_ = intern("ctrl:target");
        stiffness_ = intern("ctrl:stiffness");
        primType_ = intern("usd-prim-type");
        schemas_ = intern("usd-schemas");
        xform_ = intern("Xform");
        cube_ = intern("Cube");
        rigidBody_ = intern("PhysicsRigidBodyAPI");
        materialBinding_ = intern("MaterialBindingAPI");

        controlQuery_ = pathQuery(kControlPaths, 3, &controlList_);
        backgroundQuery_ = pathQuery(kBackgroundPaths, 2, &backgroundList_);
        ASSERT_NE(controlQuery_, OVSTAGE_INVALID_QUERY_HANDLE);
        ASSERT_NE(backgroundQuery_, OVSTAGE_INVALID_QUERY_HANDLE);
        ASSERT_TRUE(internPaths(kControlPaths, 3, controlIds_));

        ASSERT_TRUE(writeTokens(controlQuery_, primType_, xform_, 3, /*isArray=*/false, 1));
        ASSERT_TRUE(writeTokens(controlQuery_, schemas_, rigidBody_, 3, /*isArray=*/true, 1));
        ASSERT_TRUE(writeFloats(controlQuery_, target_, { 0.0f, 0.0f, 0.0f }, 1));
        ASSERT_TRUE(writeFloats(controlQuery_, stiffness_, { 10.0f, 20.0f, 30.0f }, 1));
        ASSERT_TRUE(writeTokens(backgroundQuery_, primType_, cube_, 2, /*isArray=*/false, 1));
        ASSERT_TRUE(writeTokens(backgroundQuery_, schemas_, materialBinding_, 2, /*isArray=*/true, 1));
        ASSERT_TRUE(seal(1, OVSTAGE_SCOPE_ALL));

        wholeStage_ = wholeStageQuery();
        ASSERT_NE(wholeStage_, OVSTAGE_INVALID_QUERY_HANDLE);
        ASSERT_EQ(primCount(wholeStage_), 5u);
    }

    void TearDown() override
    {
        // Release every handle before destroying the instance, in reverse order
        // of acquisition: queries, then the path-list references they wrapped.
        if (stage_)
        {
            for (ovstage_query_handle_t query : { wholeStage_, backgroundQuery_, controlQuery_ })
            {
                if (query != OVSTAGE_INVALID_QUERY_HANDLE)
                    waitOp(stage_, ovstage_release_query(stage_, query), "release_query");
            }
        }
        if (dict_)
        {
            for (ovx_primpath_list_t list : { backgroundList_, controlList_ })
            {
                if (list != OVX_INVALID_PRIMPATH_LIST)
                    path_dictionary_release_path_list_reference(dict_, list);
            }
        }
        if (stage_)
            ovstage_destroy_instance(stage_);
    }

    ovx_token_t intern(const char* name)
    {
        ovx_token_t token = OVX_INVALID_TOKEN;
        const ovx_string_t view = str(name);
        if (path_dictionary_create_tokens_from_strings(dict_, &view, 1, &token).status != OVX_API_SUCCESS)
            ADD_FAILURE() << "intern token " << name;
        return token;
    }

    bool internPaths(const char* const* paths, size_t count, ovx_primpath_t* out)
    {
        std::vector<ovx_string_t> views;
        for (size_t i = 0; i < count; ++i)
            views.push_back(str(paths[i]));
        return path_dictionary_create_paths_from_strings(dict_, views.data(), count, out).status == OVX_API_SUCCESS;
    }

    // A path-list query over `paths`; the caller releases *outList after the query.
    ovstage_query_handle_t pathQuery(const char* const* paths, size_t count, ovx_primpath_list_t* outList)
    {
        std::vector<ovx_string_t> views;
        for (size_t i = 0; i < count; ++i)
            views.push_back(str(paths[i]));
        if (path_dictionary_create_path_list_from_strings(dict_, views.data(), count, outList).status != OVX_API_SUCCESS)
        {
            ADD_FAILURE() << "create path list";
            return OVSTAGE_INVALID_QUERY_HANDLE;
        }
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        if (ovstage_query_from_path_list(stage_, *outList, &query) != OVSTAGE_OK)
        {
            ADD_FAILURE() << "query_from_path_list";
            return OVSTAGE_INVALID_QUERY_HANDLE;
        }
        return query;
    }

    // Every prim on the stage: HAS(usd-path), the built-in every prim carries.
    ovstage_query_handle_t wholeStageQuery()
    {
        ovstage_predicate_t predicate{};
        predicate.attribute.string = str("usd-path");
        predicate.op = OVSTAGE_FILTER_OP_HAS;
        ovstage_filter_t filter{};
        filter.predicates = &predicate;
        filter.count = 1;
        ovstage_query_handle_t query = OVSTAGE_INVALID_QUERY_HANDLE;
        if (!waitOp(stage_, ovstage_query(stage_, &filter, nullptr, 0, &query), "query HAS(usd-path)"))
            return OVSTAGE_INVALID_QUERY_HANDLE;
        return query;
    }

    size_t primCount(ovstage_query_handle_t query)
    {
        ovstage_query_result_t result{};
        if (ovstage_fetch_query_result(stage_, query, OVSTAGE_TIMEOUT_INFINITE, &result) != OVSTAGE_OK)
        {
            ADD_FAILURE() << "fetch_query_result";
            return 0;
        }
        const size_t count = result.total_prim_count;
        ovstage_release_query_result(stage_, &result);
        return count;
    }

    static DLTensor tokenTensor(ovx_token_t* data, int64_t* shape)
    {
        DLTensor tensor{};
        tensor.data = data;
        tensor.device = { kDLCPU, 0 };
        tensor.ndim = 1;
        tensor.dtype = { kDLUInt, 64, 1 };
        tensor.shape = shape;
        return tensor;
    }

    // Write one token id to every prim of `query`: a fixed column (usd-prim-type,
    // one tensor with a row per prim) or a ragged column (usd-schemas, one
    // single-element tensor per prim).
    bool writeTokens(ovstage_query_handle_t query, ovx_token_t attribute, ovx_token_t value, size_t count,
                     bool isArray, ovstage_ordinal_t ordinal)
    {
        std::vector<ovx_token_t> values(count, value);
        std::vector<int64_t> shapes;
        std::vector<DLTensor> tensors;
        if (isArray)
        {
            shapes.assign(count, 1);
            for (size_t i = 0; i < count; ++i)
                tensors.push_back(tokenTensor(&values[i], &shapes[i]));
        }
        else
        {
            shapes.assign(1, static_cast<int64_t>(count));
            tensors.push_back(tokenTensor(values.data(), shapes.data()));
        }
        ovstage_write_data_t write{};
        write.tensors = tensors.data();
        write.tensor_count = static_cast<uint32_t>(tensors.size());
        write.is_array = isArray;
        return waitOp(stage_,
            ovstage_write_attribute(stage_, query, { attribute, {} }, ordinal, write, OVSTAGE_PRIM_MODE_UPSERT),
            "write tokens");
    }

    bool writeFloats(ovstage_query_handle_t query, ovx_token_t attribute, std::vector<float> values,
                     ovstage_ordinal_t ordinal)
    {
        int64_t shape[] = { static_cast<int64_t>(values.size()) };
        DLTensor tensor{};
        tensor.data = values.data();
        tensor.device = { kDLCPU, 0 };
        tensor.ndim = 1;
        tensor.dtype = { kDLFloat, 32, 1 };
        tensor.shape = shape;
        ovstage_write_data_t write{};
        write.tensors = &tensor;
        write.tensor_count = 1;
        write.is_array = false;
        return waitOp(stage_,
            ovstage_write_attribute(stage_, query, { attribute, {} }, ordinal, write, OVSTAGE_PRIM_MODE_UPSERT),
            "write floats");
    }

    bool seal(ovstage_ordinal_t ordinal, ovstage_scope_t scope, const std::vector<ovx_token_t>& attributes = {})
    {
        ovstage_write_floor_desc_t floor{};
        floor.ordinal = ordinal;
        floor.scope = scope;
        floor.attributes = attributes.empty() ? nullptr : attributes.data();
        floor.attribute_count = attributes.size();
        return waitOp(stage_, ovstage_advance_write_floor(stage_, &floor), "advance_write_floor");
    }

    // The sealed floor of one attribute, as the stage reports it.
    ovstage_ordinal_t attributeFloor(ovx_token_t attribute)
    {
        ovstage_ordinal_query_handle_t handle = OVSTAGE_INVALID_ORDINAL_QUERY_HANDLE;
        if (!waitOp(stage_, ovstage_get_attribute_write_floor(stage_, { attribute, {} }, &handle),
                    "get_attribute_write_floor"))
            return 0;
        ovstage_ordinal_t ordinal = 0;
        EXPECT_EQ(ovstage_fetch_ordinal(stage_, handle, OVSTAGE_TIMEOUT_INFINITE, &ordinal), OVSTAGE_OK);
        waitOp(stage_, ovstage_release_ordinal_query(stage_, handle), "release_ordinal_query");
        return ordinal;
    }

    // Resolve the prim paths a group covers: its list, then offset or index_map.
    std::vector<ovx_primpath_t> groupPaths(const ovstage_prim_group_t& prims)
    {
        std::vector<ovx_primpath_t> covered;
        size_t listCount = 0;
        if (path_dictionary_get_num_paths_from_path_list(dict_, prims.list, &listCount).status != OVX_API_SUCCESS)
        {
            ADD_FAILURE() << "get_num_paths_from_path_list";
            return covered;
        }
        std::vector<ovx_primpath_t> list(listCount);
        size_t fetched = 0;
        if (listCount != 0 &&
            path_dictionary_get_paths_from_path_list(dict_, prims.list, 0, listCount, list.data(), &fetched).status !=
                OVX_API_SUCCESS)
        {
            ADD_FAILURE() << "get_paths_from_path_list";
            return covered;
        }
        for (uint32_t i = 0; i < prims.count; ++i)
        {
            const size_t index = prims.index_map ? prims.index_map[i] : static_cast<size_t>(prims.offset) + i;
            if (index >= fetched)
            {
                ADD_FAILURE() << "prim index " << index << " outside the group's list (" << fetched << ")";
                break;
            }
            covered.push_back(list[index]);
        }
        return covered;
    }

    // Enqueue a range read over [first, last] and visit every group. Returns the
    // first non-OK status seen across enqueue, fetch and wait, so a rejected
    // read reports its typed status whether it was validated at enqueue or
    // deferred.
    template <typename Visit>
    ovstage_api_status_t readRange(ovstage_query_handle_t query, const std::vector<ovx_token_t>& attrs,
                                   ovstage_ordinal_t first, ovstage_ordinal_t last, Visit&& visit)
    {
        ovstage_ordinal_range_t range{};
        range.has_start_ordinal = true;
        range.start_ordinal = first;
        range.end_ordinal = last;
        ovstage_read_handle_t read = OVSTAGE_INVALID_READ_HANDLE;
        const ovstage_enqueue_result_t enqueue =
            ovstage_read_attributes(stage_, query, attrs.data(), attrs.size(), range, &read);
        if (enqueue.status != OVSTAGE_OK)
            return enqueue.status; // rejected at enqueue: no handle or op to release

        ovstage_api_status_t status = OVSTAGE_OK;
        for (;;)
        {
            ovstage_read_group_t group{};
            const ovstage_api_status_t fetch = ovstage_fetch_read_next(stage_, read, OVSTAGE_TIMEOUT_INFINITE, &group);
            if (fetch == OVSTAGE_ERROR_END_OF_ITERATION)
                break;
            if (fetch != OVSTAGE_OK)
            {
                status = fetch;
                break;
            }
            visit(group);
            ovstage_release_group(stage_, &group);
        }
        ovstage_op_wait_result_t wait{};
        const ovstage_api_status_t waited = ovstage_wait_op(stage_, enqueue.op_index, OVSTAGE_TIMEOUT_INFINITE, &wait);
        if (status == OVSTAGE_OK && waited != OVSTAGE_OK)
            status = waited;
        if (status == OVSTAGE_OK && wait.error_op_id_count != 0)
            status = OVSTAGE_ERROR_OP_FAILED;
        ovstage_release_op(stage_, enqueue.op_index);
        waitOp(stage_, ovstage_release_read(stage_, read), "release_read");
        return status;
    }

    // Structural change rows (attribute, prim path, token) over [first, last],
    // sorted. Array columns transport one tensor per covered prim; fixed columns
    // one tensor with a row per prim.
    ovstage_api_status_t structuralRows(ovstage_query_handle_t query, ovstage_ordinal_t first, ovstage_ordinal_t last,
                                        std::vector<TokenRow>* rows)
    {
        rows->clear();
        const std::vector<ovx_token_t> attrs{ schemas_, primType_ };
        const ovstage_api_status_t status = readRange(query, attrs, first, last, [&](const ovstage_read_group_t& group) {
            EXPECT_FALSE(group.is_delete);
            const std::vector<ovx_primpath_t> covered = groupPaths(group.prims);
            for (size_t i = 0; i < covered.size(); ++i)
            {
                const size_t dataRow = group.data.index_map ? group.data.index_map[i] : i;
                const size_t tensorIndex = group.is_array ? dataRow : 0;
                const size_t element = group.is_array ? 0 : dataRow;
                if (tensorIndex >= group.data.tensor_count || group.data.tensors[tensorIndex].data == nullptr ||
                    group.data.tensors[tensorIndex].shape == nullptr ||
                    element >= static_cast<size_t>(group.data.tensors[tensorIndex].shape[0]))
                {
                    ADD_FAILURE() << "structural group row " << i << " has no payload";
                    continue;
                }
                const auto* tokens = static_cast<const ovx_token_t*>(group.data.tensors[tensorIndex].data);
                rows->push_back(TokenRow{ group.attribute, covered[i], tokens[element] });
            }
        });
        std::sort(rows->begin(), rows->end());
        return status;
    }

    // Value rows (attribute, prim path, float) of fixed float columns over
    // [first, last], sorted.
    ovstage_api_status_t floatRows(ovstage_query_handle_t query, const std::vector<ovx_token_t>& attrs,
                                   ovstage_ordinal_t first, ovstage_ordinal_t last, std::vector<FloatRow>* rows)
    {
        rows->clear();
        const ovstage_api_status_t status = readRange(query, attrs, first, last, [&](const ovstage_read_group_t& group) {
            EXPECT_FALSE(group.is_delete);
            EXPECT_FALSE(group.is_array);
            if (group.data.tensor_count != 1 || group.data.tensors[0].data == nullptr ||
                group.data.tensors[0].shape == nullptr)
            {
                ADD_FAILURE() << "value group without a payload tensor";
                return;
            }
            const DLTensor& tensor = group.data.tensors[0];
            const auto* values = static_cast<const float*>(tensor.data);
            const std::vector<ovx_primpath_t> covered = groupPaths(group.prims);
            for (size_t i = 0; i < covered.size(); ++i)
            {
                const size_t dataRow = group.data.index_map ? group.data.index_map[i] : i;
                if (dataRow >= static_cast<size_t>(tensor.shape[0]))
                {
                    ADD_FAILURE() << "value row " << dataRow << " outside the tensor";
                    continue;
                }
                rows->push_back(FloatRow{ group.attribute, covered[i], values[dataRow] });
            }
        });
        std::sort(rows->begin(), rows->end());
        return status;
    }

    // The expected value rows of one column over the three control prims.
    std::vector<FloatRow> controlRows(ovx_token_t attribute, const std::array<float, 3>& values) const
    {
        std::vector<FloatRow> rows;
        for (size_t i = 0; i < 3; ++i)
            rows.push_back(FloatRow{ attribute, controlIds_[i], values[i] });
        std::sort(rows.begin(), rows.end());
        return rows;
    }

    // The structural rows a fourth control prim contributes when created.
    std::vector<TokenRow> newControlRows(ovx_primpath_t newId) const
    {
        std::vector<TokenRow> rows{ TokenRow{ primType_, newId, xform_ }, TokenRow{ schemas_, newId, rigidBody_ } };
        std::sort(rows.begin(), rows.end());
        return rows;
    }

    ovstage_instance_t* stage_ = nullptr;
    path_dictionary_instance_t* dict_ = nullptr;
    ovx_token_t target_ = OVX_INVALID_TOKEN;
    ovx_token_t stiffness_ = OVX_INVALID_TOKEN;
    ovx_token_t primType_ = OVX_INVALID_TOKEN;
    ovx_token_t schemas_ = OVX_INVALID_TOKEN;
    ovx_token_t xform_ = OVX_INVALID_TOKEN;
    ovx_token_t cube_ = OVX_INVALID_TOKEN;
    ovx_token_t rigidBody_ = OVX_INVALID_TOKEN;
    ovx_token_t materialBinding_ = OVX_INVALID_TOKEN;
    ovx_primpath_t controlIds_[3] = { OVX_INVALID_PRIMPATH, OVX_INVALID_PRIMPATH, OVX_INVALID_PRIMPATH };
    ovx_primpath_list_t controlList_ = OVX_INVALID_PRIMPATH_LIST;
    ovx_primpath_list_t backgroundList_ = OVX_INVALID_PRIMPATH_LIST;
    ovstage_query_handle_t controlQuery_ = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_query_handle_t backgroundQuery_ = OVSTAGE_INVALID_QUERY_HANDLE;
    ovstage_query_handle_t wholeStage_ = OVSTAGE_INVALID_QUERY_HANDLE;
};

// A target-only tick sealed for every column: the structural feed is empty and
// the value feed carries exactly the written column.
TEST_F(ChangeFeedTest, TargetOnlyTickSealedAllYieldsEmptyStructuralRange)
{
    ASSERT_TRUE(writeFloats(controlQuery_, target_, { 0.5f, 1.5f, 2.5f }, 2));
    ASSERT_TRUE(seal(2, OVSTAGE_SCOPE_ALL));

    // [snippet:empty-change-range-c]
    // Tick 2 wrote only `ctrl:target` on the control prims and sealed every
    // column with SCOPE_ALL. The structural feed over exactly that tick --
    // usd-schemas and usd-prim-type read over [2, 2] on the whole-stage
    // HAS(usd-path) query retained from earlier ticks -- is a successful read
    // with zero groups: nothing structural changed, and a range that selects
    // no change is not an error.
    std::vector<TokenRow> structural;
    EXPECT_EQ(structuralRows(wholeStage_, 2, 2, &structural), OVSTAGE_OK);
    EXPECT_TRUE(structural.empty());
    // [/snippet:empty-change-range-c]

    // The value feed for the same tick on the retained control query carries
    // exactly the written column: one row per control prim for ctrl:target and
    // none for ctrl:stiffness, which did not change at 2.
    std::vector<FloatRow> values;
    EXPECT_EQ(floatRows(controlQuery_, { target_, stiffness_ }, 2, 2, &values), OVSTAGE_OK);
    EXPECT_EQ(values, controlRows(target_, { 0.5f, 1.5f, 2.5f }));
}

// A target-only tick sealed with SCOPE_INCLUDE moves only the written column's
// floor; the value feed is readable and the structural feed stays empty.
TEST_F(ChangeFeedTest, IncludeSealOfTargetOnlyTickMovesOnlyThatFloor)
{
    ASSERT_TRUE(writeFloats(controlQuery_, target_, { 0.25f, 0.5f, 0.75f }, 2));

    // [snippet:include-seal-change-range-c]
    // A tick that wrote nothing structural may seal just the column it wrote.
    // SCOPE_INCLUDE{ctrl:target} moves that attribute's floor to 2 and leaves
    // every other column -- including usd-prim-type and usd-schemas -- at the
    // previous SCOPE_ALL seal. The value feed over [2, 2] carries the new rows;
    // the structural feed over the same tick is still a clean zero-group read.
    ASSERT_TRUE(seal(2, OVSTAGE_SCOPE_INCLUDE, { target_ }));
    EXPECT_EQ(attributeFloor(target_), 2u);
    EXPECT_EQ(attributeFloor(stiffness_), 1u);
    EXPECT_EQ(attributeFloor(primType_), 1u);
    EXPECT_EQ(attributeFloor(schemas_), 1u);

    std::vector<FloatRow> values;
    EXPECT_EQ(floatRows(controlQuery_, { target_ }, 2, 2, &values), OVSTAGE_OK);
    EXPECT_EQ(values, controlRows(target_, { 0.25f, 0.5f, 0.75f }));

    std::vector<TokenRow> structural;
    EXPECT_EQ(structuralRows(wholeStage_, 2, 2, &structural), OVSTAGE_OK);
    EXPECT_TRUE(structural.empty());
    // [/snippet:include-seal-change-range-c]
}

// A new prim enters a fresh query's feed but not a retained query's.
TEST_F(ChangeFeedTest, RetainedQueryKeepsMembershipWhileFreshQuerySeesNewPrim)
{
    ovx_primpath_list_t newList = OVX_INVALID_PRIMPATH_LIST;
    const ovstage_query_handle_t newQuery = pathQuery(kNewControlPaths, 1, &newList);
    ASSERT_NE(newQuery, OVSTAGE_INVALID_QUERY_HANDLE);
    ovx_primpath_t newId = OVX_INVALID_PRIMPATH;
    ASSERT_TRUE(internPaths(kNewControlPaths, 1, &newId));

    // Tick 2 creates a fourth control prim (type, schema, target) and seals
    // every column.
    ASSERT_TRUE(writeTokens(newQuery, primType_, xform_, 1, /*isArray=*/false, 2));
    ASSERT_TRUE(writeTokens(newQuery, schemas_, rigidBody_, 1, /*isArray=*/true, 2));
    ASSERT_TRUE(writeFloats(newQuery, target_, { 4.0f }, 2));
    ASSERT_TRUE(seal(2, OVSTAGE_SCOPE_ALL));

    // [snippet:frozen-vs-fresh-query-c]
    // A filter query keeps the membership it resolved when it ran. The query
    // retained from before tick 2 still covers five prims, and its structural
    // feed over [2, 2] is empty: the new prim is outside its membership. A
    // fresh HAS(usd-path) query resolves against the latest committed state --
    // six prims -- and the same range read reports the new prim's type and
    // schema rows.
    EXPECT_EQ(primCount(wholeStage_), 5u);
    std::vector<TokenRow> retainedRows;
    EXPECT_EQ(structuralRows(wholeStage_, 2, 2, &retainedRows), OVSTAGE_OK);
    EXPECT_TRUE(retainedRows.empty());

    const ovstage_query_handle_t fresh = wholeStageQuery();
    ASSERT_NE(fresh, OVSTAGE_INVALID_QUERY_HANDLE);
    EXPECT_EQ(primCount(fresh), 6u);
    std::vector<TokenRow> freshRows;
    EXPECT_EQ(structuralRows(fresh, 2, 2, &freshRows), OVSTAGE_OK);
    EXPECT_EQ(freshRows, newControlRows(newId));
    waitOp(stage_, ovstage_release_query(stage_, fresh), "release_query");
    // [/snippet:frozen-vs-fresh-query-c]

    waitOp(stage_, ovstage_release_query(stage_, newQuery), "release_query");
    path_dictionary_release_path_list_reference(dict_, newList);
}

// Sealing a tick that also wrote structure with SCOPE_INCLUDE leaves that
// change above its column's floor: a same-tick range read that selects it is a write-floor
// violation until a seal covering the structural columns lands.
TEST_F(ChangeFeedTest, IncludeSealLeavesStructuralChangesUnreadableUntilCovered)
{
    ovx_primpath_list_t newList = OVX_INVALID_PRIMPATH_LIST;
    const ovstage_query_handle_t newQuery = pathQuery(kNewControlPaths, 1, &newList);
    ASSERT_NE(newQuery, OVSTAGE_INVALID_QUERY_HANDLE);
    ovx_primpath_t newId = OVX_INVALID_PRIMPATH;
    ASSERT_TRUE(internPaths(kNewControlPaths, 1, &newId));

    // Tick 2 updates the targets and creates a prim, but seals only the targets.
    ASSERT_TRUE(writeFloats(controlQuery_, target_, { 1.0f, 2.0f, 3.0f }, 2));
    ASSERT_TRUE(writeTokens(newQuery, primType_, xform_, 1, /*isArray=*/false, 2));
    ASSERT_TRUE(writeTokens(newQuery, schemas_, rigidBody_, 1, /*isArray=*/true, 2));
    ASSERT_TRUE(seal(2, OVSTAGE_SCOPE_INCLUDE, { target_ }));

    // The value feed is readable: ctrl:target is sealed at 2.
    std::vector<FloatRow> values;
    EXPECT_EQ(floatRows(controlQuery_, { target_ }, 2, 2, &values), OVSTAGE_OK);
    EXPECT_EQ(values, controlRows(target_, { 1.0f, 2.0f, 3.0f }));

    // A fresh whole-stage query includes the new prim, whose type/schema change
    // at 2 sits above the structural floors (still 1): the structural feed is
    // rejected rather than served.
    const ovstage_query_handle_t fresh = wholeStageQuery();
    ASSERT_NE(fresh, OVSTAGE_INVALID_QUERY_HANDLE);
    EXPECT_EQ(primCount(fresh), 6u);
    std::vector<TokenRow> rows;
    EXPECT_EQ(structuralRows(fresh, 2, 2, &rows), OVSTAGE_ERROR_WRITE_FLOOR_VIOLATION);
    EXPECT_TRUE(rows.empty()); // documents that no partial rows are returned on rejection

    // A seal covering the structural columns makes the same read succeed.
    ASSERT_TRUE(seal(2, OVSTAGE_SCOPE_ALL));
    EXPECT_EQ(structuralRows(fresh, 2, 2, &rows), OVSTAGE_OK);
    EXPECT_EQ(rows, newControlRows(newId));

    waitOp(stage_, ovstage_release_query(stage_, fresh), "release_query");
    waitOp(stage_, ovstage_release_query(stage_, newQuery), "release_query");
    path_dictionary_release_path_list_reference(dict_, newList);
}

} // namespace
