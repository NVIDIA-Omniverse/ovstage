# ovstage usd-export (Python)

Export selected runtime data to USD and inspect the result. The first workflow
populates an inline USD scene and exports its typed `/World/Props` subtree.
A second, separate runtime stage is authored directly to demonstrate transform
export, property selection and renaming, destination ownership, and change export.

The [C/C++ example](../../c/usd-export/README.md) covers the same workflows.

## At a glance

Start with typed-hierarchy export when the runtime data already carries USD type
and schema information. The later cases make export choices explicit for data
written directly by an application. Export selects and projects runtime data;
it is not an automatic serialization of every runtime column.

Pseudocode (workflow only):

```text
populate the inline scene; finish writes through ordinal 1
export /World/Props as typed USD and inspect the file
export the mesh subtree separately
on a new stage, write a transform and custom properties at ordinal 1
compare one-shot file export with an explicitly owned destination
select, rename, and type exported properties; inspect the reports
change temperature at ordinal 2; export changes since ordinal 1
```

## What you'll see

| Case | Result and meaning |
|---|---|
| Typed hierarchy | The file contains Sphere `Ball` and Cube `Crate`; `OutsideSelection` is excluded. |
| Mesh subtree | A separate file exports the meshes and their schema data. |
| Transform-only export | One transform is exported for the directly authored ball. |
| Reused destination | Transform and temperature exports are accumulated before an explicit save. |
| Renamed property | Runtime `solver:collisionRadius = 2.5` becomes USD `radius` on a Sphere definition. |
| Changed properties | Temperature changes from 20 to 24.5; the change export reports one attribute. |

The program also demonstrates closing a destination without saving and rejecting
an export request at an invalid earlier ordinal. These are deliberate lifetime
and error-handling cases, not additional successful file exports.

Files are created in a temporary directory and removed when the program exits.
The saved-file message describes work performed during the run; it does not name
a persistent output artifact. Inspect the file checks in the source to see what
was exported.

## How to read the code

Start in `main()` in [main.py](main.py), then follow the helpers it calls.
The names below are searchable `[snippet:name]` markers in that file.

| Source markers | What to learn |
|---|---|
| `typed-hierarchy-python-example`, `export-mesh` | Read these blocks in main first: select a subtree, initialize typed-hierarchy export, save it, and inspect the result. Meshes use a second selection. |
| `export-example-setup`, `export-write-runtime` | In the runtime-data helper, create a separate stage and author a transform, temperature, collision radius, and USD type information. |
| `export-file`, `export-lifetime`, `export-reuse` | Compare the file convenience operation (export, save, close) with a destination that the caller must explicitly save and close. |
| `export-custom`, `export-rule`, `export-rename` | Select a property namespace, then use an explicit rule when the runtime name or desired USD type differs. |
| `export-selection`, `export-matrix` | Narrow the selected prim/properties and choose explicit matrix-op export for the transform. |
| `export-report`, `export-async` | Inspect counts, handle the invalid-ordinal case, and wait for asynchronous file export to finish saving before using its report. |
| `export-changed` | Update temperature at ordinal 2, finish writes, and export properties changed since ordinal 1. |

## Build and run

Use an ovstage package with population/export support.
See the [shared setup](../../README.md) for package prerequisites.
Run these commands from this example's directory.

```bash
uv run main.py
```


## Notes

- Closing an owned destination does not save it. The one-shot file operation
  performs both steps; the reusable-destination path calls save explicitly.
- Export reads through a completed write ordinal. The example waits for its
  writes and floor advancement before requesting each export.
- Changed-property export selects changed properties; it does not turn the
  runtime stage into a general history archive.

## Further reading

- [Exporting runtime data to USD](../../../docs/scene/exporting_to_usd.rst)
- [Population example](../population/README.md)
- [Transform example](../transforms/README.md)
