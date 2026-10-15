# Population

Select USD properties and metadata, read their runtime values, and sample animation.
Requires a population-enabled ovstage build. The adjacent `scene.usda` supplies the data.

## Run

```sh
uv run main.py
```

See the parent examples README for package setup. The program deliberately tries a missing file
near the end, then retries with the valid scene.

## Selection and metadata

After populating physics at ordinal 1, select the Cube’s properties and metadata
at ordinal 2.

```python
# Select only /World/Body. is_under_path("/World") also includes its descendants.
body_condition = population.PrimPredicate.has_path("/World/Body")

# Request the values used below, plus metadata on those properties.
selector = population.Selector(
    prim_predicate=body_condition,
    property_predicate=population.PropertyPredicate.has_name(
        "size", "temperature", "state", "points", "emptyName", "emptyArray", "extent"
    ),
    prim_metadata_paths=["customData:label"],
    property_metadata_paths=["customData:unit"],
)

desc = population.Desc(
    domains=PopulationDomain.NONE,
    selectors=[selector],
    stage_metadata_paths=["timeCodesPerSecond"],
)
population.open_usd_with_desc(
    stage, scene, ordinal=2, time=population.DEFAULT_TIME, descs=[desc]
)
stage.advance_write_floor(ordinal=2).wait()
```

## Animation

Sample one second (USD time code 24) at ordinal 3 and read temperature 30.

```python
# One second is USD time code 24 in this scene. The ordinal is independent.
population.update_from_usd_time(stage, time=1.0, ordinal=3)
stage.advance_write_floor(ordinal=3).wait()
with paths.create_path_list_from_strings(["/World/Body"]) as selected:
    with stage.query_from_path_list(selected) as query:
        with stage.read_attributes(query, [paths.intern_token("temperature")], OrdinalRange.latest(3)) as read:
            read.wait()
            for group in read.groups():
                with group:
                    print(float(group.array(0).ravel()[0]))
                    # 30.0
```
