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
# Client-side authoring + hierarchy: build a multi-environment world with ZERO
# USD -- prims come into existence via attribute writes to nested paths -- then
# show that derived world transforms are pull-computed: they stay stale until a
# hierarchy computation model runs. The world-transform compute runs on the GPU
# in this ovstage revision, so a CUDA-capable device is required.
#
# Expected output: see README.md. Snippet markers are referenced by the skills
# under ../../../skills/ -- keep them intact.

"""Client-side authoring + hierarchy: zero-USD multi-env world, pull-computed world transforms."""

# [snippet:setup]
import numpy as np

import ovstage
from ovstage import (AttributeSemantic, DLDataType, DLDataTypeCode, HierarchyComputationModel,
                     HierarchyRelation, OrdinalRange, OvstageError, PrimMode, make_dltensor)
# [/snippet:setup]

# The prototype subtree: nested paths only -- hierarchy derives from the path
# structure, no parenting API involved. Arm and Body are siblings under Proto.
PROTO_PATHS = ["/World", "/World/Proto", "/World/Proto/Arm", "/World/Proto/Body", "/World/Proto/Body/Tip"]

# One USD prim type per prototype prim (interned to tokens below).
PROTO_TYPES = ["Xform", "Xform", "Cube", "Xform", "Cube"]

# Distinct local translation per hierarchy level (row-vector convention:
# translation in matrix elements [12..14]), so every world translation is a
# recognizable sum down its chain.
PROTO_LOCALS = [(0.0, 100.0, 0.0), (10.0, 0.0, 0.0), (0.0, 0.0, 3.0), (0.0, 0.0, 5.0), (0.0, 0.0, 2.0)]

ENV_PATHS = ["/World/Env_0", "/World/Env_1", "/World/Env_2"]

# Derived world-transform column maintained by the hierarchy computation models.
WORLD_MATRIX = "omni:fabric:worldMatrix"

# The Tip prim of the prototype and of each clone: printed after the compute.
TIP_PATHS = ["/World/Proto/Body/Tip"] + [env + "/Body/Tip" for env in ENV_PATHS]


def main() -> int:
    # A Stage owns the ovstage instance; its path dictionary is instance-owned.
    with ovstage.Stage("example.authoring-hierarchy") as stage, ovstage.PathDictionary(stage) as paths:
        proto_list = paths.create_path_list_from_strings(PROTO_PATHS)
        body_list = paths.create_path_list_from_strings(["/World/Proto/Body"])
        with (stage.query_from_path_list(proto_list) as proto_query,
              stage.query_from_path_list(body_list) as body_query):
            # ---- 1. author at ordinal 1: the prototype subtree, zero USD ----

            # [snippet:insert-author-subtree]
            # An INSERT write of any attribute to nested paths materializes the
            # prims; hierarchy derives purely from the path structure. INSERT is
            # create-only (PRIM_NOT_FOUND if a prim already exists), so it
            # doubles as an "author exactly once" guard.
            stage.write_attribute(
                proto_query, "proto:part", ordinal=1,
                tensors=np.arange(len(PROTO_PATHS), dtype=np.float32),
                is_array=False, prim_mode=PrimMode.INSERT,
            ).wait()
            # [/snippet:insert-author-subtree]

            # [snippet:author-prim-types]
            # Stamp USD prim types client-side via the reserved "usd-prim-type"
            # attribute: one interned token id per prim (scalar uint64 column).
            # The same ids later serve usd-prim-type IN query filters.
            type_ids = np.array([paths.intern_token(name) for name in PROTO_TYPES], dtype=np.uint64)
            stage.write_attribute(proto_query, "usd-prim-type", ordinal=1, tensors=type_ids,
                                  is_array=False).wait()
            # [/snippet:author-prim-types]

            # [snippet:author-applied-schemas]
            # Apply API schemas via the reserved "usd-schemas" attribute: an
            # ARRAY of interned token ids per prim (is_array=True). Each row is
            # a whole-set assignment -- a write replaces that prim's full schema
            # set, and an empty row clears it.
            schema_ids = np.array(
                [paths.intern_token(name) for name in ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]],
                dtype=np.uint64,
            )
            stage.write_attribute(body_query, "usd-schemas", ordinal=1, tensors=schema_ids,
                                  is_array=True).wait()
            # [/snippet:author-applied-schemas]

            # [snippet:author-local-transforms]
            # omni:xform is the canonical 4x4 double local-matrix column (an
            # alias of the derived `omni:fabric:localMatrix`): ONE 16-lane float64
            # element per prim, MATRIX semantic. Sealing ordinal 1 makes the
            # authored state readable.
            stage.write_attribute(
                proto_query, "omni:xform", ordinal=1, tensors=_matrix_rows(PROTO_LOCALS),
                is_array=False, semantic=AttributeSemantic.MATRIX,
            ).wait()
            stage.advance_write_floor(ordinal=1).wait()
            # [/snippet:author-local-transforms]

            # ---- 2. read back prim types + applied schemas ----
            # usd-schemas rows have no guaranteed element order; print sorted.
            prim_type = paths.intern_token("usd-prim-type")
            schemas = paths.intern_token("usd-schemas")
            print("prototype prim types:",
                  " ".join(_resolve_column_tokens(stage, paths, proto_query, prim_type, 1)))
            print("applied schemas on /World/Proto/Body:",
                  " ".join(sorted(_resolve_column_tokens(stage, paths, body_query, schemas, 1))))

            # ---- 3. clone at ordinal 2: /World/Proto -> /World/Env_0..2 ----
            # [snippet:clone-prototype-envs]
            # Clone the prototype subtree to three targets in ONE call (clone is
            # an ordinal-keyed write; the source must exist, the targets must
            # not), then give each environment root a distinct translation so
            # the environments tile along +X.
            stage.clone("/World/Proto", ENV_PATHS, ordinal=2)
            for index, env in enumerate(ENV_PATHS):
                _write_local_translation(stage, paths, env, (100.0 * (index + 1), 0.0, 0.0), ordinal=2)
            stage.advance_write_floor(ordinal=2).wait()
            print("cloned /World/Proto ->", " ".join(ENV_PATHS))
            # [/snippet:clone-prototype-envs]

            # ---- 4. enumerate hierarchy computation models ----
            # [snippet:hierarchy-computation-models]
            # In this ovstage revision the GPU models recompute world transforms
            # for client-authored prims; cpu-incremental derives them only
            # during USD population flows -- hence GPU_INCREMENTAL below.
            models = stage.get_hierarchy_computation_models()
            print("hierarchy computation models:", " ".join(model.name for model in models))
            # [/snippet:hierarchy-computation-models]

            # ---- 5. staleness demo: world transforms are pull-computed ----
            # [snippet:world-matrix-staleness]
            # Derived world transforms are PULL-computed. Before any compute the
            # derived column does not even exist: reading it yields no rows (a
            # normal state, not an error). A pure client-side stage then
            # materializes the output column itself (identity placeholder rows
            # -- USD population would have created them); the placeholders stay
            # STALE until a computation model runs. compute_hierarchy takes the
            # ordinal to compute from (input) and the ordinal stamped onto the
            # derived results (output).
            translation = _read_world_translation(stage, paths, "/World/Env_0/Body/Tip", end_ordinal=2)
            print("world matrix rows before compute_hierarchy:",
                  "absent (derived rows are pull-computed)" if translation is None else "unexpectedly present")

            # 5 prototype prims + 4 prims per cloned environment (Env_i, Arm, Body, Tip).
            all_paths = PROTO_PATHS + [env + suffix for env in ENV_PATHS
                                       for suffix in ["", "/Arm", "/Body", "/Body/Tip"]]
            all_list = paths.create_path_list_from_strings(all_paths)
            with stage.query_from_path_list(all_list) as all_query:
                stage.write_attribute(
                    all_query, WORLD_MATRIX, ordinal=3,
                    tensors=_matrix_rows([(0.0, 0.0, 0.0)] * len(all_paths)),
                    is_array=False, semantic=AttributeSemantic.MATRIX,
                ).wait()
                stage.advance_write_floor(ordinal=3).wait()
            paths.destroy_path_list(all_list)

            translation = _read_world_translation(stage, paths, "/World/Env_0/Body/Tip", end_ordinal=3)
            if translation is None:
                raise SystemExit("no derived world matrix row for /World/Env_0/Body/Tip"
                                 " after seeding placeholders")
            print("stale placeholder world translation /World/Env_0/Body/Tip:", _fmt_translation(translation))

            try:
                stage.compute_hierarchy(input_ordinal=3, output_ordinal=3,
                                        model=HierarchyComputationModel.GPU_INCREMENTAL)
            except OvstageError as err:  # expected without a CUDA-capable device
                raise SystemExit(f"compute_hierarchy failed (code {int(err.code)}): {err.message}\n"
                                 "the world-transform compute runs on the GPU and needs a CUDA-capable"
                                 " device")

            # Now the derived rows are correct compositions: each Tip world
            # translation is the sum of the pure translations down its chain.
            for tip in TIP_PATHS:
                translation = _read_world_translation(stage, paths, tip, end_ordinal=3)
                if translation is None:
                    raise SystemExit(f"no derived world matrix row for {tip} after compute_hierarchy")
                print(f"world translation {tip}:", _fmt_translation(translation))
            # [/snippet:world-matrix-staleness]

            # ---- 6. late edit: move /World/Env_1, compute again ----
            # [snippet:recompute-after-edit]
            # A local edit at a later ordinal needs another compute_hierarchy
            # with the new input ordinal to land in the derived rows. Reading
            # the derived column between the edit and the recompute is NOT a
            # reliable staleness probe -- the read itself can pull an
            # incremental refresh -- so this example reads only after the
            # compute.
            _write_local_translation(stage, paths, "/World/Env_1", (200.0, 0.0, 40.0), ordinal=4)
            stage.advance_write_floor(ordinal=4).wait()
            print("moved /World/Env_1 at ordinal 4")

            stage.compute_hierarchy(input_ordinal=4, output_ordinal=4,
                                    model=HierarchyComputationModel.GPU_INCREMENTAL)
            translation = _read_world_translation(stage, paths, "/World/Env_1/Body/Tip", end_ordinal=4)
            if translation is None:
                raise SystemExit("no derived world matrix row for /World/Env_1/Body/Tip after recompute")
            print("recomputed world translation /World/Env_1/Body/Tip:", _fmt_translation(translation))
            # [/snippet:recompute-after-edit]

            # ---- 7. hierarchy lookups: parent / children / siblings ----
            # Relation paths read back unordered; print sorted.
            print("parent of /World/Proto/Body:",
                  " ".join(sorted(_lookup(stage, paths, "/World/Proto/Body", HierarchyRelation.PARENT, 4))))
            print("children of /World/Proto:",
                  " ".join(sorted(_lookup(stage, paths, "/World/Proto", HierarchyRelation.CHILDREN, 4))))
            print("siblings of /World/Proto/Body:",
                  " ".join(sorted(_lookup(stage, paths, "/World/Proto/Body", HierarchyRelation.SIBLINGS, 4))))

        # Happy-path cleanup: the query `with` blocks released their handles;
        # return the path-list references before the stage goes down.
        paths.destroy_path_list(body_list)
        paths.destroy_path_list(proto_list)

    return 0


def _matrix_rows(rows):
    """4x4 double translation matrices as a matrix column expects them: row-major, translation
    in the last row. This helper uses the canonical transport form: shape [len(rows)] with a
    lanes=16 dtype. A compact 4x4-of-lanes=1 write is accepted but normalizes to this raw form."""
    data = np.array(
        [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, tx, ty, tz, 1] for (tx, ty, tz) in rows], dtype=np.float64
    )
    return make_dltensor(
        np.ascontiguousarray(data.ravel()),
        dtype=DLDataType(code=DLDataTypeCode.kDLFloat, bits=64, lanes=16),
        shape=[len(rows)],
    )


def _fmt_translation(translation):
    """Format a translation tuple as "x y z" with one decimal each."""
    return " ".join(f"{value:.1f}" for value in translation)


def _write_local_translation(stage, paths, path, translation, ordinal):
    """Write one prim's local transform (omni:xform) as a pure translation."""
    path_list = paths.create_path_list_from_strings([path])
    with stage.query_from_path_list(path_list) as query:
        stage.write_attribute(
            query, "omni:xform", ordinal=ordinal, tensors=_matrix_rows([translation]),
            is_array=False, semantic=AttributeSemantic.MATRIX,
        ).wait()
    paths.destroy_path_list(path_list)


def _read_world_translation(stage, paths, path, end_ordinal):
    """Read one prim's derived world translation (``omni:fabric:worldMatrix`` elements [12..14]).

    Returns a (tx, ty, tz) tuple, or None when the derived column has no row for
    this prim -- a normal state before any hierarchy compute, not an error."""
    path_list = paths.create_path_list_from_strings([path])
    translation = None
    with (stage.query_from_path_list(path_list) as query,
          stage.read_attributes(query, [paths.intern_token(WORLD_MATRIX)],
                                OrdinalRange.latest(end_ordinal)) as read):
        read.wait()
        group = read.fetch_next()
        if group is not None:
            # This single-prim query must yield exactly one 16-lane float64 row.
            # Check tensor_count before touching tensor 0: a zero-tensor group
            # would make group.tensor(0) raise instead of this diagnostic.
            if group.tensor_count != 1:
                raise SystemExit(f"unexpected {WORLD_MATRIX} read-group shape for {path}")
            tensor = group.tensor(0)
            if (not tensor.data or tensor.shape_tuple != (1,)
                    or tensor.dtype.code != DLDataTypeCode.kDLFloat
                    or tensor.dtype.bits != 64 or tensor.dtype.lanes != 16):
                raise SystemExit(f"unexpected {WORLD_MATRIX} read-group shape for {path}")
            # A data index_map (sparse row gather) can only map to row 0 here.
            row = group.data_row_index(0) if group.has_data_index_map else 0
            matrix = np.asarray(group.array(0)).ravel()
            translation = (float(matrix[row * 16 + 12]), float(matrix[row * 16 + 13]),
                           float(matrix[row * 16 + 14]))
            stage.release_group(group)
    paths.destroy_path_list(path_list)
    return translation


def _resolve_column_tokens(stage, paths, query, attr_token, end_ordinal):
    """Read a token-id column (uint64 ids) and resolve every id back to its string."""
    names = []
    with stage.read_attributes(query, [attr_token], OrdinalRange.latest(end_ordinal)) as read:
        read.wait()
        for group in read.groups():
            for token_id in np.asarray(group.array(0)).ravel().tolist():
                names.append(paths.token_to_string(int(token_id)))
            stage.release_group(group)
    return names


# [snippet:hierarchy-lookups]
# Backend hierarchy lookup for one prim: get_hierarchy takes an ordered path
# list and a relation, and returns per-input items (a missing input prim
# reports item status NOT_FOUND instead of failing the batch). The blocking
# wrapper fetches and releases the lookup handle internally. Relation paths
# come back as-is, with no guaranteed order.
def _lookup(stage, paths, target, relation, ordinal):
    path_list = paths.create_path_list_from_strings([target])
    result = stage.get_hierarchy(path_list, ordinal=ordinal, relation=relation)
    paths.destroy_path_list(path_list)
    item = result.items[0]
    return item.paths if item.ok else []
# [/snippet:hierarchy-lookups]


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OvstageError as err:
        print(f"ovstage error (code {int(err.code)}): {err.message}")
        raise SystemExit(1)
