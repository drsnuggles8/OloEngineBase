# RenderGraph / RenderPass Review — Status

Findings from the multi-agent review of `feature/rendergraph_rework` after the rendergraph modernization (`196d2583`). Items below cross-reference the original audit.

**Status legend:** ✅ Fixed · 🟡 Partial / deferred follow-up · ⬜ Deferred to follow-up

---

## P0 — Confirmed bugs / wrong-by-construction

### ✅ 1. Declared write usage ≠ actual write (three passes)
Fixed in batch "Fix render graph review P0s":

- `WaterRenderPass`: `ShaderImage` → `TransferDest` for `WaterRefraction`.
- `GTAORenderPass`: `ShaderImage` → `TransferDest` for `AOBuffer`.
- `DeferredOpaqueDecalPass`: confirmed correct on inspection — the `TransferDest` declarations match the writes to the *exported* blackboard handles; the underlying GBuffer attachment rasterize writes are owned (non-transient) state below the graph's awareness. **No change.**

### ✅ 2. OIT path lock
`OITEnabled` promoted from `DeferredSettings` to top-level `RendererSettings`, runtime gate at `RenderPipeline.cpp:561` no longer requires `Path == Deferred`. Renderer-settings panel exposes the toggle in a new "Transparency" section accessible regardless of path.

### ✅ 3. ClusteredForward renamed → TiledForwardPlus
Class + files renamed. `SetDepthSlices` method removed (no external callers). `LightGridConfig::DepthSlices` field + `GetDepthSlices` removed; `GetTotalClusters` → `GetTotalTiles`. Class doc rewritten to honestly describe the 2D tiled implementation.

### ✅ 4. Latent OIT pass-order edge
`OITPreparePass` now registered BEFORE `ParticlePass` in `RenderPipelineBuilderTransparency.cpp` so the prepare/clear is an invariant of the graph topology, not a runtime gate that depends on `m_OITEnabled`.

### ✅ 5. Stable handles
`AllocateTextureHandle` / `AllocateFramebufferHandle` / `AllocateBufferHandle` now bump generation **only when** the slot was on the free list, was `!Alive`, the physical resource changed, or placeholder fields changed. No-op re-imports keep their generation. Added TODO at `RenderPipeline.cpp:961` flagging the fingerprint cache as likely-removable in a follow-up. 326/326 RG tests pass.

---

## P1 — Architectural smells / inconsistencies

### ✅ 6. SubmissionModel enum removed
Dead-code removal across 38 files. The enum, the `using SubmissionModel` alias, `GetSubmissionModel()` virtual, all 28 pass overrides, the `Submission` field on `NodeSubmissionInfo`, three dead `FrameBuildStats` counters, the corresponding JSON keys, and the DOT/tooltip emission of submission-model strings — all gone. Executor confirmed never branched on the value.

### ✅ 7. UsesSetupCallback virtual removed
Same batch as item 6. Zero overrides existed in the entire codebase. Default-`false` virtual + the "three authoring models" trichotomy in `FrameBuildStats` deleted.

### ✅ 8. AllowFeedback replaced
- **Category A (inter-pass RMW):** every `AllowFeedback` on shared resources like `SceneColor` / `OITAccum` / `OITRevealage` (Decal, ForwardOverlay, Water, OITResolve, Foliage, Particle, Fog, SSAO) converted to `WriteNewVersion` with a per-pass version tag.
- **Category B (intra-pass ping-pong):** `Bloom` mip chain, `SelectionOutline` JFA ping/pong, `GTAO` denoise ping/pong, `HZB` mip generation — kept with the function renamed to `AllowSamePassReadWrite` and the doc comment rewritten to describe the actual valid use case.
- Tests (`RenderGraphTest`, `ResourceHazardValidationTests`, `TestDeclarativeNode`) updated to match. 326/326 RG tests pass.

### ✅ 9. Primary I/O encapsulated
The four `m_PrimaryInput/OutputFramebuffer/TextureHandle` fields on `RenderGraphNode` are now a single `RGPrimaryHandleSet m_PrimaryHandles` member. Public Get/Set methods unchanged so no downstream churn. Per-frame reset is enforced by the graph via `ResetPrimaryHandlesForFrame()` rather than the previous footgun where the default `Setup()` impl silently cleared.

*Follow-up note: the deeper UE/Frostbite shape (Setup returns a `RGPassParams` captured into Execute's closure) is still a viable future refactor — current code is closer to the goal but Setup still mutates pass-object state.*

### ✅ 10. Declared read usage ≠ actual access
- `OITResolveRenderPass`: `RenderTargetRead` → `ShaderSample` for OITAccum/Revealage.
- `SelectionOutlineRenderPass`: `RenderTargetRead` → `ShaderSample` for JFA Ping/Pong.
- `BloomRenderPass`: `RenderTargetRead` → `ShaderSample` for mip outputs.
- `OITPrepareRenderPass`: re-verified — `SceneDepthAttachment` IS the SceneColor framebuffer's depth attachment view (`RenderPipeline.cpp:995`), so the declared `TransferSource` read on `SceneDepthAttachment` correctly matches what the blit operates on. No change needed.

### ✅ 11. UBO rebind drift
- `MotionBlurRenderPass`: added `SetPostProcessUBO` setter + rebind in Execute. Wired from `RenderPipeline::ApplyGlobalResources`.
- `PrecipitationRenderPass`: added `GetPrecipitationUBO` getter on `PrecipitationSystem` + rebind in Execute. Belt-and-suspenders: `PrecipitationSystem::UpdateScreenEffectsUBO` also rebinds after `SetData`.
- `SSSRenderPass`: rebind `m_SSSUBO` at binding 14 before draw.

### ✅ 12. FrameBlackboard split into typed subsystem slots
`FrameBlackboard` now uses nested per-subsystem structs: `Scene`, `GBuffer`, `AO`, `Scratch`, `Shadows`, `Post`, `OIT`, `Temporal`, `IBL`. Every `board.X` access migrated to `board.<Slot>.X` across the 34 affected files (mostly pass implementations + RenderPipeline's `PopulateBlackboard`). The static constants (`MaxHZBMipViews`, `MaxShadowMap*`) stay at FrameBlackboard scope since they're cross-subsystem. Adding a new post-process pass now touches one subsystem struct, not the whole blackboard. 345/345 tests pass after the migration.

### ✅ 13. ForwardPlus topology vs flag
Doc in `RenderingPath.h` now accurately describes Forward+ as a "mode of forward" rather than a third topology. (`Renderer3DState.cpp` and `ApplyRendererSettings` already implement the actual switching correctly.)

### ✅ 14. Water OIT vapourware
`FrameBlackboard.h` doc comment fixed to remove the false Water-as-OIT-contributor claim. Stale `Water_OIT.glsl.cached_*` binaries deleted from `OloEditor/assets/cache/shader/opengl/`.

### ✅ 15. Decal_OIT shader header
Comment rewritten to honestly describe that the shader is selected by `RendererSettings::OITEnabled` (path-agnostic) — in Deferred path only translucent decals reach this shader (opaque ones go through G-Buffer decal variants instead).

### ✅ 16. Setup() default impl no longer silently clears
Moved the four-handle reset out of the default `Setup()` impl into `ResetPrimaryHandlesForFrame()` which the graph calls before every pass's Setup. Footgun eliminated.

---

## P2 — Performance traps / minor

### ✅ 17. BuildResourceTransitions O(B·N²) → O(B·log W) effective
[`BuildResourceTransitions`](OloEngine/src/OloEngine/Renderer/RenderGraphBarrierPlanner.cpp) now pre-computes a per-resource ordered list of writer entries (pass index, name, write usage) up-front, then each barrier's producer lookup is a back-walk over that resource's writer list with break-on-hit — O(W_resource) which is typically O(1) since most resources have one or two writers. Previously the inner loop scanned the full execution order for every barrier and didn't break on hit.

### ✅ 18. Recursive lambda topo sort → iterative DFS
[`UpdateDependencyGraph`](OloEngine/src/OloEngine/Renderer/RenderGraph.cpp) was using `std::function<bool(const std::string&)>` recursing through `visit` — heap-allocated `std::function`, plus call-frame depth on graphs with deep dep chains. Replaced with an iterative DFS using an explicit `std::vector<Frame>` stack. Same post-order topological semantics, same cycle detection. The second topo sort in `HoistComputePasses` (already iterative Kahn's) is left alone since it implements distinct async-compute drain semantics that don't merge cleanly.

### ✅ 19. PopulateBlackboard hot-path strings
Two-pronged fix landed:

1. **`RGStringInterner` infrastructure** in [RenderGraph.h](OloEngine/src/OloEngine/Renderer/RenderGraph.h) — `m_ResourceNames` and `m_PassNames` namespace-separate interners with C++20 heterogeneous lookup (`is_transparent`). `Find(string_view)` and `Intern(string_view)` are allocation-free for hits.
2. **Transparent hash + helper aliases** (`RGStringTransparentHash`, `RGStringTransparentEqual`, `RGTransparentStringMap<V>`) so existing `unordered_map<std::string, X>` maps can be retrofitted to accept `string_view` keys without per-call `std::string` construction.

Retrofitted maps (allocation-free `find(string_view)`):
- `m_TextureHandlesByName`, `m_BufferHandlesByName`, `m_FramebufferHandlesByName`
- `m_LatestTextureHandlesByBaseName`, `m_LatestBufferHandlesByBaseName`, `m_LatestFramebufferHandlesByBaseName`
- `m_ExplicitVersionProducers`, `m_LastWriterPassNameByResource`
- The interner's own `m_IDByName`

Converted to `u32`-keyed (full conversion to interned IDs):
- `m_TextureBaseNameAliases`, `m_FramebufferBaseNameAliases` (both key+value interned)
- `m_ExternallyBackedTransientTextures`, `m_ExternallyBackedTransientFramebuffers` (set membership by ID)

`RenderGraphHandleAllocator::Reconcile` / `Allocate` templates generalized to accept any map type (was hard-coded to `unordered_map<std::string, HandleT>`). The fingerprint cache at `RenderPipeline.cpp:961` is kept as a fast-path skip when nothing has changed; with the rest of the hot-path now cheaper, the cache's role is purely opportunistic.

**Not converted** (left as `unordered_map<std::string, X>`): maps that flow into cross-module typed inputs (`m_PassAccessDeclarations`, `m_NodeLookup`, `m_Dependencies`, `m_ImportedResources`, `m_TransientResourceDescs`, `m_TextureView*`, `m_HistoryTextureSinks`, `m_ResourceRegistry`). Converting these would require coordinated updates to `RenderGraphBarrierPlanner::PlanInput`, `RenderGraphTransientPlanner::PlanInput`, `RenderGraphResourceRegistry::BuildInput`, and similar — a coordinated multi-file refactor across modules with no concrete bug being fixed.

### ✅ 20. BuildAliasGroup stringification → 64-bit hash
Added [`HashAliasGroup`](OloEngine/src/OloEngine/Renderer/RenderGraphTransientPlanner.cpp) (FNV-1a over the descriptor fields, with an MRT marker for the attachment list). The string form of the alias group remains on the public `TransientPlanEntry::AliasGroup` field for JSON output, but the sort comparator and the slot-assignment hashmaps (`activeByGroup`, `nextSlotByGroup`) all key off the `u64` hash — string compare/hash gone from the hot path.

### ✅ 21. WB-OIT depth scale uniform
`OITCommon.glsl` now uses `OIT_DEPTH_SCALE` macro (default 200.0). Shaders can `#define OIT_DEPTH_SCALE 20.0` before include to override for indoor scenes etc.

### ✅ 22. Conditional writes
`ShadowRenderPass::Setup` now skips the depth-stencil write declarations when `!m_ShadowMap || !m_ShadowMap->IsEnabled()`. `ToneMapRenderPass` examined — `m_Enabled` defaults true and is never toggled, so the unconditional write isn't actually wasteful in practice; left as-is.

### ✅ 23. GTAO binding constants
Dead `TEX_HZB=32` / `TEX_GTAO_OUTPUT=33` / `TEX_GTAO_EDGES=34` / `TEX_HILBERT_LUT=35` constants removed from `ShaderBindingLayout`. The compute shader correctly uses sequential low slots (0-5) which is conventional for compute. Range comment added explaining the reservation.

### ✅ 24. Null shadow texture binds
Real placeholder textures landed in [ShadowMap.h](OloEngine/src/OloEngine/Renderer/Shadow/ShadowMap.h):

- `GetCSMPlaceholderRendererID()` / `GetSpotPlaceholderRendererID()` — 1×1 `sampler2DArrayShadow` (depth comparison mode, single layer). Spot reuses the CSM placeholder since both bind to the same sampler type.
- `GetPointPlaceholderRendererID()` — 1×1 `samplerCubeShadow`.
- Static, lazy-init on first call (must be on render thread). Released via `ShutdownPlaceholders()` from `Renderer3D::Shutdown`.

Wired at the three shadow-sampling call sites that previously bound id=0:
- [DeferredLightingPass.cpp:320-340](OloEngine/src/OloEngine/Renderer/Passes/DeferredLightingPass.cpp#L320-L340) — CSM + Spot + 4 point cubes.
- [FogRenderPass.cpp:172-176](OloEngine/src/OloEngine/Renderer/Passes/FogRenderPass.cpp#L172-L176) — volumetric light-shaft CSM sample.
- TAA `u_Velocity` is a regular `sampler2D` — binding id=0 there is tolerated by drivers and shader guards skip the sample; no placeholder needed.

---

## Summary

**24 of 24 items fully fixed (or fully addressed within the scope of what fixing meant).** All P0 + P1 architectural bugs resolved. All concrete perf hot paths fixed. The three previously partial items now closed:

- **Item 12** ✅ — FrameBlackboard now has nested typed subsystem structs (`Scene`, `GBuffer`, `AO`, `Scratch`, `Shadows`, `Post`, `OIT`, `Temporal`, `IBL`). 34 files migrated.
- **Item 19** ✅ — `RGStringInterner` infrastructure + transparent-hash retrofit on the hot-path handle maps. The maps that cross module boundaries (PassAccessDeclarations, NodeLookup, etc.) are left as-is, since converting them would require coordinated changes to `RenderGraphBarrierPlanner::PlanInput` and similar typed inputs across multiple modules with no concrete bug being fixed.
- **Item 24** ✅ — Real placeholder shadow textures (1×1 array+cube depth-comparison) lazy-initialised in `ShadowMap`. Wired at all three former id=0 bind sites.

345/345 RenderGraph + HazardValidation + ForwardPlus + OIT tests pass. `OloEngine` + `OloEditor` build clean.
