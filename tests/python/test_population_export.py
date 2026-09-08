# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.

import ctypes
import gc
import math
import warnings

import numpy as np
import pytest

from ovstage import (AttributeSemantic, Filter, FilterOp, PathDictionary,
                     PopulationDomain, Predicate, Stage, population)
from ovstage._src import population_export as _export
from ovstage._src import population_export_bindings as _bindings

pytestmark = pytest.mark.skipif(
    not population.export_available(),
    reason="libovstage was built without the population export-to-USD bridge",
)


class _Stage:
    _inst = object()


def _descriptor(**overrides):
    options = {
        "ordinal": 7,
        "since_ordinal": 0,
        "selection": _export.OVSTAGE_POPULATION_EXPORT_SELECTION_EXPLICIT_PREDICATE,
        "layer_mode": _export.OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF,
        "transform_policy": _export.OVSTAGE_POPULATION_EXPORT_TRANSFORM_USD_XFORM_OP,
        "unknown_metadata_policy": _export.OVSTAGE_POPULATION_EXPORT_UNKNOWN_METADATA_DROP,
        "source_api_schema_policy": _export.SourceApiSchemaPolicy.APPLY_RECORDED,
        "projection": _export.ExportProjection.SCHEMA_DECLARED,
        "prim_predicate": None,
        "property_predicate": None,
        "prim_rules": None,
        "api_schema_rules": None,
        "attribute_rules": None,
        "property_rules": None,
        "metadata_rules": None,
    }
    options.update(overrides)
    return _export._build_export_desc(_bindings.load(), **options)


def _translation_matrices(translations) -> np.ndarray:
    matrices = np.repeat(np.eye(4, dtype=np.float64)[None, :, :], len(translations), axis=0)
    matrices[:, 3, :3] = np.asarray(translations, dtype=np.float64)
    return matrices


def _write_local_transforms(stage, prim_paths, translations, ordinal: int) -> None:
    with PathDictionary(stage) as paths:
        path_list = paths.create_path_list_from_strings(prim_paths)
        try:
            with stage.query_from_path_list(path_list) as query:
                stage.write_attribute(
                    query,
                    "omni:xform",
                    ordinal=ordinal,
                    tensors=_translation_matrices(translations),
                    is_array=False,
                    semantic=AttributeSemantic.MATRIX,
                ).wait()
        finally:
            paths.destroy_path_list(path_list)
    stage.advance_write_floor(ordinal=ordinal).wait()


def _write_float_columns(stage, prim_paths, columns, ordinal: int) -> None:
    with PathDictionary(stage) as paths:
        path_list = paths.create_path_list_from_strings(prim_paths)
        try:
            with stage.query_from_path_list(path_list) as query:
                for name, values in columns.items():
                    stage.write_attribute(
                        query,
                        name,
                        ordinal=ordinal,
                        tensors=np.asarray(values),
                        is_array=False,
                    ).wait()
        finally:
            paths.destroy_path_list(path_list)
    stage.advance_write_floor(ordinal=ordinal).wait()


def _attribute_names(stage, prim_path: str) -> set[str]:
    with PathDictionary(stage) as paths:
        with stage.query(filter=Filter([Predicate("usd-path", FilterOp.IN, [prim_path])])) as query:
            query.wait()
            return {paths.token_to_string(attribute) for attribute in query.result().attributes}


def _filtered_prim_count(stage, predicates) -> int:
    with stage.query(filter=Filter(predicates)) as query:
        query.wait()
        return query.result().total_prim_count


def test_export_local_transforms_with_defaults(stage, tmp_path):
    destination = tmp_path / "transforms.usda"
    # [snippet:export-runtime-to-usd]
    prim_paths = ["/World/Red", "/World/Green", "/World/Blue"]
    translations = [(0.0, 0.0, 0.0), (2.0, 0.0, 0.0), (4.0, 0.0, 0.0)]

    with PathDictionary(stage) as paths:
        path_list = paths.create_path_list_from_strings(prim_paths)
        try:
            with stage.query_from_path_list(path_list) as query:
                stage.write_attribute(
                    query,
                    "omni:xform",
                    ordinal=1,
                    tensors=_translation_matrices(translations),
                    is_array=False,
                    semantic=AttributeSemantic.MATRIX,
                ).wait()
        finally:
            paths.destroy_path_list(path_list)
    stage.advance_write_floor(ordinal=1).wait()

    # The one-shot API creates an empty destination, exports, saves, and closes.
    report = population.export_to_usd_file(
        stage,
        str(destination),
        ordinal=1,
        attribute_rules=["omni:xform"],
    )
    # [/snippet:export-runtime-to-usd]

    assert destination.exists()
    assert report["prims_exported"] == 3
    assert report["attributes_exported"] == 3
    assert report["transforms_exported"] == 3
    assert 'over "Red"' in destination.read_text(encoding="utf-8")


def test_reusable_destination_accumulates_then_saves(stage, tmp_path):
    _write_float_columns(
        stage,
        ["/World/Red", "/World/Blue"],
        {"temperature": np.asarray([21.5, 22.0], dtype=np.float32)},
        ordinal=1,
    )
    destination = tmp_path / "accumulated.usda"

    # [snippet:export-reusable-destination]
    with population.ExportDestination.create(stage, str(destination)) as usd:
        usd.export(
            ordinal=1,
            prim_predicate=population.PrimPredicate.is_under_path("/World/Red"),
            attribute_rules=[{"source_attribute_name": "temperature", "usd_type_name": "float"}],
        )
        usd.export(
            ordinal=1,
            prim_predicate=population.PrimPredicate.is_under_path("/World/Blue"),
            attribute_rules=[{"source_attribute_name": "temperature", "usd_type_name": "float"}],
        )
        usd.save()
    # [/snippet:export-reusable-destination]

    text = destination.read_text(encoding="utf-8")
    assert 'over "Red"' in text
    assert 'over "Blue"' in text


def test_export_destination_finalizer_releases_identifier_without_saving(stage, tmp_path):
    destination = tmp_path / "finalizer-release.usda"
    with pytest.warns(ResourceWarning, match="ExportDestination was garbage-collected"):
        abandoned = population.ExportDestination.create(stage, str(destination))
        del abandoned
        gc.collect()

    # The best-effort destroy must release process-wide canonical ownership, but
    # must not persist the abandoned destination's empty layer.
    assert not destination.exists()
    replacement = population.ExportDestination.create(stage, str(destination))
    replacement.close()


def test_export_destination_explicit_close_emits_no_resource_warning(stage, tmp_path):
    destination = tmp_path / "explicit-close.usda"
    with warnings.catch_warnings(record=True) as captured:
        warnings.simplefilter("always", ResourceWarning)
        export_destination = population.ExportDestination.create(stage, str(destination))
        export_destination.close()
        del export_destination
        gc.collect()
    assert not [warning for warning in captured if issubclass(warning.category, ResourceWarning)]


def test_export_custom_attributes_preserving_names(stage, tmp_path):
    prim_paths = ["/World/SensorA", "/World/SensorB"]
    _write_float_columns(
        stage,
        prim_paths,
        {
            "userProperties:temperature": np.array([21.5, 22.0], dtype=np.float32),
            "userProperties:confidence": np.array([0.9, 0.8], dtype=np.float32),
        },
        ordinal=1,
    )
    destination = tmp_path / "custom-properties.usda"
    # [snippet:export-custom-attributes-to-usd]
    report = population.export_to_usd_file(
        stage,
        str(destination),
        ordinal=1,
        attribute_rules=[
            {
                "source_attribute_name": "userProperties:*",
                "source_attribute_name_match": population.OVSTAGE_POPULATION_EXPORT_NAME_MATCH_GLOB,
                "flags": (
                    population.OVSTAGE_POPULATION_EXPORT_RULE_INFER_CUSTOM_TYPE
                    | population.OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED
                ),
            }
        ],
    )
    # [/snippet:export-custom-attributes-to-usd]

    assert report["prims_exported"] == 4
    assert report["attributes_exported"] == 4
    assert report["custom_attributes_inferred"] == 2
    text = destination.read_text(encoding="utf-8")
    assert "float userProperties:temperature" in text
    assert "float userProperties:confidence" in text


def test_export_runtime_attribute_to_usd_schema_name(stage, tmp_path):
    with PathDictionary(stage) as paths:
        path_list = paths.create_path_list_from_strings(["/World/Ball"])
        try:
            with stage.query_from_path_list(path_list) as query:
                stage.write_attribute(
                    query,
                    "solver:collisionRadius",
                    ordinal=1,
                    tensors=np.array([2.5], dtype=np.float64),
                    is_array=False,
                ).wait()
                # Runtime-created prims can declare their source type through
                # this public built-in column. DEF export reads it when no prim
                # rule overrides the destination type.
                stage.write_attribute(
                    query,
                    "usd-prim-type",
                    ordinal=1,
                    tensors=np.array([paths.intern_token("Sphere")], dtype=np.uint64),
                    is_array=False,
                ).wait()
        finally:
            paths.destroy_path_list(path_list)
    stage.advance_write_floor(ordinal=1).wait()

    destination = tmp_path / "schema-mapping.usda"
    # [snippet:export-schema-attribute-to-usd]
    report = population.export_to_usd_file(
        stage,
        str(destination),
        ordinal=1,
        layer_mode=population.OVSTAGE_POPULATION_EXPORT_LAYER_MODE_DEF,
        attribute_rules=[
            {
                "source_attribute_name": "solver:collisionRadius",
                "destination_attribute_name": "radius",
                "usd_type_name": "double",
                "flags": population.OVSTAGE_POPULATION_EXPORT_RULE_REQUIRED,
            }
        ],
    )
    # [/snippet:export-schema-attribute-to-usd]

    assert report["attributes_exported"] == 1
    text = destination.read_text(encoding="utf-8")
    assert 'def Sphere "Ball"' in text
    assert "double radius = 2.5" in text
    assert "solver:collisionRadius" not in text


def test_export_homogeneous_mesh_subtree(stage, tmp_path):
    source = """#usda 1.0
def Xform "World"
{
    def Xform "SimulatedMeshes"
    {
        def Mesh "Floor" (prepend apiSchemas = ["PhysicsCollisionAPI"])
        {
            point3f[] points = [(0, 0, 0), (2, 0, 0), (0, 2, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            bool physics:collisionEnabled = true
        }
        def Mesh "Dynamic" (
            prepend apiSchemas = ["PhysicsCollisionAPI", "PhysicsRigidBodyAPI"]
        )
        {
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            bool physics:collisionEnabled = true
            bool physics:rigidBodyEnabled = true
        }
    }
    def Sphere "OutsideSelection"
    {
        double radius = 4
    }
}
"""
    population.open_usd_from_string(
        stage, source, ordinal=1, time_code=math.nan, domains=PopulationDomain.ALL
    )
    stage.advance_write_floor(1).wait()

    destination = tmp_path / "mesh-subtree.usda"
    # [snippet:export-homogeneous-mesh-subtree]
    report = population.export_typed_hierarchy_to_usd_file(
        stage,
        str(destination),
        "/World/SimulatedMeshes",
        ordinal=1,
    )
    # [/snippet:export-homogeneous-mesh-subtree]

    assert report["api_schemas_applied"] == 3
    assert report["attributes_exported"] >= 8

    with Stage("test.ovstage.population-export.mesh-result") as destination_owner:
        population.open_usd(
            destination_owner,
            str(destination),
            ordinal=1,
            time_code=math.nan,
            domains=PopulationDomain.ALL,
        )
        destination_owner.advance_write_floor(1).wait()
        root = "/World/SimulatedMeshes"
        exported_meshes = [
            Predicate("usd-path", FilterOp.PREFIX, [root + "/"]),
            Predicate("usd-prim-type", FilterOp.IN, ["Mesh"]),
        ]
        assert _filtered_prim_count(destination_owner, exported_meshes) == 2
        assert _filtered_prim_count(
            destination_owner,
            [Predicate("usd-path", FilterOp.IN, ["/World/OutsideSelection"])],
        ) == 0
        assert _filtered_prim_count(
            destination_owner,
            exported_meshes + [Predicate("usd-schemas", FilterOp.CONTAINS, ["PhysicsCollisionAPI"])],
        ) == 2
        assert _filtered_prim_count(
            destination_owner,
            [
                Predicate("usd-path", FilterOp.IN, [root + "/Floor"]),
                Predicate("usd-schemas", FilterOp.CONTAINS, ["PhysicsRigidBodyAPI"]),
            ],
        ) == 0
        assert _filtered_prim_count(
            destination_owner,
            [
                Predicate("usd-path", FilterOp.IN, [root + "/Dynamic"]),
                Predicate("usd-schemas", FilterOp.CONTAINS, ["PhysicsRigidBodyAPI"]),
            ],
        ) == 1

        exported_names = _attribute_names(destination_owner, root + "/Dynamic")
        assert {
            "points",
            "faceVertexCounts",
            "faceVertexIndices",
            "physics:collisionEnabled",
            "physics:rigidBodyEnabled",
        } <= exported_names


def test_export_changed_property_overlay(stage, tmp_path):
    prim_paths = ["/World/SensorA", "/World/SensorB", "/World/SensorC"]
    _write_float_columns(
        stage,
        prim_paths,
        {"temperature": np.array([20.0, 21.0, 22.0], dtype=np.float32)},
        ordinal=1,
    )

    rule = {"source_attribute_name": "temperature", "usd_type_name": "float"}
    base_destination = tmp_path / "base-properties.usda"
    base_report = population.export_to_usd_file(
        stage,
        str(base_destination),
        ordinal=1,
        attribute_rules=[rule],
    )
    assert base_report["attributes_exported"] == 3

    _write_float_columns(
        stage,
        ["/World/SensorB"],
        {"temperature": np.array([24.5], dtype=np.float32)},
        ordinal=2,
    )
    changed_destination = tmp_path / "changed-properties.usda"
    # [snippet:export-changed-properties-to-usd]
    report = population.export_to_usd_file(
        stage,
        str(changed_destination),
        ordinal=2,
        selection=population.OVSTAGE_POPULATION_EXPORT_SELECTION_CHANGED_PROPERTIES_SINCE_ORDINAL,
        since_ordinal=1,
        attribute_rules=[rule],
    )
    # [/snippet:export-changed-properties-to-usd]

    assert report["attributes_exported"] == 1
    assert report["prims_exported"] == 1
    changed_text = changed_destination.read_text(encoding="utf-8")
    assert 'over "SensorB"' in changed_text
    assert "SensorA" not in changed_text
    assert "SensorC" not in changed_text


def test_async_export_completes_with_report(stage, tmp_path):
    _write_local_transforms(stage, ["/World/Cube"], [(1.0, 2.0, 3.0)], ordinal=1)
    destination = tmp_path / "async-exported.usda"
    # [snippet:export-runtime-to-usd-async]
    operation = population.export_to_usd_file_async(
        stage,
        str(destination),
        ordinal=1,
        attribute_rules=["omni:xform"],
    )
    report = operation.wait()
    # [/snippet:export-runtime-to-usd-async]

    assert destination.exists()
    assert report["attributes_exported"] == 1
    assert report["transforms_exported"] == 1


def test_population_export_package_surface_and_descriptor_layout(ovstage_mod):
    assert population.SourceApiSchemaPolicy.APPLY_RECORDED == 1
    assert population.ExportProjection.SCHEMA_DECLARED == 1
    assert callable(population.export_typed_hierarchy_to_usd_file)
    assert callable(population.export_typed_hierarchy_to_usd_file_async)
    for removed in (
        "export_to_usd_layer",
        "export_to_usd_layer_async",
        "export_to_usd_stage",
        "export_to_usd_stage_async",
        "export_typed_hierarchy_to_usd_layer",
        "export_typed_hierarchy_to_usd_layer_async",
    ):
        assert not hasattr(population, removed)

    desc, _ = _descriptor()
    assert desc.struct_size == ctypes.sizeof(_bindings.ovstage_population_export_desc_t)
    assert desc.source_api_schema_policy == 1
    assert desc.projection == 1
    fields = _bindings.ovstage_population_export_desc_t._fields_
    assert fields[1] == ("source_api_schema_policy", ctypes.c_int)
    assert fields[2] == ("projection", ctypes.c_int)
    assert _bindings.ovstage_population_export_desc_t.source_api_schema_policy.offset == 4
    assert _bindings.ovstage_population_export_desc_t.projection.offset == 8

    lib = _bindings.load()
    native_desc = _bindings.ovstage_population_export_desc_t()
    assert lib.ovstage_population_export_desc_init(ctypes.byref(native_desc)) == _bindings.OVSTAGE_OK
    assert native_desc.struct_size == ctypes.sizeof(_bindings.ovstage_population_export_desc_t)
    assert native_desc.source_api_schema_policy == _export.SourceApiSchemaPolicy.EXPLICIT_ONLY
    assert native_desc.projection == _export.ExportProjection.EXPLICIT_RULES


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("selection", 99),
        ("layer_mode", 99),
        ("transform_policy", 99),
        ("unknown_metadata_policy", 99),
        ("source_api_schema_policy", 99),
        ("projection", 99),
    ],
)
def test_population_export_package_rejects_unknown_descriptor_enum_values(field, value):
    with pytest.raises(ValueError):
        _descriptor(**{field: value})


@pytest.mark.parametrize(
    "options",
    [
        {"attribute_rules": [{"source_attribute_name": "radius", "path_match": 99}]},
        {
            "attribute_rules": [
                {"source_attribute_name": "radius", "source_attribute_name_match": 99}
            ]
        },
        {"property_rules": [{"source_property_name": "rel", "path_match": 99}]},
        {"property_rules": [{"source_property_name": "rel", "property_kind": 99}]},
        {"property_rules": [{"source_property_name": "rel"}]},
        {"prim_rules": [{"path": "/World", "path_match": 99}]},
        {
            "api_schema_rules": [
                {"path": "/World", "schema_name": "PhysicsRigidBodyAPI", "path_match": 99}
            ]
        },
        {"metadata_rules": [{"source_metadata_name": "kind", "path_match": 99}]},
        {"metadata_rules": [{"source_metadata_name": "kind", "metadata_kind": 99}]},
    ],
)
def test_population_export_package_rejects_unknown_rule_enum_values(options):
    with pytest.raises(ValueError):
        _descriptor(**options)


def test_population_export_package_typed_helpers_compile_the_same_policies(monkeypatch):
    calls = []

    def capture_file_sync(stage, identifier, ordinal, **options):
        calls.append(("file-sync", options))
        return {"attributes_exported": 2}

    expected_file_async = object()

    def capture_file_async(stage, identifier, ordinal, **options):
        calls.append(("file-async", options))
        return expected_file_async

    monkeypatch.setattr(_export, "export_to_usd_file", capture_file_sync)
    monkeypatch.setattr(_export, "export_to_usd_file_async", capture_file_async)

    file_report = population.export_typed_hierarchy_to_usd_file(
        _Stage(), "scene.usda", "/", 7, include_custom_namespaces=["simulation"]
    )
    file_operation = population.export_typed_hierarchy_to_usd_file_async(
        _Stage(), "scene.usda", "/", 7, include_custom_namespaces=["simulation"]
    )

    assert file_report["attributes_exported"] == 2
    assert file_operation is expected_file_async
    assert [kind for kind, _ in calls] == ["file-sync", "file-async"]
    for _, options in calls:
        assert options["source_api_schema_policy"] == population.SourceApiSchemaPolicy.APPLY_RECORDED
        assert options["projection"] == population.ExportProjection.SCHEMA_DECLARED
        assert options["prim_predicate"].kind == population.PrimPredicateKind.IS_UNDER_PATH
        assert options["prim_predicate"].strings == ("/",)
        assert options["attribute_rules"][0]["source_attribute_name"] == "simulation:*"


def test_population_export_package_typed_policy_runs_native_export_and_persists(stage, tmp_path):
    # [snippet:export-typed-hierarchy-to-usd-file]
    scene = """#usda 1.0
def Xform "World"
{
    def Xform "Props"
    {
        def Sphere "Ball" (prepend apiSchemas = ["PhysicsCollisionAPI"])
        {
            double radius = 1.5
            bool physics:collisionEnabled = true
        }
        def Cube "Crate" (prepend apiSchemas = ["PhysicsRigidBodyAPI"])
        {
            double size = 2
            bool physics:rigidBodyEnabled = true
        }
    }
}
"""
    population.open_usd_from_string(
        stage,
        scene,
        ordinal=1,
        time_code=math.nan,
        domains=PopulationDomain.ALL,
    )
    stage.advance_write_floor(1).wait()

    output = tmp_path / "typed-hierarchy.usda"
    report = population.export_typed_hierarchy_to_usd_file(
        stage,
        str(output),
        "/World/Props",
        1,
    )
    # [/snippet:export-typed-hierarchy-to-usd-file]

    assert report["prims_exported"] >= 3
    assert report["api_schemas_applied"] == 2
    assert report["attributes_exported"] >= 4
    text = output.read_text(encoding="utf-8")
    assert 'def Sphere "Ball"' in text
    assert 'def Cube "Crate"' in text
    assert "double radius = 1.5" in text
    assert "PhysicsCollisionAPI" in text
    assert "PhysicsRigidBodyAPI" in text

    async_output = tmp_path / "typed-hierarchy-async.usda"
    operation = population.export_typed_hierarchy_to_usd_file_async(
        stage,
        str(async_output),
        "/World/Props",
        1,
    )
    async_report = operation.wait()

    assert async_report["prims_exported"] >= 3
    assert async_report["attributes_exported"] >= 4
    async_text = async_output.read_text(encoding="utf-8")
    assert 'def Sphere "Ball"' in async_text
    assert 'def Cube "Crate"' in async_text
    assert "double radius = 1.5" in async_text
