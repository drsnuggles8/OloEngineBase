# Parallel recording pass audit (#1013)

Prepare resources and freeze shared inputs before recording concurrently. Keep
GPU dependencies and transparent draw order, and join before publishing frontend
state or running primary-only operations. Preparation is not runtime proof: use
the reported item names and secondary counts to establish actual parallel work.

## Shared replay contract

| State | Ownership inside a region |
|---|---|
| Frame tables, committed GPU Scene records, draw links, shadow handles | Shared, read only |
| Dispatcher bind/material/render-state caches, viewport, camera override | Item |
| Material, camera, animation, terrain, decal, foliage, water and VSM uploads | Item-owned buffer objects, allocated before fork |
| Model instances | Item-owned SSBO, sized for the largest replayed batch before fork |
| Heap offset table, epoch, dirty flag and offset UBO | Item, seeded from the published table |
| Profiler counters and instanced-draw records | Item; publish in item order after join |
| Queries, capture mutation, lazy resource preparation and readbacks | Primary |

## Geometry and compute families

| Family | Partition and ordering | Implementation and correctness evidence |
|---|---|---|
| Scene depth/color | Contiguous sorted packet ranges; clear/cull before fork; queried packets stay inline | Implemented; three-view Vulkan off/on exact pixels and OpenGL scene captures |
| Planar reflection | Reflected-view packet ranges; view-local camera/light state | Implemented; WaterTest above/below on both backends |
| Decal forward/OIT/G-Buffer | Sorted ranges preserve blending and draw-buffer order | Implemented; Vulkan mode readbacks and two-angle live matrix on both backends |
| Forward overlay | Sorted packet ranges with shared scene attachments | Implemented; common replay contracts and inline fallback for small lists |
| Foliage | Layer packet ranges; private foliage/model uploads | Implemented; two-angle live foliage on both backends |
| Water depth/color | Tile packet ranges; depth capture precedes color | Implemented; two-angle live water/reflection on both backends; Vulkan validation clean after separate array fix |
| Overdraw | Additive packet ranges; clear once | Implemented; Vulkan target readback and common replay contracts |
| CSM, atlas and point-face shadows | Cascade or atlas-entry items; clear/transition shared atlas before fork | Implemented; CSM attachment off/on exact pixels, atlas device checks, heavy multi-light scene |
| Terrain/foliage shadow casters | Same shadow-view item; private terrain/foliage/instance uploads | Implemented; live terrain and foliage on both backends; Vulkan VT sample uses fallback shading, as detailed below |
| Virtual-geometry shadow casters | Same view item; private compute parameters and indirect args | Implemented; device checks retained; full Vulkan helmet evidence blocked by existing support described below |
| VSM marking/raster | Prepared marking dispatch; raster ranges with private globals/bones/instances; shared pool uses GPU atomic-min | Implemented; 96-VAO raster readback, worker sampling regression and live GTAO/mark group with zero conflicts |
| DDGI | Per-probe six-face capture/resample; relocation and visibility ranges; ordered atlas publication | Implemented; single-volume and cascade live captures; cascade scene records 16 items with zero conflicts |
| GPU/deferred occlusion | Indirect replay ranges after primary cull | Implemented; Vulkan indirect-replay checks and heavy deferred pipeline |
| Virtual geometry main | Phase-1 instance cull, both hardware raster phases and SW material-resolve ranges | Implemented; primary args reset/HZB/SW raster/export chain; OpenGL helmet captures; full Vulkan evidence blocked below |
| Fluid intermediates | Depth/thickness splat ranges, private fluid UBOs; primary clear and final smoothing parameters | Implemented; OpenGL multi-angle fluid evidence; Vulkan 96-submission depth/thickness comparison, six secondaries and zero conflicts |
| Mesh particles | Contiguous instance ranges, private mesh-instance UBOs | Implemented; Vulkan 96-instance distinct-color comparison, three secondaries and zero conflicts; real OpenGL scene shows all three color ranges from three angles |
| Billboard/trail/GPU particles | Already one draw per batch; shared batch callback remains primary | Existing Vulkan billboard/trail checks; no artificial split of a single draw |
| Ray-tracing scene | Backend batches BLAS requests into one command; TLAS depends on BLAS | Sequential allocation, retirement, compaction queries and statistics; no independent build-command loop remains |
| Shader debug | Seven indirect channel draws, then staged header readback | Sequential; below the 32-command replay grain, with primary readback; small-scene recording overhead is measured in the performance report |
| Selection outline | JFA seed, each ping-pong flood step, composite | Sequential dependent stages |

## Prepared graph passes

| Pass | Prepared body and publication |
|---|---|
| AOApply | Fullscreen body; primed shared post-process/SSAO UBOs; resolved AO/depth/color; diagnostics primary |
| ChromaticAberration, FXAA, Vignette | Fullscreen bodies; primed shared post-process UBO; primitive prepared before fork |
| ContactShadow | Fullscreen body; resolved G-Buffer/depth/color and primed inputs; warning counters in preparation |
| DOF, SSS | Fullscreen bodies; primed post-process/SSS uploads; SSS preserves no-clear behavior |
| EASU | Fullscreen body with private sizing upload and reduced-color input |
| DepthVelocityUpscale | Two-MRT fullscreen body with private sizing upload; reduced depth/velocity inputs independent of EASU color |
| SSAO | Raw/blur/export chain with private SSAO upload and explicit scratch/output writes; shared settings publish after join |
| GTAO | HZB/classifier/AO/denoise/export chain; caller resolves/resizes/snapshots; private uploads; settings publish after join |
| VolumetricFog | Scatter/integrate chain; captured clustered-light SSBOs and private froxel/Forward+ UBOs; history/settings publish after join |
| VSM marking | Page-mark dispatch with private globals and declared page/meta/request/statistics writes |

The default live deferred graph demonstrates GTAO/VSM marking. Additional live
captures prove DepthVelocityUpscale/EASU, SSAO/DepthVelocityUpscale, and
VolumetricFog/GTAO/VSM-marking groups. Each off/on image pair matches exactly;
reported item names prove worker-path execution, with zero conflicts and shader
errors. Detailed subqueries stay inline-only, with primary timestamps around
ordered pass execution. See [graph recording](vulkan-parallel-graph-recording.md).

## Cross-pass dependencies that retain sequential execution

`Setup()` selects the latest valid color resource. Disabled stages collapse these
chains while preserving an edge between the remaining producer and consumer.

- SSGI -> SSR -> ContactShadow -> EASU/Bloom, followed by Bloom -> DOF ->
  MotionBlur -> TAA -> Cloudscape -> Precipitation -> Fog, consume successive color
  versions. Internal ping-pong stages alone are not an exclusion: SSAO, GTAO and
  froxel fog keep their dependent chain inside one prepared body.
- ColorGrading consumes the latest ChromaticAberration/Fog color. ToneMap consumes
  graded color; Upscaler consumes ToneMap; Vignette, FXAA and SelectionOutline
  continue that chain. ColorBlind consumes UIComposite; Final consumes the finished
  composite and owns the acquired-backbuffer boundary.
- SphereProxyAO modifies GTAO's completed AO. AOApply consumes final AO and the
  preceding scene color. Proxy selection/upload and subpass queries stay primary.
- OITPrepare consumes scene-framebuffer depth, including DeferredLighting's depth
  copy. Contributors depend on preparation; OITResolve consumes completed
  contributors and the preceding scene color. OITPrepare clears/blits once.
- FluidComposite consumes FluidIntermediates' published textures/appearance and
  the preceding SceneColor writer, then performs its refraction copy and draw.
- Fixed-grid light culling is one dispatch; depth-aware culling consumes its depth
  reduction and active-cluster compaction results.

Additional primary-only constraints are explicit. UIComposite invokes an
unrestricted Renderer2D/UI callback and requires a prepared UI command list before
whole-pass recording is safe. ParticlePass builds/flushes shared batch state and
toggles OIT mode; its mesh ranges are converted internally. FSR2's Vulkan factory
returns BackendUnsupported, so no Vulkan recording body exists to partition.
DeferredOpaqueDecal invokes converted replay before its dependent MSAA/export
copies. DeferredLighting's fullscreen draw is followed by depth/entity-ID/debug
copies; its output feeds the following scene-color consumers.

Graph resource resolvers are not automatically safe because they are const:
registry construction, placeholder warnings and resolve diagnostics mutate shared
state. Resolve and prewarm on the caller. Each recording lane owns its active-pass
label; physical-resource write declarations include shared uploads published after
join, so unrelated prepared readers cannot silently see old parameters.

## Existing Vulkan limitations encountered during validation

`VirtualGeometryTest.olo` renders from both inspected OpenGL angles. This branch's
base predates [PR #1062](https://github.com/drsnuggles8/OloEngineBase/pull/1062),
which implements missing raw geometry upload/index-buffer paths (#1052). Vulkan
helmet captures here are empty even with forced MDI. The additional mesh-task
snapshot/device-fault defect is tracked by
[#1058](https://github.com/drsnuggles8/OloEngineBase/issues/1058). The user identified
these existing work items during validation. Repeat full Vulkan helmet evidence
after those dependencies land; empty captures are not successful visual evidence.

The compressed terrain VT array path is explicitly unsupported by the Vulkan
factory (missing BC7 tile-stage copy). Vulkan checks use an in-memory
`VTCompressedCache=false`; authored scene data is unchanged. Even then the final
live sample reports zero resident tiles and `readyForShading=false`, so its
correct terrain image proves fallback shading, not the new ready-cache
publication branch. OpenGL exercises the authored compressed configuration.
The separate water-array fix supplies required storage
usage for supported linear color formats and has a four-format descriptor test.

The authored SnowfallParticles sample produced no visible billboards in either
Vulkan recording mode, including Release. This is not accepted visual evidence
and its cause was not established. Billboard batching is unchanged here. The
modified mesh-particle path is covered separately by the real-device off/on
readback and three-angle OpenGL scene test above.

## Validation and timing

The final Release heavy-scene recording blocks improve consistently; the normal
scene exposes a small recording overhead. The shared host prevents a reliable
end-to-end FPS speedup claim. See the [measurements and evidence report](../analysis/vulkan-parallel-recording-1013.md)
for the interleaved blocks, untouched pass control, optional fork-cost probe,
attachment comparisons and validation limits. Linux TSan is a PR completion gate.
