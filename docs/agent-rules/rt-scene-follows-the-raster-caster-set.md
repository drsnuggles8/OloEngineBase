# The ray-traced scene follows the raster caster set

**A producer that stages geometry for the TLAS gives an instance the raster tier does not cast the
no-shadow-cast mask (`RayTracing::kVisibilityMaskNoShadowCast`), and stages nothing at all when no
ray but a shadow ray would read it.** A shadow ray must not be stopped by what the shadow maps skip.
Under a fail-closed budget, a non-caster that overruns it switches off every ray-traced shadow in the
scene.

## What happened (#1533)

The showcase dog's lawn is a 60 plants/m² foliage layer with `CastShadows: false`: a 1,257-triangle
tuft within 6 m and cards out to 24 m. The raster tier never cast it. The ray-traced vegetation
producer (`FoliageRenderer::QueueRayTracing`) ignored the flag and queued every group. That was
about 6,800 tufts: 9.4M vertices and 385 MiB per refresh against a 64 MiB, 1M-vertex budget.

`VegetationPolicy` refuses work fail-closed, and `RayTracingScene` withholds the whole TLAS while
vegetation is incomplete. So with the RT shadow tier armed, every technique reported `noData`. The
dog's ray-traced coat shadow (#1253) never ran, and nothing in the frame pointed at the lawn: the
counters read 617 groups requested and 493 refused, which looks like a budget to raise.

Raising the budget would have been the wrong fix. It would have paid every frame for geometry that
no ray reads.

## The rule, applied

- `VegetationSurfaceInput::CastShadows` carries the layer's flag. The cache stages a non-casting
  group's instances with `kVisibilityMaskNoShadowCast`. Shadow rays (`kInstanceMaskShadowCaster`:
  the RT shadow tier and ReSTIR DI) pass through them, and reflection rays still hit them.
- `VegetationPolicy::TracesLayer(castsShadows, reflectionsReadVegetation)` keeps a non-casting
  layer out of the queue unless RT reflections read vegetation this frame, since nothing else would.
- Mesh producers already followed this convention
  (`castsShadow ? GPUSceneInstanceInput{}.m_VisibilityMask : kVisibilityMaskNoShadowCast` in
  `Renderer3DGPUScene.cpp`). Vegetation was the producer that did not.

Tests: `VegetationPolicy.ALayerThatCastsNoShadowIsTracedOnlyForReflections` (the gate) and
`RayTracingDevice.ANonCastingVegetationGroupIsStagedOutOfTheShadowCasterLane` (the staged mask, read
off the GPU Scene instances the TLAS is built from).

## Checking a new producer

1. Find the raster flag that keeps it out of the shadow maps, and map it to the mask lane.
2. List which rays read it. If only shadow rays do, a non-caster costs acceleration-structure memory
   and budget for no ray, so stage nothing.
3. Arm the RT shadow tier in a scene with a large non-casting layer, then read `olo_rt_scene_stats`.
   You want `vegetation.requested` at 0 and a built TLAS, not refusals.

## Not solved here

With RT reflections on, the non-casting lawn is queued for the reflection rays. At its density it
still overruns the vegetation budget and withholds the TLAS. That is the budget's fail-closed design
meeting a 9.4M-vertex near field, and fixing it needs a ray-space level of detail for authored plants,
not a mask.
