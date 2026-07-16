# ovstage Examples

This directory contains example projects demonstrating the ovstage runtime stage.
The C examples share the check/wait helpers in
[c/common/ovstage_example_utils.h](c/common/ovstage_example_utils.h); the `minimal`
example keeps them inline so it stays self-contained. The examples fail fast —
any unexpected API failure prints and exits, so the happy path reads straight
through; a real application would propagate errors instead.

Each example is a self-contained project: the C examples build standalone with
CMake against the released ovstage package, and the Python examples run with
[uv](https://docs.astral.sh/uv/) against the released `ovstage` wheel. Per-example
READMEs carry the exact build and run commands.

## Example Projects


<table>
  <tr>
    <td align="center" width="50%">
      <b>Minimal</b>
      <br>
      <blockquote>
        <p align="left"><em>“Create the smallest useful ovstage example that interns prim paths through the path dictionary, creates an instance, writes an attribute column, advances the write floor, and reads the latest committed data back while cleaning up resources appropriately.”</em></p>
      </blockquote>
      <sub>Build &amp; run in: <a href="c/minimal/">C →</a>, <a href="python/minimal/">Python →</a></sub>
    </td>
    <td align="center" width="50%">
      <b>Runtime Loop</b>
      <br>
      <blockquote>
        <p align="left"><em>“Create a headless ovstage example that loads a USD scene, populates it into the runtime stage, reads back prim data to confirm it landed, then demonstrates the two ways a client updates a live stage — animating a prim's transform by writing directly into the ovstage table, and editing the USD source and propagating it through — reading after each to show the change, and cleaning up resources appropriately.”</em></p>
      </blockquote>
      <sub>Build &amp; run in: <a href="c/runtime-loop/">C →</a>, <a href="python/runtime-loop/">Python →</a></sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <b>Time and Ordinals</b>
      <br>
      <blockquote>
        <p align="left"><em>“Create an ovstage example where the application owns time: a saxpy simulator steps spheres with non-uniform dt while a time-sampled USD clip animates alongside, both landing in the same ordinal slot per tick through the app's explicit ordinal-to-time table — every sealed ordinal a time-coherent snapshot for consumers — with a final clip rewind showing USD sampling time is playback policy, free to diverge from the timeline.”</em></p>
      </blockquote>
      <sub>Build &amp; run in: <a href="c/time-and-ordinals/">C →</a>, <a href="python/time-and-ordinals/">Python →</a></sub>
    </td>
    <td align="center" width="50%">
      <b>Write Flavors</b>
      <br>
      <blockquote>
        <p align="left"><em>“Create one ovstage example that tours every write path: scalar and fixed-lane columns, ragged arrays, semantic roles, upsert vs insert admission, sparse index-map and masked writes, batched multi-attribute writes, subtree cloning, attribute and whole-prim deletion, the CPU map/unmap lifecycle, pipelined asynchronous submission with zero-timeout polling, and — in Python — zero-copy CUDA ingest from warp.”</em></p>
      </blockquote>
      <sub>Build &amp; run in: <a href="c/write-flavors/">C →</a>, <a href="python/write-flavors/">Python →</a></sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <b>Producer–Consumer</b>
      <br>
      <blockquote>
        <p align="left"><em>“Create an ovstage example of the pull-based update model: a producer writes at its own rate and advances the write floor while a consumer tracks the last ordinal it saw and reads only the delta — tombstones included — showing that a lagging consumer simply gets a larger delta.”</em></p>
      </blockquote>
      <sub>Build &amp; run in: <a href="c/producer-consumer/">C →</a>, <a href="python/producer-consumer/">Python →</a></sub>
    </td>
    <td align="center" width="50%">
      <b>Authoring &amp; Hierarchy</b>
      <br>
      <blockquote>
        <p align="left"><em>“Create an ovstage example that builds a multi-environment world with zero USD — author prims, types, schemas, and local transforms directly, clone a prototype into environments, and show that derived world transforms are stale until a hierarchy compute derives them.”</em></p>
      </blockquote>
      <sub>Build &amp; run in: <a href="c/authoring-hierarchy/">C →</a>, <a href="python/authoring-hierarchy/">Python →</a></sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <b>Queries</b>
      <br>
      <blockquote>
        <p align="left"><em>“Create an ovstage example that discovers prims with filter queries instead of path lists — type, path-prefix, parent, children, and applied-schema predicates plus attribute presence — introspects what the query found, and — in C — maps scene-graph instancing structure.”</em></p>
      </blockquote>
      <sub>Build &amp; run in: <a href="c/queries/">C →</a>, <a href="python/queries/">Python →</a></sub>
    </td>
    <td align="center" width="50%">
      <b>USD to ovstage Migration</b>
      <br>
      <blockquote>
        <p align="left"><em>“Create an example that runs one small workflow twice in one process — define typed prims, author attributes one by one, batch a group of edits, read the values back — first with the plain USD API on a stage never bound to ovstage, then with the ovstage equivalents, contrasting USD's per-prim calls with ovstage's vectorized and batched writes by op count.”</em></p>
      </blockquote>
      <sub>Build &amp; run in: <a href="python/usd-to-ovstage/">Python only →</a></sub>
    </td>
  </tr>
</table>

## Snippets and skills

The example sources are the **source of truth for the code snippets** referenced
by the ovstage skills (`../skills/*/SKILL.md`). Skills reference `[snippet:name]` …
`[/snippet:name]` regions via `> **Source:**` lines, and CI validates that every
such reference resolves to an existing region — keep the markers intact when
editing example code. Not every region is referenced by a skill yet; unreferenced
regions mark reusable patterns for future skill updates.
