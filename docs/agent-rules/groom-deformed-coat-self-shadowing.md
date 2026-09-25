# Self-shadowing a coat bound to a moving body (#1426)

Read before touching the deformed arm of `GroomRenderPass::AcquireCoatVolume`,
`GroomCoatShadow::{BuildCoatSegmentsFromStrandVertices, CaptureCoatPose, MaxCoatPoseDrift,
CoatRebakePolicy}`, or `GroomCoatShadowFallbackReason::{DeformedPoseUnavailable,
RepresentationStale}`. The static coat's rules are in
[groom-coat-self-shadowing.md](groom-coat-self-shadowing.md); this file adds the moving case.

## The rules

1. **Bake a bound coat from the strands the pass DRAWS, in groom object space.**
   `AcquireDrawnPose` hands the drawn centrelines to `AcquireCoatVolume`, and the bake reads them
   instead of the asset's rest curves. Since #1427 a bound coat is deformed in the vertex shader, so
   the pose is evaluated on the CPU from the same frame buffer the GPU reads
   ([groom-gpu-strand-deformation.md](groom-gpu-strand-deformation.md) rule 8); on the CPU reference
   path it is corners 0 and 2 of the rebuilt stream. Those vertices are already in groom object space with the
   pose applied, and they reach the screen through the same model matrix the march inverts. So the
   volume and the strands share one space, and the shader lookup (`u_GroomCoatWorldToObject`)
   needs no change at all.

   The two alternatives the issue named, and why they lost:

   - **Bind space.** Every shading point would have to be mapped back to the bind pose, which
     means per-fragment inverse skinning the strand shader has no data for. It is also the wrong
     density: a folded leg compresses its fur, and a bind-space volume cannot describe that.
   - **World space.** Every rigid move of the entity would invalidate the bake, which throws away
     rule 7 of the static guide (an animated light or a moving entity costs zero rebuilds).

   Baking from the drawn stream also picks up everything the draw has and the rest curves lack:
   the LOD tier, the coat authoring's per-strand width and the guide simulation's displacement.

2. **At rest, the drawn stream and the rest curves bake the same coat.** Binding a groom must not
   change its shadow while nothing moves. `TheDrawnStreamAtRestIsTheSameCoatAsTheRestCurves`
   compares the two volumes voxel by voxel. The mesh `Radius` is already halved and carries the
   coat width, so the one lever still applied is `WidthScale`. Halving again is the thinner-coat
   error.

3. **Rebake on DRIFT, not on a frame count.** Drift is the largest centreline-midpoint displacement
   since the bake, in voxels of the volume in force (`MaxCoatPoseDrift`, `CoatDriftInVoxels`). The
   default policy rebakes past **0.5 voxels**. A drift bound caps the lag at a stated number
   whatever the animation does, and a coat that stops moving costs nothing. A frame count caps
   only the cost: the lag is whatever the animation covers in N frames, and an idle coat keeps
   paying. **On a walk, expect a rebake every frame** (Measured, below): the bound saves nothing
   there, and the cost is #1427's to remove.

4. **Past the stale bound (2 voxels) the volume is not sampled.** The decision reports
   `RepresentationStale` and the coat reads fully lit, per rule 10 of the static guide. A coat
   shadowed by where its strands used to be looks plausible; a lit one visibly says "this did not
   run". This is only reachable when a rebake failed or `RebakeOnDrift` is off. The negative
   control turns the rebake off on purpose to show the detector fires.

5. **An incomparable pose is infinitely far.** A different segment count (a LOD hand-over rebuilt
   the curve set) or a non-finite point returns +infinity, never zero. Otherwise a bake of the
   wrong curve set would be kept forever. The policy is sanitised on entry: a NaN bound would
   compare false against every drift and freeze the bake.

6. **The one refusal left is `DeformedPoseUnavailable`, and it fails closed.**
   `GroomCoatShadowInputs::DeformedPoseAvailable` defaults to false, so a deformed groom has to
   supply its pose positively. Both deformation paths supply one since #1427: the GPU path
   evaluates it from the frame buffer the vertex shader reads, the CPU reference path reads it off
   its rebuilt stream. The arm is reached only when neither produced a pose.

7. **A new 3D texture per bake, never `SetData` into the resident one.** A draw in flight may still
   be sampling the old volume, and an in-place upload into an image a queued Vulkan frame reads is
   a hazard, not a copy. The bake's dimensions also move with the pose.

## Measured

**The cadence sweep** (`ADriftBoundHoldsItsLagAndCostsNothingWhileStill`, CPU, deterministic): a
3000-strand pelt whose +x half swings ±3 cm over a 60-frame cycle, then holds for 30 frames, at 32³.
Lag is the mean |ΔT| on limb probes against a fresh bake of the same frame.

| policy | bakes (of which still) | mean \|ΔT\| | max \|ΔT\| | worst drift in use |
|---|---|---|---|---|
| drift 0.25 vox | 40 (0) | 0.0076 | 0.31 | 0.25 vox |
| **drift 0.5 vox** (default) | **20 (0)** | **0.0313** | 0.43 | **0.48 vox** |
| drift 1.0 vox | 8 (0) | 0.0671 | 0.73 | 0.95 vox |
| drift 2.0 vox | 4 (0) | 0.0969 | 0.61 | 1.81 vox |
| every frame | 90 (30) | 0 | 0 | 0 |
| every 2 | 45 (15) | 0.0104 | 0.30 | 0.32 vox |
| every 4 | 23 (8) | 0.0246 | 0.58 | 0.96 vox |
| every 8 | 12 (4) | 0.0484 | 0.71 | 2.14 vox |

**What the table does NOT show:** that a drift bound has a smaller *worst* error than a frame count
at the same cost. The first version of the test asserted it and the numbers refuted it. The worst
|ΔT| is set by single probes whose march crosses a voxel boundary as the grid re-quantises, which
happens under either policy. The choice rests on rule 3's two properties, and the test asserts
those.

**The real walk** (`TheMovingCoatShadowFollowsTheWalk`, GL, **Debug**, two walking horses of 57k
and 94k strands and a 44k-strand turning head, 59 frames after warm-up):

| policy | rebakes per frame (of 3 coats) | bake CPU per frame, mean / worst | worst drift sampled | stale |
|---|---|---|---|---|
| drift 0.25 vox | 3.00 | 566 / 588 ms | 0.00 vox | 0 |
| **drift 0.5 vox** (default) | **3.00** | **583 / 838 ms** | 0.00 vox | 0 |
| drift 1.0 vox | 2.83 | 556 / 686 ms | 1.00 vox | 0 |
| drift 2.0 vox | 2.12 | 468 / 589 ms | 1.99 vox | 0 |
| frozen (negative control) | 0 | 0 | — | 170 refusals, min shadowed 0 |

**On a walking animal the bound does not save anything, and that is the animation, not the
policy.** Leg and lower-leg fur is about 40% of the long coat, and a walking hoof moves several
centimetres a frame at 60 Hz, which is more than a voxel of a 64³ volume over a horse. So every
coat rebakes every frame at any bound tight enough to hold the lag under a voxel. The bound pays
off where coats are still or slow: idling, breathing, a head turning. It holds the lag at a stated
number in either case.

**Tried and removed: carrying the rigid part of the motion as a lookup offset.** Removing the
coat's mean displacement before measuring drift is exact for a coat moved whole (the rigid
contract test), but on the walk it changed 176 rebakes into 177. The drift is non-rigid. Do not
re-add it for walking coats without a measurement that says otherwise.

The bake cost above is a Debug build's, timed around the whole bake (segments, binning, packing,
upload call); how it splits between those was not measured. It sat
beside #1427's per-frame strand rebuild of the same coats (420 ms/frame Release for three coats,
from that issue). #1427 removed the rebuild and feeds the bake from the GPU path's own frame buffer,
so the bake is now the largest remaining per-frame groom cost (57-109 ms live, Release, for the three
walking coats). This slice measures the cost and does not optimise it; the optimisation is #1445.

**On Vulkan, a bound coat larger than 16 MiB was not drawn at all (#1446) — resolved.** That was
the per-frame vertex re-upload overflowing the frame arena, not this bake. #1446 made large vertex
streams command-ordered transfers, and #1427 removed the per-frame stream altogether. #1426's live
Vulkan check predates both, which is why it ran the counters on all three coats and the pixel A/B
on a coat thinned to fit.

**The acceptance lever** (`ActiveVisualLeversChangeTheMovingLongCoat`, Forward): before #1426
switching #1248 off changed 2.32% of the moving long coat's frame against a 2.23% repeat floor,
so the lever was disconnected. Now it changes 3.96% against 2.35% (1.69×), and brightens the frame
by +101 267 summed luma against a repeat drift of 1 035 (98×). The case asserts the luma direction
for this lever. A pixel count scores a term that only darkens on the same scale as a stochastic
coat re-dithering in both directions, so it undersells the term.

## Things that will bite

- **A capture's settle frames advance the walk.** Two captures taken back to back are two poses,
  whatever the fixture's `Capture` comment says. Replay to the same frame before every arm of an
  A/B, as the lever case does. The first version of the motion-capture case did not, and measured
  a 109k-pixel "repeat floor" that was the legs moving.

- **Compare a posed volume against the reference, not against another volume.** At a different
  grid alignment, two volumes differ by their quantisation. At 48³ the volume is already ~0.07 mean
  |ΔT| off the exact answer at rest (the static guide's rule 4 bias). The first version of the limb
  case compared volume with volume and could not tell a pose bake from a bind-pose bake (0.055
  against 0.063). Against the reference, and counted as excess over the at-rest error, the pose
  bake adds 0.013 and the bind-pose bake 0.134.
- **A negative control displaced ALONG the light hides the error it controls for.** The probe
  slides along its own ray, and the bind-pose answer comes out nearly right by accident. Swing
  perpendicular to the light.
- **`AcquireDrawnPose` clears the pose first, and `AcquireGeometry` clears the CPU path's stream
  first**, so an undeformed groom, or a deformed one that built nothing, can never hand the bake the
  previous groom's pose.

## Where the evidence lives

- The space, the bound and the cadence:
  `OloEngine/tests/Rendering/PropertyTests/GroomCoatShadowDeformedPropertyTests.cpp`.
- The decision table: `OloEngine/tests/Groom/GroomCoatShadowSelectionTest.cpp`.
- The moving coat, on every GL path and under MSAA and upscale:
  `GroomAnimalsAcceptanceEvidenceTest.{EveryChildIntegratesOnTheMovingSubjectsOnEveryPath,
  ActiveVisualLeversChangeTheMovingLongCoat, TheMovingCoatShadowFollowsTheWalk,
  TheMovingCoatIsSelfShadowedThroughTheWalk}`, which write
  `OloEditor/assets/tests/visual/GroomAnimalsMovingCoatShadow[Off]_GL_<Path>_LongCoat_F<frame>_<Angle|Cell>.png`.
