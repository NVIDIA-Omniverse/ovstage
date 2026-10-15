# USD export (python)

Export a typed USD subtree, mesh geometry, and runtime-created values. The example
shows destination lifetime, custom attribute rules, renaming, selection, transforms,
reports, changed properties, and asynchronous export. No USD Python binding is needed.

## Run

```sh
uv run main.py
```

Python outputs are temporary and removed on exit.

## Snippet context

The source contains `/World/Ball` with its watermark at ordinal 1. The final
example advances it to 2. See the full program for the shared setup.

## Save a transform overlay

```python
# A one-shot export creates, saves, and closes an overlay layer.
report = population.export_to_usd_file(
    stage, str(directory / "transforms.usda"), ordinal=1,
    attribute_rules=["omni:xform"],
)
print("Transforms exported:", report["transforms_exported"])
# Transforms exported: 1
```

## Accumulate exports, then save

```python
with population.ExportDestination.create(stage, str(directory / "combined.usda")) as usd:
    usd.export(ordinal=1, attribute_rules=["omni:xform"])
    usd.export(ordinal=1, attribute_rules=[{
        "source_attribute_name": "userProperties:temperature",
        "usd_type_name": "float",
    }])
    # Save both exports together. The destination remains open until block exit.
    usd.save()
```

## Export a changed temperature

```python
# custom.usda already holds the baseline temperature 20 at ordinal 1.
with paths.create_path_list_from_strings(["/World/Ball"]) as selected:
    with stage.query_from_path_list(selected) as query:
        stage.write_attribute(query, "userProperties:temperature", ordinal=2,
                              tensors=np.array([24.5], dtype=np.float32), is_array=False).wait()
stage.advance_write_floor(ordinal=2).wait()
changed = population.export_to_usd_file(
    stage, str(directory / "changed.usda"), ordinal=2, since_ordinal=1,
    selection=population.OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL,
    attribute_rules=[{
        "source_attribute_name": "userProperties:temperature",
        "usd_type_name": "float",
    }],
)
print("Changed attributes exported:", changed["attributes_exported"])
# Changed attributes exported: 1
```
