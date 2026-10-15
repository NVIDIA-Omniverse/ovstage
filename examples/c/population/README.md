# Population

Select USD properties and metadata, read their runtime values, and sample animation.
Requires a population-enabled ovstage build. The adjacent `scene.usda` supplies the data.

## Run

```sh
cmake -S . -B build
cmake --build build
cd build
./population
```

See the parent examples README for package setup. The C example uses the sibling
`common/` and `cmake/` directories. The program deliberately tries a missing file
near the end, then retries with the valid scene.

## Selection and metadata

After populating physics at ordinal 1, select the Cube’s properties and metadata
at ordinal 2.

```c
const ovx_string_t bodyPath[] = {literal_to_ovx_string("/World/Body")};
// Match this exact prim; is_under_path would select a subtree instead.
auto bodyCondition = ovstage_population_prim_predicate_has_path(bodyPath, 1);

const ovx_string_t primMetadata[] = {literal_to_ovx_string("customData:label")};
const ovx_string_t propertyMetadata[] = {literal_to_ovx_string("customData:unit")};
ovstage_population_selector_t selector{};
selector.prim_predicate = bodyCondition;
const ovx_string_t properties[] = {
    literal_to_ovx_string("size"), literal_to_ovx_string("temperature"), literal_to_ovx_string("state"),
    literal_to_ovx_string("points"), literal_to_ovx_string("emptyName"),
    literal_to_ovx_string("emptyArray"), literal_to_ovx_string("extent")};
selector.property_predicate = ovstage_population_property_predicate_has_name(properties, 7);
selector.prim_metadata_paths = primMetadata;
selector.prim_metadata_path_count = 1;
selector.property_metadata_paths = propertyMetadata;
selector.property_metadata_path_count = 1;

const ovx_string_t stageMetadata[] = {literal_to_ovx_string("timeCodesPerSecond")};
ovstage_population_desc_t desc{};
desc.domains = OVSTAGE_POPULATION_DOMAIN_NONE;
desc.selectors = &selector;
desc.selector_count = 1;
desc.stage_metadata_paths = stageMetadata;
desc.stage_metadata_path_count = 1;
// Population copies the description and the referenced arrays during submission.
waitPop(stage, ovstage_population_open_usd_from_file_with_desc(
    stage, scenePath, 2, OVSTAGE_POPULATION_DEFAULT_TIME, &desc, 1), "open with description");
watermark.ordinal = 2;
waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
```

## Animation

Sample one second (USD time code 24) at ordinal 3 and read temperature 30.

```c
// Seconds are converted using timeCodesPerSecond=24, independently of ordinal 3.
waitPop(stage, ovstage_population_apply_usd_time(stage, 3, 1.0), "sample one second");
watermark.ordinal = 3;
waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
{
    const ovx_string_t selected[] = {literal_to_ovx_string("/World/Body")};
    ovx_primpath_list_t pathList{};
    checkOvx(dict, path_dictionary_create_path_list_from_strings(dict, selected, 1, &pathList), "paths");
    ovstage_query_handle_t query{};
    check(stage, ovstage_query_from_path_list(stage, pathList, &query), "query");
    const ovx_string_t name = literal_to_ovx_string("temperature");
    ovx_token_t attribute{};
    checkOvx(dict, path_dictionary_create_tokens_from_strings(dict, &name, 1, &attribute), "attribute");
    ovstage_ordinal_range_t range{};
    range.end_ordinal = 3;
    ovstage_read_handle_t read{};
    waitOp(stage, ovstage_read_attributes(stage, query, &attribute, 1, range, &read), "read");
    ovstage_read_group_t group{};
    check(stage, ovstage_fetch_read_next(stage, read, OVSTAGE_TIMEOUT_INFINITE, &group), "fetch");
    const DLTensor& tensor = group.data.tensors[0];
    const auto* values = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(tensor.data) + tensor.byte_offset);
    std::printf("%.1f\n", static_cast<double>(values[0]));
    // 30.0
    // Release the borrowed data before releasing the read and query.
    ovstage_release_group(stage, &group);
    waitOp(stage, ovstage_release_read(stage, read), "release read");
    waitOp(stage, ovstage_release_query(stage, query), "release query");
    path_dictionary_release_path_list_reference(dict, pathList);
}
```
