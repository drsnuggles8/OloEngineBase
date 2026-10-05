# A renderer test case removes the assets it added

**Rule.** A `RendererAttachedTest` case that registers assets removes them when it ends. The base
fixture resets only the scene. The project and its asset manager live as long as the process, so a
memory-only asset a case adds (and every one the importer adds for it, such as generated LOD
meshes) stays registered and resident for every later case. Snapshot the manager's memory-only
assets when the case builds its scene, and remove the new ones and any files it imported in
`TearDown`. Take no snapshot, and remove nothing, when the base fixture skipped before
`BuildScene`.

## What happened (#1533)

The dog's evidence suite builds the dog for every case: a 256k-triangle skinned body, whose import
registers its LOD meshes as memory-only assets, plus a 410k-strand coat, its binding, the coat's
colour map and five skin profiles, all registered with `AddMemoryOnlyAsset`. Nothing removed them.
Each case left about 120 MB resident. In the full sweep the chunk holding the evidence suites
crossed the 6 GB per-process ceiling 24 dog cases in. The case named in the stop message peaks at
2.8 GB when run alone.

The memory ceiling names the running test, which is the test that crossed the line, not the one
that leaked. To find the leak, record the process's resident set at the start and end of each
case: a case that ends higher than it started, case after case, is holding something. Four
rendering cases repeated twice ended at 1,490 → 1,913 MB without the cleanup and 1,494 → 1,260 MB
with it.

Cases that skip after building the scene, before running a frame, still grow by about 40 MB each
with the cleanup in place; rendering cases do not. The likely holder is the deferred GPU deletion
queues, which only a frame or process shutdown flushes. It was not chased further.
