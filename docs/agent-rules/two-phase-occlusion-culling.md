# Two-phase occlusion culling: which pyramid, which region, and how to prove it

Distilled from issue **#682** (virtualized geometry), which mirrors the scheme
already run for instanced batches in #431 / #486. Read this before touching
`VirtualClusterCull.comp`, `InstanceOcclusionCull.comp`, or any pass that builds
an occlusion HZB.

## The rule that makes the whole scheme work

**Phase 1 must test the PREVIOUS frame's FINAL depth pyramid. A pyramid built
mid-frame can only contain the occluders drawn before the pass that builds it.**

That single sentence is the feature. Before #682 the virtual-geometry cull built
its pyramid from ScenePass's depth at the top of `VirtualGeometryPass::Execute` —
so it held classic-path occluders only, and a virtual-geometry cluster could
never be culled by other virtual geometry no matter how much of it was in front.
Raster cost scaled with the *selected* cluster count, not the *visible* one.

The retained pyramid (`Renderer3D::GenerateOcclusionHZB`, tail of `EndScene`) is
built from the final depth, so it contains everything — classic geometry,
instanced statics, and virtual geometry. Testing against it is what lets a
foreground VG building occlude the VG city behind it.

The cost of using last frame's pyramid is that it is in last frame's screen
space. Two consequences, both handled the same way the instanced path handles
them:

- Bounds are **reprojected**: the VP that matches the pyramid (`prev VP`, made
  relative to *this* frame's render origin — issue #429) and the object's
  **previous transform** (`PrevTransform`; identical to `Transform` for a static
  instance). Using the current transform against a previous-frame pyramid tests
  the object at a position that pyramid knows nothing about.
- Phase 1's verdict is **provisional**. A hit is appended to a reject list, not
  dropped. Phase 2 re-tests exactly those rejects against a pyramid rebuilt from
  the now-current depth and draws whatever turned out visible.

**The safety property is worth stating explicitly: phase 1 may be arbitrarily
wrong in the reject direction — reprojection error, a camera cut, a viewport
resize, a disocclusion — with no visible consequence, because only a phase-2
reject is final.** Every design question ("is this conservative enough?")
reduces to: *does this reject reach phase 2 and get re-tested against a correct
current-frame pyramid?* If the phase-2 pyramid fails to build, phase 2 must run
with occlusion DISABLED so every reject is drawn — a reject nobody re-tests is
exactly the hole the scheme must not have.

## Ordering: the retained pyramid is consumed, then destroyed, in place

`Renderer3D::BuildCurrentOcclusionHZB` regenerates `s_Data.OcclusionHZB` **in
place** — it overwrites the very texture phase 1 reads. So any consumer of the
retained pyramid must read it *before* anything rebuilds it that frame. In the
Deferred graph that is why `VirtualGeometryPass` is registered before
`DeferredGPUOcclusionPass` (`RenderPipelineBuilderScene.cpp`), and why
`Renderer3D::GetRetainedOcclusionHZB()` carries an ordering warning in its
declaration. A new pass that wants previous-frame depth has to be placed with
this in mind; there is no second pyramid to fall back on.

## Phase 2 needs its own command region, not an append

The phase-1 draws have already been issued by the time phase 2 culls. Appending
phase-2 survivors to the same per-instance command segment therefore makes the
second `glMultiDrawElementsIndirectCount` **re-issue every phase-1 draw** (double
raster, doubled overdraw counters) — the count word is absolute and there is no
"start at draw N" parameter.

The fix that keeps everything CPU-addressable: size the command / visible / args
buffers for **two regions** and offset phase 2 by a constant the CPU already
knows — the frame's cluster count for commands+visible, the frame's instance
count for args (`u_CommandSlotBase` / `u_ArgsSlotBase` in the cull shader). Both
are known before the dispatch, so the second MDI can name its region without a
GPU readback. Each cluster is emitted by at most one phase, so the second region
is never more than a duplicate worst case. Pinned by
`VirtualClusterTwoPhaseOcclusion.PhaseCommandRegionsAreDisjointAndInBounds`.

Corollary for the stats readback: anything summing the args buffer must read
**both** regions. Reading only `[0, n)` silently under-reports every
disocclusion-recovered cluster.

## What can safely be left out of the phase-2 pyramid

The phase-2 pyramid is built from the framebuffer depth after the phase-1
*hardware* draws. Software-rasterized clusters live in the visibility buffer
until the resolve pass runs, so they are **not** in it. That is fine — a weaker
occluder set only ever under-culls, and under-culling is free of holes. It is
also why the software rasterizer can run once, after both cull phases, over the
union work list: one dispatch, one resolve, no extra cost versus single-phase.

The same reasoning covers per-sample MSAA (the multisample G-Buffer is not
resolved until the end of the pass) and a mid-frame viewport resize.

Watch the asymmetry it creates, though: phase 1's pyramid *does* contain last
frame's software-rasterized clusters (the retained pyramid is built after the
resolve), phase 2's does not. So in a settled, static scene with the software
rasterizer on, `Phase2Recovered` sits at a small non-zero number rather than 0 —
that is the asymmetry, not a bug. With `swRasterMode=disabled` both pyramids see
the same set and it drops to exactly 0. Useful as a self-check.

## Expect the drawn-cluster count to fall even with nothing in front

Once phase 1 tests a pyramid that contains the object itself, an object
self-occludes: the far side of a closed mesh is now culled by its own front
surface. That is correct and invisible, but it **breaks any pre-existing test
that asserted "occlusion on must not reduce the drawn-cluster count"** — such a
test encoded the old, VG-blind behaviour. Re-anchor those assertions on
**pixels**, which is the real contract; keep only a floor on the cluster count to
catch a total collapse.

## Proving it: pixels, not counters — and measure the noise floor first

Counters prove the cull is doing work; only pixels prove it is not punching
holes. Both are required, and the pixel comparison has a trap.

A live occlusion-on/occlusion-off diff on a virtual-geometry scene came back at
**1.24% of pixels changed, 300x the frame-to-frame noise floor** — which reads
exactly like an over-cull. It was not. The compute software rasterizer resolves
depth by `atomicMin` on a packed word and its bit-identical-depth ties **race
benignly** (either triangle is a legitimate winner); its payload encodes the
`swList` record index, so changing the culled set reshuffles the record ordering
and flips which triangle wins each tie. The result is a thin outline of
single-pixel differences tracing every silhouette — no missing geometry anywhere.

The falsifiable check that settled it in one step: **re-run the A/B with
`swRasterMode=disabled`.** The diff collapsed from 1123 pixels over threshold to
10 (noise floor: 5). A real over-cull would have survived that switch, because
the hardware path has no such nondeterminism.

Generalise: before attributing a pixel diff to the change under test, (1) measure
the noise floor from two captures with *nothing* changed, and (2) find a switch
that should remove the suspected benign mechanism and confirm the diff goes with
it. See also [live-verification-noise-floor.md](live-verification-noise-floor.md).

The automated form of the same verification, worth copying: sweep the camera
through several poses **one frame per pose** — so every pose renders against a
pyramid from a *different* viewpoint, which is the reprojection stress case — and
compare each pose against the identical sweep with occlusion off. A
stationary-camera test cannot see over-culling at all, because there the two
pyramids agree. (`VirtualGeometryVisualEvidence.TwoPhaseOcclusionMatchesTheUnculledFrameWhileTheCameraMoves`,
five poses including a 180-degree jump.)

## The A/B lever

`olo_renderer_settings_set { setting: 'hzbocclusion', value: 'on'|'off' }` toggles
the culling live over MCP (added by #682 — before it, A/B-ing either two-phase
cull meant a rebuild). Pair it with `olo_virtual_geometry_stats`, whose
`phase2Recovered` field reports the disocclusion set: a count that never leaves 0
while the camera moves means phase 1 is not rejecting anything, i.e. the pyramid
is missing or occlusion is off.

Note that toggling the lever off invalidates the retained pyramid, so the first
frame after turning it back on is frustum-only — capture at least two frames.

## Guards

- `VirtualClusterTwoPhaseOcclusionTest.cpp` (shaderpipe, CPU): the phase-1/phase-2
  decision model, the previous-transform reprojection, the camera-cut recovery,
  and the two-region command/args arithmetic.
- `VirtualGeometryVisualEvidence.TwoPhaseOcclusionCullsVirtualGeometryBehindVirtualGeometry`
  (GPU, SKIPs headless): the issue's row-of-statues scene — counters drop, DAG cut
  unchanged, pixels identical, and the frame right after the occluder moves away
  matches an occlusion-off reference (the one-frame-pop check).
- `VirtualGeometryVisualEvidence.TwoPhaseOcclusionMatchesTheUnculledFrameWhileTheCameraMoves`
  (GPU, SKIPs headless): the moving-camera multi-angle sweep described above.
- `McpRendererSettingsApply.HZBOcclusionTogglesTheLeverAndReportsPrior`: the MCP lever.
