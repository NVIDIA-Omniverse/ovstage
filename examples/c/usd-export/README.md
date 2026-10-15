# USD export (c)

Export a typed USD subtree, mesh geometry, and runtime-created values. The example
shows destination lifetime, custom attribute rules, renaming, selection, transforms,
reports, changed properties, and asynchronous export. No USD Python binding is needed.

## Run

```sh
cmake -S . -B build
cmake --build build --config Release
./build/usd-export /tmp/destination.usda
```

On multi-config generators, use `build/Release/usd-export`. The main output path
defaults to `destination.usda`; other files add suffixes to it.

## Snippet context

The source contains `/World/Ball` with its watermark at ordinal 1. The final
example advances it to 2. See the full program for the shared setup.

## Save a transform overlay

```cpp
ovstage_population_export_attribute_rule_t transformRule{};
transformRule.source_attribute_name = literal_to_ovx_string("omni:xform");
ovstage_population_export_desc_t transformDesc{};
check(stage, ovstage_population_export_desc_init_snapshot(&transformDesc, 1), "init export");
transformDesc.attribute_rules = &transformRule;
transformDesc.attribute_rule_count = 1;
const std::string transformFile = prefix + "transforms.usda";
ovstage_population_export_report_t report{};
// One-shot export creates, saves, and closes the overlay layer.
check(stage, ovstage_population_export_to_usd_file(stage,
    {transformFile.c_str(), transformFile.size()}, &transformDesc, &report), "export transforms");
std::printf("Transforms exported: %zu\n", report.transforms_exported);
// Transforms exported: 1
```

## Accumulate exports, then save

```cpp
const std::string combinedFile = prefix + "combined.usda";
ovstage_population_export_destination_handle_t usd{};
check(stage, ovstage_population_export_destination_create(stage,
    {combinedFile.c_str(), combinedFile.size()}, OVSTAGE_POPULATION_EXPORT_DESTINATION_EMPTY,
    &usd), "create destination");
check(stage, ovstage_population_export_to_destination(stage, usd, &transformDesc, nullptr), "export transform");
ovstage_population_export_attribute_rule_t temperatureRule{};
temperatureRule.source_attribute_name = literal_to_ovx_string("userProperties:temperature");
temperatureRule.usd_type_name = literal_to_ovx_string("float");
ovstage_population_export_desc_t temperatureDesc{};
check(stage, ovstage_population_export_desc_init_snapshot(&temperatureDesc, 1), "init export");
temperatureDesc.attribute_rules = &temperatureRule;
temperatureDesc.attribute_rule_count = 1;
check(stage, ovstage_population_export_to_destination(stage, usd, &temperatureDesc, nullptr), "export temperature");
// Persist both exports, then release the destination. Save itself leaves it open.
check(stage, ovstage_population_export_destination_save(stage, usd), "save");
check(stage, ovstage_population_export_destination_destroy(stage, usd), "close destination");
```

## Export a changed temperature

```cpp
// custom.usda already holds the baseline temperature 20 at ordinal 1.
// tensors[1] points to temperature; tokens[1] names userProperties:temperature.
// query still selects the single prim /World/Ball.
temperature = 24.5f;
waitOp(stage, ovstage_write_attribute(stage, query, {tokens[1], {}}, 2,
    writeData(&tensors[1]), OVSTAGE_PRIM_MODE_UPSERT), "update temperature");
watermark.ordinal = 2;
waitOp(stage, ovstage_advance_write_floor(stage, &watermark), "advance watermark");
auto changedDesc = temperatureDesc;
changedDesc.ordinal = 2;
changedDesc.since_ordinal = 1;
changedDesc.selection = OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL;
const std::string changedFile = prefix + "changed.usda";
ovstage_population_export_report_t changed{};
check(stage, ovstage_population_export_to_usd_file(stage,
    {changedFile.c_str(), changedFile.size()}, &changedDesc, &changed), "export changes");
std::printf("Changed attributes exported: %zu\n", changed.attributes_exported);
// Changed attributes exported: 1
```
