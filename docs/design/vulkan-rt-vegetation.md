# Vulkan ray-query vegetation

Build ray geometry from canonical plants and the shared raster wind producer;
never trace a rest-pose canopy or replace a tree by a main-view-facing card.

`FoliageRenderer::QueueRayTracing` visits stable registry IDs in their 16 m
spatial cells. Authored groups contain at most 32 plants; card groups contain
at most 256. One group owns one deformed vertex stream and concatenated index
stream. Mesh parts share those streams and retain separate index ranges and
albedo materials. Instance transforms remain the terrain transform; deformation
outputs terrain-local positions. The producer evaluates roots in absolute space
using the same render-origin seam as raster wind.

## Selection and approximation

Authored canopies use current wind within `max(50 m, MeshFadeStartDistance)`.
Cards use current wind within 12 m. More distant groups retain a recent snapshot
of the same geometry. A canonical-ID phase staggers refresh buckets. A snapshot
is reusable only within its bucket, with continuous nondecreasing time, and
below both 50 ms age and 0.25 m conservative world-space displacement error.
The deadline is `min(0.05, 0.25 / velocityBound)`; invalid bounds refuse work.
The bound uses sine derivatives, the nonexpansive hierarchical field cap,
sanitized weights, and a terrain matrix operator-norm bound.

The far octahedral raster LOD has no view-independent card silhouette. RT keeps
its authored source canopy and uses the impostor's rigid sway at influence 0.5
(0.15 for legacy wind). It switches at the authored mesh handover end. Within
the raster dither transition, RT retains the authored near representation.
Atlas angular quantization, texture filtering, and raster dither do not become
ray geometry; the 0.25 m temporal error bound applies to the selected physical
canopy's wind, rather than asserting pixel identity with a baked atlas.

## Work, memory, and fallback

Live output/row/index buffers are capped at 64 MiB and 4096 groups. Offered
extraction data is bounded by the same geometry estimate before GPU allocation.
Each frame reserves at most 1024 group updates, 1048576 vertices, and 524288
triangles. AS recording has an independent reservation so retries cannot evade
the work cap. Live vegetation BLAS storage is capped at 64 MiB using actual
driver size queries. Every one of those numbers is sized from the measured
reference fixture below, with headroom over a FULL refresh: a cap under the
full refresh does not throttle the feature, it switches it off. These limits exclude shared source assets, TLAS, the
scene-wide scratch pool, transient dispatch tables, and fence-retired copies;
diagnostics report scene-wide AS and scratch memory separately.

New groups and oldest snapshots receive reservations first, with canonical
identity breaking ties. A refused request does not consume capacity. Backend
per-key acknowledgements commit successful partial builds so warming can
advance. The shader's dispatch counter acknowledges recorded commands; dropped
pipeline/root-data dispatches invalidate their outputs and skip AS building.
Explicit barriers protect previous hit/refit reads and order deformation,
AS builds, and ray reads on the graphics recording queue. Replaced resources
use Vulkan's deferred fence retirement.

If any requested vegetation cannot be represented or updated, hybrid consumers
receive no usable TLAS address and select their existing whole raster fallback.
Forward/Forward+ and OpenGL remain raster paths. Both hybrid consumers confirm
masked candidates through `HybridRayTracingAlpha.glsl`, sampling canonical UVs,
material alpha/cutoff, and the generation-checked raster material heap record.
Unresolved heap maps select raster fallback on CPU.

Representation/parameter changes, removal, reset, and readiness transitions
invalidate shadow/TAA histories. Reflections are part of the TAA color chain.
Paused snapshots reuse geometry; reverse time and discontinuous wind refresh it.
Scene reset clears producer state before canonical records are rebuilt.

## Diagnostics and persistence

Editor statistics and `olo_rt_scene_stats.vegetation` report representation,
reuse, dispatch batches, memory, refusals, history reset, and readiness. Require
both supported RT capability and vegetation readiness when checking evidence.
`olo_rt_vegetation_diagnostic` temporarily forces detailed updates for an A/B
benchmark without changing plants, wind, camera, raster LOD, or budgets. Restore
`forceDetailed:false` afterward. This render-thread diagnostic is not authored
scene, save-game, or asset state; no ECS or scripting field is added.

Existing foliage YAML/save-game/asset schemas remain the authority for plants,
materials, and wind. Runtime output buffers and ASes are rebuilt, never cooked
or serialized. Engine shaders continue to load from the engine shader directory;
`AssetImporter` registers no raw shader/compute serializer. Deployment therefore
needs the new compute shader and its shared includes alongside the existing
engine shaders. A scene asset pack alone does not package engine shaders.

Verification measurements and the execution matrix belong in the validation
report; successful headless ray tests do not establish live editor behavior.

## Measured validation

Windows 11, RTX 4090 (driver 104.256.0, Vulkan 1.4.351), Debug editor, Vulkan
backend, deferred path, `Scenes/FoliageHierarchicalWind.olo` at 960x540.

The reference fixture offers 533 groups and 61,824 plants: 3,274 authored pines
of 112 vertices / 48 triangles and 58,970 cards. A full refresh is 602,568
vertices and 275,092 triangles. Live steady state under moving wind: 533 of 533
groups dispatched in 3 batches, 0 refused, 600,888 vertices deformed per frame,
24.3 MiB of live geometry, 19.2 MiB of vegetation BLAS storage, 1.9 MiB scratch,
533 TLAS instances all classed `deformed`, `ready: true`.

**The per-frame caps must cover a full refresh, not a staggered fraction.** A
snapshot is reusable only while it is inside the 0.25 m error deadline, which is
at most `MaximumProxyAge`; a Debug editor frame here measures 114 ms, longer than
that deadline, so every group legitimately refreshes every frame and reuse is 0.
The first budgets (262,144 vertices) sat at 0.44x of the full refresh, so ~315
groups were refused every frame; refusal is fail-closed, so the TLAS address was
withheld and the hybrid consumers stayed on raster permanently. Budgets sized
below the full refresh do not make the feature slower, they switch it off.

Hybrid shadow evidence, frame paused so wind is the only frozen variable:
ray-traced shadows on versus off changes 299,328 of 518,400 pixels (57.7%, mean
|delta| 8.8) with a repeatability floor of 0 changed pixels. The resolved mask
(`sun-rtmask.png`) shows per-plant conifer silhouettes with alpha-tested ragged
edges rather than card rectangles. The fixture's sun ships `RayTracedShadows:
false`, so the per-light opt-in must be set before the RT shadow tier reports any
light; with it off the pass counts zero ray-traced and zero fallback lights.

At this frame rate detailed and temporal selection cost the same (131.9 ms versus
136.8 ms) because neither can reuse. The `olo_rt_vegetation_diagnostic` override
is what makes that A/B measurable: it reports 533 detailed / 0 proxy groups
against 51 / 482 automatic.

Not yet covered: the full conditional matrix (MSAA, upscale modes, non-native
resolutions, the OpenGL rows) and reuse behaviour at a frame time shorter than
the error deadline, which needs a Release editor.
