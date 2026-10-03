# Charge a ray-tracing budget where it is enforced

**When a backend enforces a limit by dropping work, the producer that feeds it charges that same
limit, in the backend's own units, before it queues anything.** A limit that only the backend
enforces gets enforced every frame, by dropping the same work again. The ray-traced scene withholds
the TLAS until every vegetation build lands, so a build the backend drops forever withholds the TLAS
forever.

## What happened (#1533)

With RT reflections on, the showcase dog's lawn (216k plants within 45 m, `CastShadows: false`)
went into the ray-traced scene as authored tufts: 1,257 triangles each, about 60 KB of staged
geometry per plant. Three limits stood between it and a TLAS, and the producer
(`FoliageRenderer::QueueRayTracing` with `VegetationSurfaceCache`) charged only the first:

| Limit | Enforced by | What it did to the lawn |
|---|---|---|
| 64 MiB staged geometry | the cache, at `Queue` | refused 493 of 617 groups; every refusal withheld the TLAS |
| per-frame build budget (`VegetationFrameBudget`) | the backend, in geometry order | 37 builds requested a frame, 13 recorded, for good |
| 64 MiB of vegetation acceleration structures | the backend, by skipping the build | 26 tuft groups filled it; the rest were dropped every frame |

Each fix exposed the next limit. Once groups were admitted nearest first instead of refused, the
build budget showed: the producer spent all of it on refreshes, so the backlog left by warm-up never
drained. Once the dropped builds were charged back, 8–9 builds were still dropped every frame. The
storage cap was rejecting them, and charging them back again could never make room.

## The rules, applied

1. **The producer charges every limit the backend enforces, in the backend's units.**
   `RayTracingScene::EstimateDeformedBlasBytes` returns the device's own size for a structure that
   does not exist yet (`vkGetAccelerationStructureBuildSizesKHR` ignores addresses). Each group
   carries `AccelerationBytes` and the cache refuses at `Queue` against `AccelerationStructureBytes`.
   `RayTracingDevice.TheCacheBudgetsTheDevicesOwnAccelerationStructureSizes` pins the estimate to the
   byte against a real build. The per-frame budget charges one build per part, each counting the
   group's whole vertex stream, because that is how the backend charges them.
2. **What the backend could not record comes back as a debt.** `RayTracingScene::GetVegetationBuildDebt`
   reports the unrecorded vegetation builds. The pass hands them to the cache, and the next frame's
   budget pays them before it refreshes anything (`carriedBuilds`).
3. **Optional work never takes budget from mandatory work.** Casting groups are served first. A
   reflection-only group whose refresh does not fit keeps its previous snapshot
   (`staleReflectionSnapshots`). A casting group in the same position is refused, because shadow rays
   must match the raster shadow.
4. **One TLAS, two readiness rules.** `GetShadowTlasDeviceAddress` needs every casting group
   (`castersComplete`); `GetTlasDeviceAddress` needs every queued group. RT shadows and ReSTIR DI read
   the first, so a gap in the lawn cannot switch off shadows the lawn plays no part in.
5. **Reflection-only layers trace cards first and the mesh nearest.**
   `VegetationPolicy::ChooseReflectionTiers` admits every group as cards (one quad per plant, the
   mesh's bake), nearest first, while geometry and structures fit. It then upgrades the nearest groups
   to the authored mesh while the difference fits. The rest are left out and counted
   (`beyondReflectionBudget`), not refused.
6. **Per-frame cost follows what changed, not how many plants there are.** A group the cache holds
   under its `ContentKey` sends no rows (`HoldsContent`). The registry's `GetRecordsEpoch` pins
   pointers into its records. A thousand groups are cheap only if no step does a hash lookup, a byte
   loop or a heavy copy per plant.

## Measured

Vulkan, RTX 4090, the dog scene in Deferred with RT reflections and RT shadows, still camera:

- Before: `noData`, so no ray-traced pass ran. RT shadows fell back to shadow maps
  (`rayTracedLights` 0).
- After: `ready`, with 1,047 TLAS instances and all 215,989 plants represented. Reach is 35–43 m as
  cards and about 0.9 m as tufts. Structures take 61.4 MB of the 64 MiB, nothing is refused or
  dropped, and nothing is carried. `rayTracedLights` is 1.
- GPU: 11.13 ms against 11.19 with RT shadows alone. `RayTracingScenePass` costs 0.31 ms and
  `RayTracedReflectionPass` 0.21 ms.
- CPU: once the lawn was in, reflections cost 42 ms a frame on top of RT shadows (a 64 ms frame
  against 20). Four fixes brought that to 4.9 ms (22.0 against 17.1, `olo_perf_cpu_scopes`):

  | Scope | Before | After | Fix |
  |---|---|---|---|
  | `RayTracing::SceneUpdate` | 13.8 ms | 0.8 ms | `GeometryKeyHash` mixes its bits. `Slot << 32 ^ Generation` left only the generation in the low bits that pick a bucket, so a thousand structures shared one bucket. |
  | `GPUScene::Upload` | 4.7 ms | 0.07 ms | The tables are `DynamicDrawExactUpload`. As `DynamicDraw`, Vulkan's draw snapshot filled the unused capacity by reading the write-combined mapping back. |
  | `Vegetation::FinishExtraction` | 9.4 ms | 1.1 ms | The state hash mixes 8 bytes a step instead of 1, and the inputs sort as an index instead of as kilobyte structs with two map lookups per comparison. |
  | `Vegetation::Queue` | 16.6 ms | 2.8 ms | Groups carry record indices (no hash lookup per plant), unchanged groups send a content key instead of rows, and the reflection-only split is reused while the camera stays within 0.25 m. |

  The first two speed up every Vulkan scene with many structures, not only this one. A snapshot that
  fills more than 4 KB from uncached mapped memory now logs once per binding.

## Checking a producer against its backend

List every limit the backend enforces and how: a refusal, a dropped build, a skipped allocation.
For each one, find where the producer charges it. Then run the heaviest scene and read
`olo_rt_scene_stats`. You want `availability.status` ready, `carriedBuilds` at 0 at a still camera,
and no `BLAS builds could not be recorded` line in the log. A backlog that stays at the same size
from frame to frame is a limit nobody charges.
