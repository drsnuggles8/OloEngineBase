# RenderGraph / RenderPass Review — Open Issues

Findings from the multi-agent review of the `feature/rendergraph_rework` branch after the rendergraph modernization (commit `196d2583`). Two regressions were fixed in `40596138` (SSAO half-res, ColorGrading LUT); everything below is still outstanding.

Items are ranked **P0** (confirmed bug / wrong-by-construction), **P1** (architectural smell / latent inconsistency), **P2** (performance trap / minor).

---

## P0 — Confirmed bugs / likely-broken-when-enabled

### 1. Declared write usage ≠ actual write (three passes)
Same family as the SSAO/AOBuffer drift just fixed — the barrier planner gets the wrong synchronization edges.

- [WaterRenderPass.cpp:63](OloEngine/src/OloEngine/Renderer/Passes/WaterRenderPass.cpp#L63) declares `ShaderImage` for `WaterRefraction` but Execute uses `glCopyImageSubData`. Should be `TransferDest`.
- [GTAORenderPass.cpp:80](OloEngine/src/OloEngine/Renderer/Passes/GTAORenderPass.cpp#L80) declares `ShaderImage` for `AOBuffer` but only writes via `glCopyImageSubData`. Should be `TransferDest`.
- [DeferredOpaqueDecalPass.cpp:49,54,59,65,70,75,80,85](OloEngine/src/OloEngine/Renderer/Passes/DeferredOpaqueDecalPass.cpp#L49) declares `TransferDest` for all 4 G-Buffer color targets, but actually **rasterizes** decals into RT0/RT1/RT2 first, *then* copies. The rasterize RT write is entirely undeclared — DeferredLightingPass may miss the correct barrier edge.

### 2. OIT will never turn on by default, and is path-locked
- [`Deferred.OITEnabled = false`](OloEngine/src/OloEngine/Renderer/RenderingPath.h#L51) at default.
- [RenderPipeline.cpp:561](OloEngine/src/OloEngine/Renderer/RenderPipeline.cpp#L561) gates `oitActive = (Path == Deferred) && OITEnabled`.
- The shaders `Decal_OIT.glsl` / `Particle_Billboard_OIT.glsl` are path-agnostic — [`Decal_OIT.glsl:9-12`](OloEditor/assets/shaders/Decal_OIT.glsl#L9-L12) even says "Forward / Forward+". Either the gate is wrong or the shader doc lies. Fix: either flip the default, relax the path lock, or update doc.

### 3. "ClusteredForward" is aspirational scaffolding
- [`LightGrid.h:13`](OloEngine/src/OloEngine/Renderer/LightCulling/LightGrid.h#L13) defaults `DepthSlices = 1`.
- `SetDepthSlices` has **zero external callers**.
- [`LightCulling.comp`](OloEditor/assets/shaders/compute/LightCulling.comp) has no froxel/slice math.

Either implement froxels or rename `ClusteredForward` → `TiledForwardPlus`. Class naming overstates the implementation.

### 4. Latent OIT pass-order bug
Pass order has `OITPreparePass` *after* `ParticlePass` (in `rendergraph.json`). The reordering relies on `builder.DependsOnPass("OITPreparePass")` declared **inside `if (m_OITEnabled)`** at [ParticleRenderPass.cpp:57](OloEngine/src/OloEngine/Renderer/Passes/ParticleRenderPass.cpp#L57) and [DecalRenderPass.cpp:71](OloEngine/src/OloEngine/Renderer/Passes/DecalRenderPass.cpp#L71). If OIT is enabled but the edge declaration ever regresses, prepare clears *after* contributors write.

Fix: register OITPreparePass before contributors in [RenderPipelineBuilderTransparency.cpp](OloEngine/src/OloEngine/Renderer/RenderPipelineBuilderTransparency.cpp), not after.

### 5. Stable-handle invariant violated
[RenderGraph.cpp:364,479](OloEngine/src/OloEngine/Renderer/RenderGraph.cpp#L364) reuses slot indices when re-importing by name but **pre-increments `generation` every time** — invalidating every handle copy from the prior frame even when nothing changed. The fingerprint cache at [RenderPipeline.cpp:957-963](OloEngine/src/OloEngine/Renderer/RenderPipeline.cpp#L957-L963) is a workaround for this self-inflicted breakage. UE's `FRDGHandle` is stable within a builder lifetime.

Fix: only bump generation when the underlying descriptor actually changes.

---

## P1 — Architectural smells / inconsistencies

### 6. `SubmissionModel` enum is dead
- [RenderGraphNode.h:47-53](OloEngine/src/OloEngine/Renderer/RenderGraphNode.h#L47-L53). 26 passes override `GetSubmissionModel()` returning `ImmediateOnly`/`Mixed`/`BucketOnly`.
- Only consumers are debug logging — [RenderGraph.cpp:4533-4539](OloEngine/src/OloEngine/Renderer/RenderGraph.cpp#L4533-L4539) and [RenderGraphDebugger.cpp:62-68](OloEngine/src/OloEngine/Renderer/Debug/RenderGraphDebugger.cpp#L62-L68).
- The executor at [RenderGraphPlanExecutor.cpp:47-57](OloEngine/src/OloEngine/Renderer/RenderGraphPlanExecutor.cpp#L47-L57) calls `Execute()` unconditionally regardless.

Either implement (bucket passes batch into command buckets, immediate executes inline) or delete the enum.

### 7. `UsesSetupCallback()` axis is fiction
- [RenderGraphNode.h:210-213](OloEngine/src/OloEngine/Renderer/RenderGraphNode.h#L210-L213) defaults `false`. **Zero overrides exist anywhere.**
- The "three authoring models" trichotomy in `FrameBuildStats` ([RenderGraph.h:602-611](OloEngine/src/OloEngine/Renderer/RenderGraph.h#L602-L611)) measures axes that don't differentiate any pass.

Delete.

### 8. `AllowFeedback` is the wrong abstraction
Used in 17 spots; it silences the hazard validator instead of solving the underlying problem with **renaming**. You already have `WriteNewVersion`. Cases on `SceneColor` / `OITAccum` / `OITRevealage` should rename through `WriteNewVersion` per frame:

- [DecalRenderPass.cpp:53,59,76](OloEngine/src/OloEngine/Renderer/Passes/DecalRenderPass.cpp#L53)
- [ForwardOverlayRenderPass.cpp:34](OloEngine/src/OloEngine/Renderer/Passes/ForwardOverlayRenderPass.cpp#L34)
- [WaterRenderPass.cpp:36](OloEngine/src/OloEngine/Renderer/Passes/WaterRenderPass.cpp#L36)
- [OITResolveRenderPass.cpp:48](OloEngine/src/OloEngine/Renderer/Passes/OITResolveRenderPass.cpp#L48)
- [FoliageRenderPass.cpp:26](OloEngine/src/OloEngine/Renderer/Passes/FoliageRenderPass.cpp#L26)
- [ParticleRenderPass.cpp:37,43,63](OloEngine/src/OloEngine/Renderer/Passes/ParticleRenderPass.cpp#L37)

Genuinely-intra-pass cases (Bloom mip ping-pong, JFA, GTAO denoise) should be modeled as `ShaderStorage`/`ShaderImage` access on a renamed handle.

### 9. Primary-input/output handle is side-channel pass state
[RenderGraphNode.h:328-331](OloEngine/src/OloEngine/Renderer/RenderGraphNode.h#L328-L331), used by 30 files. Setup calls `pass->SetPrimaryInputFramebufferHandle(...)`, Execute reads back from member state. UE's FRDG passes capture parameters into the lambda closure / a generated `RDG_PARAMS` struct.

This coupling blocks any future parallel-Setup or compile-once/execute-many scheme.

### 10. Declared read usage ≠ actual access (four passes)
- [OITResolveRenderPass.cpp:39,44](OloEngine/src/OloEngine/Renderer/Passes/OITResolveRenderPass.cpp#L39) declares `RenderTargetRead` for OITAccum/Revealage but shader-samples them. Should be `ShaderSample`.
- [SelectionOutlineRenderPass.cpp:73,81](OloEngine/src/OloEngine/Renderer/Passes/SelectionOutlineRenderPass.cpp#L73) declares `RenderTargetRead` for JFA ping-pong but shader-samples them. Should be `ShaderSample`.
- [BloomRenderPass.cpp:65-67](OloEngine/src/OloEngine/Renderer/Passes/BloomRenderPass.cpp#L65-L67) declares `RenderTargetRead` for mip outputs but Execute shader-samples them. Should be `ShaderSample`.
- [OITPrepareRenderPass.cpp:56,64](OloEngine/src/OloEngine/Renderer/Passes/OITPrepareRenderPass.cpp#L56) declares wrong depth source — the blit reads from sceneFramebuffer's depth attachment, not `SceneDepthAttachment`.

### 11. UBO rebind drift across post-process passes
Other passes (IBL precompute, Bloom mip updates) can displace UBO bindings.

- [MotionBlurRenderPass.cpp:131-132](OloEngine/src/OloEngine/Renderer/Passes/MotionBlurRenderPass.cpp#L131-L132) only rebinds binding 8, shader also reads PostProcessUBO at 7.
- [PrecipitationRenderPass.cpp:139-159](OloEngine/src/OloEngine/Renderer/Passes/PrecipitationRenderPass.cpp#L139-L159) only rebinds binding 19, never `UBO_PRECIPITATION` at 18.
- [SSSRenderPass.cpp:116](OloEngine/src/OloEngine/Renderer/Passes/SSSRenderPass.cpp#L116) comment claims UBO is bound by EndScene; never rebinds — vulnerable to displacement.

### 12. `FrameBlackboard` is a 200-field POD god struct
[FrameBlackboard.h:31-205](OloEngine/src/OloEngine/Renderer/FrameBlackboard.h#L31-L205). Every pipeline config carries every possible field. UE/Frostbite split into per-subsystem `Blackboard::GetOrCreate<T>()` slots.

Adding a new post-process pass is a multi-file edit (FrameBlackboard + ResourceNames + PopulateBlackboard + the pass). Bloom mips, GTAO scratch, Fog history, Water refraction all leak one level too high.

### 13. ForwardPlus is not a topology branch
[RenderingPath.h:18-27](OloEngine/src/OloEngine/Renderer/RenderingPath.h#L18-L27) suggests three paths, but ForwardPlus and Forward have identical pass topology — only `ForwardPlusMode` and `EnableDepthPrepass` flags differ. Closer to a "feature flag inside forward" than a third path. Either document accurately or wire it as a real path.

### 14. Water OIT is vapourware
- [FrameBlackboard.h:25](OloEngine/src/OloEngine/Renderer/FrameBlackboard.h#L25) claims Water is an OIT contributor.
- `WaterRenderPass.cpp` has zero OIT code.
- Only stale `OloEditor/assets/cache/shader/opengl/Water_OIT.glsl.cached_*` binaries remain — the source is gone.

Delete stale references + cached binaries, or implement Water → OIT.

### 15. `Decal_OIT.glsl` shader header lies about Forward/Forward+ support
Given the path-locked runtime gate (P0 #2). Either update header to reflect Deferred-only, or fix the gate.

### 16. `Setup()` default impl silently clears `m_PrimaryInput/OutputHandles`
[RenderGraphNode.h:101-104](OloEngine/src/OloEngine/Renderer/RenderGraphNode.h#L101-L104) — passive contract no caller can see. Footgun for any pass that forgets to call `RenderGraphNode::Setup(...)` first.

---

## P2 — Performance traps / minor

### 17. `BuildResourceTransitions` is O(B·N²) by design
[RenderGraphBarrierPlanner.cpp:309-337](OloEngine/src/OloEngine/Renderer/RenderGraphBarrierPlanner.cpp#L309-L337). Inner loop walks the entire execution order for every barrier and doesn't break on first hit. Last-writer state was already tracked during `ComputePlan` and thrown away.

### 18. Recursive lambda topo sort + duplicate Kahn's
[RenderGraph.cpp:2883-2912](OloEngine/src/OloEngine/Renderer/RenderGraph.cpp#L2883-L2912) uses `std::function` (heap-allocates) and recurses. `HoistComputePasses` already has a Kahn's implementation next to it. Two topo sorts back-to-back.

### 19. `PopulateBlackboard` hot path
Code's own comment at [RenderPipeline.cpp:949-955](OloEngine/src/OloEngine/Renderer/RenderPipeline.cpp#L949-L955) admits "~65ms per frame on a stable scene" without the fingerprint cache. Root cause: every declaration is keyed by `std::string` through `unordered_map`. Switch to interned name → `RGTextureHandle` slot table.

### 20. `BuildAliasGroup` stringifies descriptors as map keys
[RenderGraphTransientPlanner.cpp:13-30,292-296](OloEngine/src/OloEngine/Renderer/RenderGraphTransientPlanner.cpp#L13-L30). Hash to a 64-bit key instead.

### 21. WB-OIT depth scale hardcoded
[OITCommon.glsl:24](OloEditor/assets/shaders/include/OITCommon.glsl#L24) normalises by hardcoded 200 m. For small or huge scenes most fragments hit clamp endpoints. Push to a uniform.

### 22. Shadow / ToneMap declare writes unconditionally
- [ShadowRenderPass.cpp:24-74](OloEngine/src/OloEngine/Renderer/Passes/ShadowRenderPass.cpp#L24-L74) declares writes even when disabled / no casters.
- [ToneMapRenderPass.cpp:41](OloEngine/src/OloEngine/Renderer/Passes/ToneMapRenderPass.cpp#L41) `WriteNewVersion` runs even when `m_Enabled == false`.

Wastes transient framebuffer allocations.

### 23. GTAO uses local literal binding slots
[GTAORenderPass.cpp:15-17](OloEngine/src/OloEngine/Renderer/Passes/GTAORenderPass.cpp#L15-L17) — `GTAO_HZB_TEXTURE_SLOT=3` / `_NORMALS=4` / `_HILBERT=5` bypass `ShaderBindingLayout::TEX_HZB=32` etc. The reserved engine constants are dead.

### 24. Texture id `0` bound to shadow array/cube samplers when absent
[FogRenderPass.cpp:207](OloEngine/src/OloEngine/Renderer/Passes/FogRenderPass.cpp#L207), [DeferredLightingPass.cpp:326-339](OloEngine/src/OloEngine/Renderer/Passes/DeferredLightingPass.cpp#L326-L339), [TAARenderPass.cpp:178](OloEngine/src/OloEngine/Renderer/Passes/TAARenderPass.cpp#L178). Even guarded by uniforms, some drivers validate texture-target type at draw call. Use small placeholder textures of the right type.

---

## Recommended order

1. **Usage-flag drift sweep** (items 1, 10) — same kind of bug that caused the SSAO regression; cheapest fix per item.
2. **Decide on `SubmissionModel` and `UsesSetupCallback`** (items 6, 7) — either implement or delete; cleaner code either way.
3. **Fix the OIT story** (items 2, 4, 14, 15) — make the gate honest, settle Water OIT vapourware.
4. **Replace `AllowFeedback` with `WriteNewVersion` renaming** (item 8) — restores hazard validator signal.
5. **Stable handles + interned slot table** (items 5, 19, 20) — biggest single architectural win; eliminates the fingerprint cache.
6. **Rename `ClusteredForward` → `TiledForwardPlus`** (item 3) — or implement froxels.
7. **Split `FrameBlackboard` into typed subsystem slots** (item 12) — biggest authoring-ergonomics win.
8. Smaller cleanups: UBO rebinds (11), primary I/O off pass members (9), Setup default clearing (16), perf (17, 18), shadow null-binds (24), GTAO binding constants (23), WB-OIT depth uniform (21), conditional declarations (22).
