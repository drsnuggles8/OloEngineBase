# Plan a complete tier before a budget withholds the TLAS

**When a ray-tracing producer meets a limit, plan a cheaper COMPLETE representation, a bounded hold or
an explicit counted fallback before it publishes; never let one group's overflow switch every
ray-traced effect off.** Name the pressure source, report requested against realized quality and
the recovery cost, and come back from a fallback only after a stable run.

## What happened (#1354)

After #1533 the reflection-only lawn was planned (cards nearest first, the mesh nearest of all), but
a vegetation layer that CASTS was still all-or-nothing. One casting group refused for staged
geometry, acceleration-structure bytes, the 4096-group cap or the per-frame build budget set
`CastersComplete` false. That withheld both TLAS addresses, so RT shadows, ReSTIR DI, reflections,
ReSTIR GI and the path tracer all went dark for the frame. Every one of them then reported
`AccelerationStructureEmpty`, which is not what had happened. Readiness was re-decided every frame,
so demand at the budget could flip all of them on and off frame to frame. A camera cut re-slices
every group, and the cut frame's full refresh was not charged against anything until the cache
refused it.

## The rules, applied

1. **Plan casters like reflection-only groups, but complete.**
   `VegetationPolicy::ChooseCastingTiers` admits every casting group at its cheapest complete tier,
   nearest first. For a mesh layer that tier is its card, the raster's own LOD past the mesh
   distance. An impostor layer has no card tier: a quad with the layer albedo is not that plant. The
   plan then upgrades the nearest groups to the requested mesh while it fits. A group that fits at no
   tier is OUT: refused, counted under its pressure, and the casters are incomplete. That last case
   is the explicit technique fallback.
2. **Charge work at its steady rate; leave the cut frame to the cache.** A tier's cost is its memory
   plus its refresh in the backend's units (one build per part, each over the group's whole vertex
   stream), times how often it refreshes: every frame when detailed, frame time over the proxy
   deadline otherwise (`VegetationPolicy::RefreshRate`). The first version charged a full refresh
   every frame. That withheld the TLAS for good in the IntegratedRenderer meadow: 1,681 casting
   groups make more builds than a frame has, although the staggered proxies fit easily. A camera cut
   makes every group new at once. The cache builds what fits, refuses the rest under `frameWork`,
   and the hysteresis brings the TLAS back once the scene has warmed up.
3. **Reduced cadence only within a declared bound.** A group whose refresh does not fit holds its
   snapshot under `VegetationPolicy::CanHoldSnapshot`. A caster gets the proxies' deadline (shadow
   error 0.25 m). A reflection-only group gets the looser reflection bound (1 m, 1 s). Past either
   bound the snapshot is obsolete: a caster is refused, and a reflection-only group is deferred
   (left out for the frame and counted, `deferredReflectionGroups`), never published as current.
4. **Hysteresis at both levels.** A tier upgrade a group did not hold last frame needs 1/16 of every
   budget left over. A withheld TLAS comes back after `RecoveryFrames` (8) consecutive complete
   frames (`VegetationPolicy::Recovered`), so flickering demand degrades the technique once.
5. **Name what happened.** `VegetationPressure` names each source. The RT techniques report
   `AccelerationStructureWithheld`, distinct from `AccelerationStructureEmpty`.
   `olo_rt_scene_stats.vegetation` reports `pressure`, `pressureGroups`, the casting plan
   (`castingFallbackGroups`, `castingGroupsLeftOut`, `nearestCastingFallback`), `cadenceHolds`, the
   oldest published snapshot's age and error, `recovery` and the hysteresis counters.
6. **Reset the histories the representation feeds, when the representation changes.** The cache
   signs the traced set as the sum of `VegetationPlantTerm(id)` times a per-tier odd factor, which
   does not depend on how plants are grouped. A camera move that re-slices plants at the same tier is
   not a change. A tier switch, plants entering or leaving, a plant edit (the registry generation is
   in the factor), a parameter edit or a wind discontinuity is a change, and it invalidates the RT
   shadow, ReSTIR DI and ReSTIR GI histories. TAA, which accumulates the unchanged raster frame, is
   not invalidated.
7. **Never build over a stream no dispatch wrote, and hold back only that stream.** A producer that
   hands out a new content revision before its dispatch is recorded must name the streams whose
   dispatch did not land. `DeformedSurfaceCache::GetUntrustedOutputs` lists them for skinned
   surfaces, as `ProducerFailed` does for vegetation, and `RayTracingScene::Update` keeps their
   previous structures while every other character builds.
8. **Plan on the clock the cache ages snapshots by, measured by the planner.** The plan's refresh rate
   is the step since its own last plan, on the clock each layer's wind runs on. It is not the scene's
   `prevAnimationTime`, which a throttled editor re-seeded every frame (see
   [animation-clock-survives-unrendered-ticks.md](animation-clock-survives-unrendered-ticks.md)), and
   a clock that stands still keeps its last real step.

## Measured

MEASURED_PLACEHOLDER

## Checking a producer for a quality cliff

For each limit, ask what one group over it costs. If the answer is "every ray-traced effect", the
limit needs a complete lower tier, a bounded hold or a counted deferral first. Then stress it:
`olo_rt_vegetation_diagnostic budgetDivisor=N` makes every vegetation budget 1/N of itself, and
`forceDetailed=true` is the complete high-quality reference. Read `olo_rt_scene_stats`. Under
pressure you want `pressure` named, `tlasWithheld` false while every caster has a tier,
`fallbackEngagements` not climbing frame by frame, and `oldestSnapshotError` at or below the bound.
