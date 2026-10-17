# ShadowPass attribution: family and class brackets, and the foliage cull's slots

**Rule.** Time the shadow pass through its view families and caster classes, not its total:
`ShadowPass/<Family>` (`Csm`, `Atlas`, `VsmPages`, `VsmCull`, `VsmSun`, `VsmLocal`) and, nested in it,
`ShadowPass/<Family>/<Class>` (`MeshCasters`, `SkinnedCasters`, `VoxelCasters`, `GroomCasters`,
`TerrainCasters`, `FoliageCull`, `FoliageCasters`, `VirtualGeometry`), opened through
`ShadowBrackets::Scope` (`Renderer/Shadow/ShadowPassBrackets.h`). A class bracket is per VIEW, so one
frame publishes a name many times: sum the repeats within a frame, never keep the first. And a
region recorded in parallel has more views than the foliage cull has slots: record it in batches of
`FoliageGPUCuller::kMaxShadowViews`. Issue #1590.

## Why

Before #1590 only foliage had brackets inside `ShadowPass`, and the pass was the largest at every
outdoor camera of the #1259 acceptance scene: 25–35 ms more under VSM than CSM, and 0.1–0.9 s through
the lamps' shadow atlas, with nothing saying which caster class paid it. The scripts that read the
brackets kept only the first entry of a name per frame (`acceptance-benchmark.py` excluded the rest as
`duplicate-frame-pass`), so of sixteen per-level VSM culls one was counted.

The atlas cost had a concrete cause. The foliage GPU cull has four shadow slots, one per view of a
region, and a region recorded in one batch gave every view past the fourth no slot: it drew every
generated plant on the island, uncompacted. One point light has six cube faces, so its last two did,
and every face of every light after it. `ShadowRenderPass::RecordShadowRegion` now culls and records a
region four views at a time, with a barrier between batches, since a batch's culls rewrite the slots
the previous batch's draws read. A cull slot's overflow demand accumulates across batches (it is an
`atomicMax`, never reset per cull), so the #1581 growth still sees every batch.

## The traps in the timer pool

* **Two levels of nesting, no more.** `GPUPassTimerPool::BeginSubPass` takes a sub-pass inside a
  sub-pass since #1590. A third level, or a sub-pass under one that got no bracket, is counted as untimed
  rather than silently dropped. A nested sub-pass keeps the TOP-LEVEL pass as its `ParentName`, so every
  sub-pass stays out of the frame total; a consumer that sums sub-passes must pick one level, or it adds
  `ShadowPass/Atlas` and `ShadowPass/Atlas/MeshCasters` together.
* **Class brackets vanish in a parallel item.** The pool is not thread-safe, so `ShadowBrackets::Scope`
  opens nothing inside a `RecordParallel` item. On Vulkan the CSM and atlas regions fork: only their
  region brackets and the region-level `FoliageCull` publish. On GL every item records inline and every
  class publishes. The VSM route is sequential on both.
* **Every Vulkan stamp ends the dynamic-rendering scope**, so a bracket inside the VSM raster scope
  splits it. Cheap, but not free; bracket classes, not draws.
* **The budget is 1024 brackets a frame** (2 timestamps each). Past it, each later bracket is counted
  as untimed.

## Checking it

`ShadowAtlasFoliageCullEvidenceTest` lights the foliage island with two shadowed lamps (twelve faces)
on all three paths: no view asks for a slot past the last (`FoliageRenderer::GetShadowViewsPastCullSlots`),
`Levers::FaultShadowFoliageCullCap` brings 64 back (the negative control), the frame matches the
single-batch one within its own noise, and a positive control (no foliage in the shadow maps) moves
about 44,000 pixels, so the comparison can see a moved shadow. Read an evidence frame through
`ApplyRendererSettings()`: a path written to `GetRendererSettings().Path` alone leaves Deferred's
graph unbuilt and the captured frame empty.
