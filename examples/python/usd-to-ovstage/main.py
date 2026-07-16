# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
#
# NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
# property and proprietary rights in and to this material, related
# documentation and any modifications thereto. Any use, reproduction,
# disclosure or distribution of this material and related documentation
# without an express license agreement from NVIDIA CORPORATION or
# its affiliates is strictly prohibited.
#
# USD-to-ovstage migration: the SAME small workflow twice in one process --
# create typed prims, author attribute values, batch a group of edits, read
# everything back -- first with plain USD via pxr (usd-core) on a stage never
# bound to ovstage, then with the ovstage equivalents (no USD). The op-count
# lines carry the lesson: the USD habit is one call per prim, ovstage is one
# vectorized write per column (and one batched op for several columns).
#
# Expected output: see README.md. Snippet markers are referenced by the
# skills under ../../../skills/ -- keep them intact.

"""One workflow, twice: plain USD via pxr, then the ovstage equivalents (vectorized + batched)."""

import numpy as np
from pxr import Sdf, Usd

import ovstage
from ovstage import OrdinalRange, OvstageError, OvxError, PrimMode, WriteDesc

# The workflow's world, shared by both halves: an Xform parent and four typed
# children. The S0..S3 labels in the prints are the last path components.
PRIM_PATHS = ["/World", "/World/S0", "/World/S1", "/World/S2", "/World/S3"]
PRIM_TYPES = ["Xform", "Cube", "Sphere", "Cone", "Cylinder"]
CHILD_PATHS = PRIM_PATHS[1:]
CHILD_NAMES = [path.rsplit("/", 1)[-1] for path in CHILD_PATHS]

# One value per child prim: temperature lands one-by-one and again vectorized;
# humidity + pressure land as the batched group of edits.
TEMPERATURE = [20.5, 21.5, 22.5, 23.5]
HUMIDITY = [40.0, 41.0, 42.0, 43.0]
PRESSURE = [101.3, 101.4, 101.5, 101.6]


def main() -> int:
    part1_plain_usd()
    print()
    part2_ovstage()
    print()
    # Migration ends on purpose: Part 1's stage is never handed to ovstage.
    # Mirroring an existing USD stage automatically is a different tool --
    # population -- shown by the runtime-loop example.
    print("mirroring an existing USD stage into ovstage automatically is population"
          " -- see the runtime-loop example")
    return 0


# ---- 1. plain USD via pxr: the habits being migrated ----
def part1_plain_usd() -> None:
    print("== Part 1: plain USD via pxr (usd-core), never bound to ovstage ==")

    # [snippet:usd-create-prims]
    # Plain USD authoring on an in-memory stage: one DefinePrim call per prim,
    # the parent Xform first, then each typed child.
    stage = Usd.Stage.CreateInMemory()
    prims = [stage.DefinePrim(path, type_name) for path, type_name in zip(PRIM_PATHS, PRIM_TYPES)]
    children = prims[1:]
    print(f"create: {len(prims)} DefinePrim calls (one per prim)")
    print("created prims:", ", ".join(f"{prim.GetPath()} ({prim.GetTypeName()})" for prim in prims))
    # [/snippet:usd-create-prims]

    # [snippet:usd-author-one-by-one]
    # The USD habit: one CreateAttribute(...).Set(...) per prim, then one
    # attr.Get() per prim to read back.
    for prim, value in zip(children, TEMPERATURE):
        prim.CreateAttribute("temperature", Sdf.ValueTypeNames.Double).Set(value)
    print(f"one-by-one: {len(children)} CreateAttribute+Set calls (one per prim)")
    print("temperature:", _fmt(prim.GetAttribute("temperature").Get() for prim in children))
    # [/snippet:usd-author-one-by-one]

    # [snippet:usd-changeblock-batch]
    # Sdf.ChangeBlock coalesces change NOTIFICATION only; the edits still
    # happen one Set call per attribute per prim (8 here). The attributes are
    # created ahead of the block so only plain value sets run inside it.
    humidity = [prim.CreateAttribute("humidity", Sdf.ValueTypeNames.Double) for prim in children]
    pressure = [prim.CreateAttribute("pressure", Sdf.ValueTypeNames.Double) for prim in children]
    with Sdf.ChangeBlock():
        for attr, value in zip(humidity, HUMIDITY):
            attr.Set(value)
        for attr, value in zip(pressure, PRESSURE):
            attr.Set(value)
    print(f"changeblock: {len(humidity) + len(pressure)} Set calls under one Sdf.ChangeBlock"
          " (coalesced notification, still per-prim edits)")
    print("humidity:", _fmt(attr.Get() for attr in humidity))
    print("pressure:", _fmt(attr.Get() for attr in pressure))
    # [/snippet:usd-changeblock-batch]


# ---- 2. the ovstage equivalents: one call per column, not per prim ----
def part2_ovstage() -> None:
    print("== Part 2: the ovstage equivalents, client-authored ==")
    # A Stage owns the ovstage instance. The application owns the ordinal
    # lifecycle (an ordinal is a version number the application picks): one
    # ordinal per step (1..4), each sealed with advance_write_floor so reads
    # can trust it.
    with ovstage.Stage("example.usd-to-ovstage") as stage, ovstage.PathDictionary(stage) as paths:
        all_list = paths.create_path_list_from_strings(PRIM_PATHS)
        children_list = paths.create_path_list_from_strings(CHILD_PATHS)
        child_lists = [paths.create_path_list_from_strings([path]) for path in CHILD_PATHS]
        all_query = stage.query_from_path_list(all_list)
        children_query = stage.query_from_path_list(children_list)
        child_queries = [stage.query_from_path_list(path_list) for path_list in child_lists]

        # [snippet:ovstage-create-prims]
        # DefinePrim's equivalent: there is no create-prim call -- prims come
        # into existence via an attribute write. ONE INSERT-mode (create-only)
        # write of the reserved usd-prim-type column creates all 5 prims AND
        # stamps their types: one interned token id (uint64) per prim.
        prim_type = paths.intern_token("usd-prim-type")
        type_ids = np.array([paths.intern_token(name) for name in PRIM_TYPES], dtype=np.uint64)
        stage.write_attribute(all_query, prim_type, ordinal=1, tensors=type_ids,
                              is_array=False, prim_mode=PrimMode.INSERT).wait()
        stage.advance_write_floor(ordinal=1).wait()
        print("create: 1 INSERT write op (the write itself creates all 5 prims"
              " and stamps their types)")

        token_ids = _read_column(stage, paths, all_query, prim_type,
                                 end_ordinal=1, expected_paths=PRIM_PATHS)
        print("created prims:", ", ".join(
            f"{path} ({paths.token_to_string(int(token))})"
            for path, token in zip(PRIM_PATHS, token_ids)))
        # [/snippet:ovstage-create-prims]

        # [snippet:ovstage-write-one-by-one]
        # The naive port of the USD habit: keep the per-prim loop, one
        # single-prim write op per value. Works -- it just buys none of the
        # batching ovstage is built around.
        temperature = paths.intern_token("temperature")
        for query, value in zip(child_queries, TEMPERATURE):
            stage.write_attribute(query, temperature, ordinal=2,
                                  tensors=np.array([value], np.float64), is_array=False).wait()
        stage.advance_write_floor(ordinal=2).wait()
        print(f"one-by-one: {len(child_queries)} write ops (one per prim)")
        print("temperature:", _fmt(_read_column(stage, paths, children_query, temperature,
                                                end_ordinal=2, expected_paths=CHILD_PATHS)))
        # [/snippet:ovstage-write-one-by-one]

        # [snippet:ovstage-write-vectorized]
        # The idiomatic port: ONE vectorized write op lands the whole column
        # -- one tensor holding one row per prim in the query. Same values as
        # the loop above, a quarter of the ops.
        stage.write_attribute(children_query, temperature, ordinal=3,
                              tensors=np.array(TEMPERATURE, np.float64), is_array=False).wait()
        stage.advance_write_floor(ordinal=3).wait()
        print(f"vectorized: 1 write op (one column covering all {len(CHILD_PATHS)} prims)")
        print("temperature:", _fmt(_read_column(stage, paths, children_query, temperature,
                                                end_ordinal=3, expected_paths=CHILD_PATHS)))
        # [/snippet:ovstage-write-vectorized]

        # [snippet:ovstage-write-batched]
        # Sdf.ChangeBlock's closest relative: write_attributes lands SEVERAL
        # columns in one batched op (one WriteDesc per column, all sharing the
        # ordinal) -- genuinely one op, but a grouping, not an atomic
        # transaction.
        humidity = paths.intern_token("humidity")
        pressure = paths.intern_token("pressure")
        stage.write_attributes(children_query, [
            WriteDesc(attribute=humidity, tensors=np.array(HUMIDITY, np.float64), is_array=False),
            WriteDesc(attribute=pressure, tensors=np.array(PRESSURE, np.float64), is_array=False),
        ], ordinal=4).wait()
        stage.advance_write_floor(ordinal=4).wait()
        print("batched: 1 write op (2 columns: humidity + pressure)")
        print("humidity:", _fmt(_read_column(stage, paths, children_query, humidity,
                                             end_ordinal=4, expected_paths=CHILD_PATHS)))
        print("pressure:", _fmt(_read_column(stage, paths, children_query, pressure,
                                             end_ordinal=4, expected_paths=CHILD_PATHS)))
        # [/snippet:ovstage-write-batched]

        # Release every handle before Stage.__exit__ tears the instance down
        # (destroy requires all handles released first).
        for query in [all_query, children_query, *child_queries]:
            stage.release_query(query).wait()
        for path_list in [all_list, children_list, *child_lists]:
            paths.destroy_path_list(path_list)


# ---- plumbing: the one-read column readback and the shared value formatter ----


# [snippet:ovstage-read-column]
# attr.Get()'s equivalent, migrated like the writes: ONE read op returns the
# whole column -- the latest committed value at or below end_ordinal for every
# prim in the query. Each group names its covered prims via its own prim list,
# so rows are matched back to the expected paths through the path dictionary
# (never by assuming query order), honoring a data index_map when present.
# The `with` block releases the read handle.
def _read_column(stage, paths, query, attribute_token, end_ordinal, expected_paths):
    index_by_path = {path: i for i, path in enumerate(expected_paths)}
    values = [None] * len(expected_paths)
    with stage.read_attributes(query, [attribute_token], OrdinalRange.latest(end_ordinal)) as read:
        read.wait()
        for group in read.groups():
            handles = paths.get_paths(group.prim_list)
            rows = np.asarray(group.array(0)).ravel()
            for local in range(group.prim_count):
                path = paths.path_to_string(handles[group.prim_index(local)])
                row = group.data_row_index(local) if group.has_data_index_map else local
                values[index_by_path[path]] = rows[row]
            stage.release_group(group)
    return values
# [/snippet:ovstage-read-column]


def _fmt(values) -> str:
    """S0..S3 value cells, printed identically by the USD and ovstage halves."""
    return ", ".join(f"{name} = {float(value):.1f}" for name, value in zip(CHILD_NAMES, values))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OvstageError as err:
        print(f"ovstage error (code {int(err.code)}): {err.message}")
        raise SystemExit(1)
    except OvxError as err:
        print(f"path dictionary error (code {int(err.code)}): {err.message}")
        raise SystemExit(1)
