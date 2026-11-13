# ovstage population (Python)

Load selected USD data into a runtime stage, inspect how values and metadata are
represented, and resample animated data. The fixture [scene.usda](scene.usda)
contains a rigid-body Cube at `/World/Body`, under a translated `/World`.
The program selects physics-related data through explicit applied-schema and
property-namespace predicates. It never steps a physics engine.

The [C/C++ example](../../c/population/README.md) covers the same workflow.

## At a glance

An ordinal orders runtime writes; USD time selects a sample from the source scene.
They are independent. After population completes, the example advances and waits
for the write floor before reading through that ordinal.

Pseudocode (workflow only):

```text
load scene with the physics selection at ordinal 1
load explicit properties and metadata at ordinal 2
read defaults, tokens, arrays, metadata, and ancestor transforms
sample USD at one second into ordinal 3
sample three more frames into ordinals 4, 5, and 6
add a subtree selection at ordinal 7
on a separate stage, fail a load and retry with a valid input
```

## What you'll see

The program prints individual values as it visits the readback sections. These
selected results explain what each read is checking:

| Read | Result and meaning |
|---|---|
| Cube `size` | `2.0`, supplied by the schema fallback rather than authored in the file. |
| `temperature` | `20.0` at default time, then `30.0` at one second (time code 24). |
| `state` and empty token | `ready` after token resolution; the empty token has identifier zero. |
| `points` and empty array | Two float3 elements; the empty array has zero elements. |
| Metadata | Stage rate `24.0`, property unit `C`, and prim label `robot`. |
| Type and schema | `Cube` and `PhysicsRigidBodyAPI`. |
| Ancestor transform | `/World` has translation `(10, 0, 0)`. |

The final missing-file attempt is intentional. Its failure is handled, followed
by a successful retry; it is not an unhandled example failure.

## How to read the code

Start in `main()` in [main.py](main.py). The names below are searchable
`[snippet:name]` markers in that file; each row explains why that section is there.

| Source markers | What to learn |
|---|---|
| `population-setup`, `population-paths` | Create the stage and path dictionary, load the physics selection, then explicitly select the body's properties. |
| `population-property-metadata`, `population-stage-metadata` | Extend the selection to metadata. Loading an attribute value and requesting its metadata are distinct choices. |
| `population-default`, `population-numeric`, `population-strings` | Compare a schema fallback, an authored number, and a token that must be resolved to text. |
| `population-arrays`, `population-empty`, `population-empty-array`, `population-extent` | Distinguish array length, element width, empty values, and the packed extent representation. |
| `population-stage-metadata-read`, `population-property-metadata-read`, `population-prim-metadata-read` | Read metadata at its three scopes rather than treating all metadata as prim attributes. |
| `population-reserved`, `population-schemas`, `population-ancestor` | Inspect populated type/schema information and the ancestor transform needed to place the body. |
| `population-time`, `population-ordinals`, `population-subtree` | Resample using seconds, assign independent write ordinals, and combine a Cube-under-World selector with the physics selector. |
| `population-failure` | Distinguish rejection at submission from failure after execution began, then choose a retry ordinal. |

## Build and run

Use an ovstage package with population support. See the [shared setup](../../README.md) for package prerequisites.
Run these commands from this example's directory.

```bash
uv run main.py
```

## Notes

- Explicit selection at ordinal 2 requests the fields used by the readback; the
  initial physics selection alone is not the whole example.
- A rejected submission has not run writes. A failure during execution can leave
  partial writes, so this example retries at a new ordinal in that case.
- Advancing the write floor closes further writes through that point; it does
  not turn each ordinal into a permanent historical snapshot.

## Further reading

- [Population and selection](../../../docs/scene/population.rst)
- [Reading attributes](../../../docs/scene/reading_attributes.rst)
- [Time and ordinals example](../time-and-ordinals/README.md)
