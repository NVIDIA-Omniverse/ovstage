# Transforms

Populate `car.usda` by selecting every Xform prim, update the wheel transform,
then override its reset state and read its world matrix. Requires a native
build with USD population and CPU incremental hierarchy computation.

Run `uv run main.py` in this directory.

Expected output:

```text
Wheel world x: 4.0
```

The snippets below are excerpts from [main.py](main.py). They require its
population and query setup, which defines `stage`, `paths`, and `query`.
After that setup, run them in order. `query` selects `/World/Car/Wheel`; its parent
has a translation of 10 along X. The USD source keeps transform inheritance enabled.

```python
import pathlib
import numpy as np
import ovstage
from ovstage import AttributeSemantic, HierarchyComputationModel, OrdinalRange, population
```

## Layout

```python
# One row-major matrix for the wheel; translation is in the last row.
matrices = np.eye(4, dtype=np.float64)[None, :, :]
matrices[0, 3, :3] = [4, 0, 0]
# A boolean per prim controls whether the parent transform is inherited.
reset = np.array([False], dtype=np.bool_)
```

## Write

```python
# Both attributes describe the same update, at ordinal 2.
stage.write_attribute(query, "omni:xform", ordinal=2, tensors=matrices,
                      is_array=False, semantic=AttributeSemantic.MATRIX).wait()
stage.write_attribute(query, "omni:resetXformStack", ordinal=2,
                      tensors=reset, is_array=False).wait()
# False keeps the wheel relative to the car. True would make it world-relative.
```

## Compute

```python
# CPU incremental computation supports this USD-populated hierarchy.
model = HierarchyComputationModel.CPU_INCREMENTAL
if model not in [item.model_id for item in stage.get_hierarchy_computation_models()]:
    raise RuntimeError("CPU incremental hierarchy computation is unavailable")
stage.compute_hierarchy(input_ordinal=2, output_ordinal=2, model=model)
stage.advance_write_floor(ordinal=2).wait()
# Car x=10 plus wheel local x=4 gives wheel world x=14.
```

## Override

```python
# Override the runtime reset without editing the source USD's xformOpOrder.
stage.write_attribute(query, "omni:resetXformStack", ordinal=3,
                      tensors=np.array([True], dtype=np.bool_), is_array=False).wait()
stage.compute_hierarchy(input_ordinal=3, output_ordinal=3, model=model)
stage.advance_write_floor(ordinal=3).wait()
# The unchanged matrix now puts the wheel at world x=4, ignoring the car.
```

## Read

```python
# Read the derived output only after computation; never write it directly.
world = paths.intern_token("omni:fabric:worldMatrix")
found = False
with stage.read_attributes(query, [world], OrdinalRange.latest(3)) as read:
    read.wait()
    for group in read.groups():
        with group:
            world_matrices = group.array(0).reshape(-1, 4, 4)
            for i in range(group.prim_count):
                row = group.data_row_index(i) if group.has_data_index_map else i
                print("Wheel world x:", float(world_matrices[row, 3, 0]))
                # Wheel world x: 4.0
                found = True
if not found:
    print("No derived world-matrix row is available.")
    # No derived world-matrix row is available.
```

## GPU transform writes

`gpu_write.py` provides a helper for existing CUDA buffers. Supply one
float64 matrix and one boolean reset flag per queried prim, and a CUDA event
recorded after producing both buffers. Keep the inputs alive until the helper
returns. The helper selects `GPU_GLOBAL` and computes the hierarchy after writing.
Use a populated hierarchy with existing derived-output rows. It does not read
payloads back to the CPU.

```python
from ovstage import AttributeSemantic, DLDataType, DLDataTypeCode, HierarchyComputationModel, make_dltensor


def write_gpu_transforms(stage, query, matrices, reset_flags, prim_count, ordinal, ready_event):
    """Write CUDA-resident matrices and reset flags after the producer event."""
    # Choose an available GPU model before changing the stage.
    model = HierarchyComputationModel.GPU_GLOBAL
    if model not in [item.model_id for item in stage.get_hierarchy_computation_models()]:
        raise RuntimeError("GPU global hierarchy computation is unavailable")

    # matrices is a contiguous CUDA DLPack producer with float64 values and
    # shape (prim_count, 4, 4) or (prim_count, 16). No buffer copy is needed.
    matrix_tensor = make_dltensor(matrices,
        dtype=DLDataType(code=DLDataTypeCode.kDLFloat, bits=64, lanes=16),
        shape=[prim_count])
    # reset_flags is a CUDA boolean array: True means explicit world transforms.
    reset_tensor = make_dltensor(reset_flags,
        dtype=DLDataType(code=DLDataTypeCode.kDLBool, bits=8, lanes=1),
        shape=[prim_count])

    # ready_event is the integer handle of a CUDA event recorded after both
    # buffers were produced. Waits synchronize work; they do not read values back.
    stage.write_attribute(query, "omni:xform", ordinal=ordinal,
        tensors=matrix_tensor, is_array=False, semantic=AttributeSemantic.MATRIX,
        cuda_event=ready_event).wait()
    stage.write_attribute(query, "omni:resetXformStack", ordinal=ordinal,
        tensors=reset_tensor, is_array=False, cuda_event=ready_event).wait()
    # Keep both buffers and the event alive and unchanged until these writes finish.
    # Compute from the completed update on the GPU; no payload readback.
    stage.compute_hierarchy(input_ordinal=ordinal, output_ordinal=ordinal, model=model)
    stage.advance_write_floor(ordinal=ordinal).wait()
```
