# Self-shadowing a coat bound to a moving body (#1426)

Read before touching the deformed arm of `GroomRenderPass::AcquireCoatVolume`, `GroomCoatShadow::{BuildCoatSegmentsFromStrandVertices, CaptureCoatPose, MaxCoatPoseDrift, CoatRebakePolicy}`, or `GroomCoatShadowFallbackReason::{DeformedPoseUnavailable, RepresentationStale}`. The static coat's rules are in [groom-coat-self-shadowing.md](groom-coat-self-shadowing.md); this file adds the moving case.

## Rules

1. **Bake a bound coat from the strands the pass DRAWS, in groom object space.** `AcquireDrawnPose` hands the drawn centrelines to `AcquireCoatVolume`; the bake reads them, not the asset's rest curves (on the CPU reference path: corners 0 and 2 of the rebuilt stream). Volume and strands then share one space and `u_GroomCoatWorldToObject` needs no change. Do not use bind space (needs per-fragment inverse skinning, wrong density) or world space (every rigid move invalidates the bake). [Details](groom-deformed-coat-self-shadowing-details.md#the-rules)
2. **At rest, the drawn stream and the rest curves must bake the same coat.** `TheDrawnStreamAtRestIsTheSameCoatAsTheRestCurves` compares voxel by voxel. The mesh `Radius` is already halved and carries the coat width; the only lever still applied is `WidthScale`. Halving again is the thinner-coat error. [Details](groom-deformed-coat-self-shadowing-details.md#the-rules)
3. **Rebake on drift, not on a frame count.** Drift is the largest centreline-midpoint displacement since the bake, in voxels of the volume in force (`MaxCoatPoseDrift`, `CoatDriftInVoxels`); the default policy rebakes past 0.5 voxels. On a walk expect a rebake every frame; the bound saves nothing there. [Details](groom-deformed-coat-self-shadowing-details.md#the-rules)
4. **Past the stale bound (2 voxels) do not sample the volume.** Report `RepresentationStale` and read the coat fully lit (rule 10 of the static guide); a lit coat visibly says "this did not run". Reachable only when a rebake failed or `RebakeOnDrift` is off. [Details](groom-deformed-coat-self-shadowing-details.md#the-rules)
5. **An incomparable pose is infinitely far.** A different segment count or a non-finite point returns +infinity, never zero; sanitise the policy on entry, since a NaN bound compares false against every drift and freezes the bake. [Details](groom-deformed-coat-self-shadowing-details.md#the-rules)
6. **`DeformedPoseUnavailable` fails closed.** `GroomCoatShadowInputs::DeformedPoseAvailable` defaults to false, so a deformed groom must supply its pose positively (GPU path: from the frame buffer the vertex shader reads; CPU path: from its rebuilt stream). [Details](groom-deformed-coat-self-shadowing-details.md#the-rules)
7. **Never `SetData` into a volume a recording still to be submitted can read.** Each coat keeps a three-slot ring and rewrites only a slot not bound on this tick or the one before; a slot of the wrong size gets a new texture. See [groom-coat-bake-cost.md](groom-coat-bake-cost.md). [Details](groom-deformed-coat-self-shadowing-details.md#the-rules)
8. **Scale a card-tier bake to the coat's fibre area** (#1428); see [groom-card-coverage.md](groom-card-coverage.md) rule 2. [Details](groom-deformed-coat-self-shadowing-details.md#the-rules)
9. **Replay to the same frame before every arm of an A/B.** A capture's settle frames advance the walk; two back-to-back captures are two poses. [Details](groom-deformed-coat-self-shadowing-details.md#things-that-will-bite)
10. **Compare a posed volume against the reference, not against another volume.** At a different grid alignment two volumes differ by quantisation (about 0.07 mean |dT| at 48^3 at rest). Count the pose bake as excess over the at-rest error. [Details](groom-deformed-coat-self-shadowing-details.md#things-that-will-bite)
11. **Swing a negative control perpendicular to the light.** Displacement along the light lets the bind-pose answer come out nearly right by accident. [Details](groom-deformed-coat-self-shadowing-details.md#things-that-will-bite)
12. **Keep `AcquireDrawnPose` clearing the pose first, and `AcquireGeometry` clearing the CPU path's stream first,** so a groom never receives the previous groom's pose. [Details](groom-deformed-coat-self-shadowing-details.md#things-that-will-bite)
13. **Do not re-add the rigid-offset lookup for walking coats** without a measurement: on the walk it changed 176 rebakes into 177, the drift being non-rigid. [Details](groom-deformed-coat-self-shadowing-details.md#measured)
14. **Judge the acceptance lever by luma direction, not pixel count.** `ActiveVisualLeversChangeTheMovingLongCoat` asserts the luma direction, because a pixel count scores a darkening-only term on the same scale as stochastic re-dithering. [Details](groom-deformed-coat-self-shadowing-details.md#measured)

## Where the evidence lives

- `OloEngine/tests/Rendering/PropertyTests/GroomCoatShadowDeformedPropertyTests.cpp`: the space, the bound and the cadence.
- `OloEngine/tests/Groom/GroomCoatShadowSelectionTest.cpp`: the decision table.
- `GroomAnimalsAcceptanceEvidenceTest` cases `TheMovingCoatShadowFollowsTheWalk`, `TheMovingCoatIsSelfShadowedThroughTheWalk`, `EveryChildIntegratesOnTheMovingSubjectsOnEveryPath`.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[groom-deformed-coat-self-shadowing-details.md](groom-deformed-coat-self-shadowing-details.md).
