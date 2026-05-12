# RenderGraph Gap Analysis vs UE RDG and Refactor Plan

## Purpose

This document complements `docs/rendergraph-modernization-plan.md`.

Its scope is narrower and more concrete:

1. Compare the current OloEngine renderer to a UE-style Render Dependency Graph using actual code, not project docs.
2. Identify the remaining gaps that keep OloEngine in a hybrid state.
3. Define a staged refactor plan that gets OloEngine to the same practical level of graph ownership and authoring quality while keeping two constraints:
   - the backend remains OpenGL 4.6 for now;
   - the Molecule-style command bucket system remains the draw recording and replay strategy for draw-heavy work.

The goal is not to mimic UE's API surface literally. The goal is to reach the same level of architectural maturity:

- graph-first pass authoring;
- graph-owned resource lifetime and extraction semantics;
- graph-owned scheduling and hazard management;
- graph-native execution of raster, fullscreen, compute, copy, UI, and present work;
- backend-portable execution metadata so Vulkan or DX12 can be added later without another major renderer rewrite.

## Evidence Base

The assessment below is based on the current implementation, especially these surfaces:

- `OloEngine/src/OloEngine/Renderer/Renderer3DFrameExecution.cpp`
- `OloEngine/src/OloEngine/Renderer/RenderGraph.h`
- `OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`
- `OloEngine/src/OloEngine/Renderer/RGBuilder.h`
- `OloEngine/src/OloEngine/Renderer/RGBuilder.cpp`
- `OloEngine/src/OloEngine/Renderer/RGCommandContext.h`
- `OloEngine/src/OloEngine/Renderer/RGCommandContext.cpp`
- `OloEngine/src/OloEngine/Renderer/Passes/RenderPass.h`
- `OloEngine/src/OloEngine/Renderer/RenderPipeline.cpp`
- `OloEngine/src/OloEngine/Renderer/RenderPipelineBuilder.cpp`
- `OloEngine/src/OloEngine/Renderer/RenderPipelineBuilderInternal.h`
- `OloEngine/src/OloEngine/Renderer/RenderPipelineBuilderScene.cpp`
- `OloEngine/src/OloEngine/Renderer/RenderPipelineBuilderTransparency.cpp`
- `OloEngine/src/OloEngine/Renderer/RenderPipelineBuilderPost.cpp`
- `OloEngine/src/OloEngine/Renderer/Passes/GTAORenderPass.cpp`
- `OloEngine/src/OloEngine/Renderer/Passes/TAARenderPass.cpp`
- `OloEngine/src/OloEngine/Renderer/Passes/FogRenderPass.cpp`
- `OloEngine/src/OloEngine/Renderer/Passes/FinalRenderPass.cpp`

## What "Same Level as UE" Means Here

Under OpenGL 4.6, OloEngine cannot match every explicit-backend capability of UE RDG.

It can match UE's level in these areas:

- graph-first authoring model;
- compile-time dependency derivation from declared resource usage;
- aggressive pass culling;
- graph-owned transient planning and aliasing at the logical level;
- explicit extraction and external resource contracts;
- graph-owned presentation and side-effect handling;
- scheduler metadata for graphics, compute, and copy work;
- backend-portable transition and submission IR;
- strong diagnostics and validation.

It cannot fully match UE on pure OpenGL in these areas:

- true multi-queue async compute overlap;
- real image-layout transitions and queue ownership transfers;
- heap-level residency control and placed-resource aliasing fidelity;
- explicit API synchronization fidelity equal to Vulkan or DX12.

Therefore the target should be:

> UE-level graph architecture and authoring quality on top of an OpenGL-compatible backend contract, with explicit-backend metadata preserved for future backends.

## Current State Summary

OloEngine is no longer a simple pass chain. The graph core already does real frame-graph work.

What already exists:

- `Renderer3D::EndScene()` runs `BuildFrameGraph()` and executes the compiled plan without a same-frame blackboard-handle repair step.
- `RenderGraph::BuildFrameGraph()` runs each node's `Setup()` declarations, derives dependencies from declared reads and writes, updates topo order, computes reachability, computes barriers, rebuilds the transient plan, and caches a submission IR.
- `RenderGraph::Execute()` replays a compiled submission plan rather than traversing topology ad hoc.
- `RenderGraph` supports imports, transient declarations, resource resolve, extraction, history contracts, hazard validation, barrier diagnostics, transition records, and resource lifetime records.
- `RenderGraph` already models pass work type and async-compute candidacy.
- The transient planner already computes alias groups and alias slots.
- Some graph setup is subresource-aware, for example the GTAO HZB mip chain.

What is still hybrid:

- the primary authoring object is still `RenderPass`, a long-lived legacy pass type with `Init()`, framebuffer ownership, and `Execute()`; the old `DeclareRead()` / `DeclareWrite()` surface no longer lives on `RenderPass` itself and now survives only through explicit node overrides or test-only helpers;
- production runtime passes now register directly as `RenderGraphNode` instances; the remaining authoring-model gap is conceptual rather than adapter-based, because `RenderPass` still carries a pass-centric lifecycle even though static declaration metadata has already been quarantined away from the runtime pass base;
- a much smaller share of important resources are still imported from pass-owned or renderer-owned objects rather than created as graph-owned resources (for example both AO techniques now publish `AOBuffer` through graph-owned textures, `SceneColor` is now a graph-owned scene MRT with explicit live attachment views (`SceneColorTexture`, `SceneEntityID`, `SceneViewNormals`, `SceneDepthAttachment`), the fullscreen/post chain now publishes explicit color attachment views for its graph-owned outputs (`SSSColorTexture`, `AOApplyColorTexture`, `BloomColorTexture`, `DOFColorTexture`, `MotionBlurColorTexture`, `TAAColorTexture`, `PrecipitationColorTexture`, `FogColorTexture`, `ChromAbColorTexture`, `ColorGradingColorTexture`, `ToneMapColorTexture`, `VignetteColorTexture`, `FXAAColorTexture`, `SelectionOutlineColorTexture`, `UICompositeTexture`, and the alias `PostProcessColorTexture`), the deferred attachment set is now modelled as explicit views of graph-declared `GBufferResolved` / `GBufferMS` roots that resolve to prepared frame-local backing instead of hidden bridge copies, and shadow-map roots are now graph-declared transient textures with explicit frame-local backing instead of imported current-frame textures; the meaningful remaining imports are the backbuffer, IBL textures, and persistent histories);
- histories are now imported/extracted explicitly and backed by renderer-owned persistent textures, with `RenderPipeline` registering graph-managed history sinks each frame so `TAA`/`Fog` no longer perform post-execute copy-back callbacks themselves;
- generic extraction already roots reachability when declared before build, and explicit external texture sink contracts now exist as first-class graph state (typed handle registration, builder wrappers, dump metadata, and reachability tests), so the remaining extraction gap is mostly about broader versioned-write semantics rather than sink representation;
- same-frame blackboard handles no longer need a post-build refresh step in `EndScene()`, and `RGBuilder::WriteNewVersion(...)` now provides an opt-in first-class versioned-write handle path with several safe production seams (`SelectionOutlineRenderPass` -> `UICompositeRenderPass`, the early-post `SSSColor` handoff preferred by `AOApplyRenderPass` / `BloomRenderPass`, the early-post `AOApplyColor` handoff preferred by `BloomRenderPass`, the late-post `BloomColor` handoff preferred by `DOFRenderPass` / `MotionBlurRenderPass` / `TAARenderPass` / `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `DOFColor` handoff preferred by `MotionBlurRenderPass` / `TAARenderPass` / `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `MotionBlurColor` handoff preferred by `TAARenderPass` / `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `TAAColor` handoff preferred by `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `PrecipitationColor` handoff preferred by `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `FogColor` handoff preferred by `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `ChromAbColor` handoff preferred by `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `ColorGradingColor` handoff preferred by `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `ToneMapColor` handoff preferred by `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `VignetteColor` handoff preferred by `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the late-post `FXAAColor` handoff preferred by `SelectionOutlineRenderPass` / `UICompositeRenderPass`, and `UICompositeRenderPass` -> `FinalRenderPass`), but most production passes still rely on canonical mutable names; framebuffer-attachment views plus single-mip, array-layer, cube-face, and multisample-resolve texture views are now first-class today;
- a shrinking subset of pass runtime code still contains fallback chains and resource-selection logic that ideally belongs in graph setup;
- some ambiguous same-resource writer chains still reveal true registration-order sensitivity, but the graph no longer over-reports redundant direct-edge differences when explicit/setup-declared dependencies already fix the semantic order.

## Gap Analysis

| Area | Current OloEngine | UE-level target | Gap | Priority |
|---|---|---|---|---|
| Pass authoring model | `RenderGraphNode` is now the single graph-node base class. `RenderPass` was collapsed into it on 2026-05-11 by absorbing its storage and default implementations (framebuffer slot `m_Target` + `m_FramebufferSpec` + default `Init(spec)`/`GetTarget()`; primary I/O handles `m_PrimaryInputFramebufferHandle` / `m_PrimaryInputTextureHandle` / `m_PrimaryOutputFramebufferHandle` / `m_PrimaryOutputTextureHandle` + accessors; side-effect flag storage `m_SideEffects` + `SideEffect` enum + `IsSideEffecting`; work-type and async-compute flags `m_PassWorkType` + `m_AsyncComputeCandidate` + `SetPassWorkType` / `SetAsyncComputeCandidate`; DRS viewport storage `m_RenderViewportWidth` / `m_RenderViewportHeight` + default `ApplyRenderViewport`; default `Setup` that resets the primary-handle cache; empty `SetupFramebuffer` / `ResizeFramebuffer` / `OnReset`) into `RenderGraphNode` itself. The `RenderPass` alias header is deleted; every production pass header now inherits from `RenderGraphNode` directly, every `Ref<RenderPass>` / `RenderPass*` / `RenderPass::` scope reference has been rewritten to `RenderGraphNode`, and `CommandBufferRenderPass` chains its `GetFlags()` override into `RenderGraphNode::GetFlags()`. The remaining lifecycle cleanups already landed earlier in this milestone (no-arg `Execute()` removed; 19 trivial `GetTarget()` overrides dropped; 5 trivial `Init(spec)` overrides dropped). Subclasses that override `GetFlags()` to set raw flag bits (the test stubs) continue to work because `GetPassWorkType()` re-derives the work-type from `GetFlags()` rather than reading `m_PassWorkType` directly. Eight intentional comment/string mentions of `RenderPass` survive in the codebase as historical references. | Graph pass declaration is the primary source of truth. No parallel legacy pass contract. | Done | — |
| Resource declaration source | Production runtime passes declare access through `Setup()`, and static declaration coverage has been pushed out of `RenderPass` and survives only in explicit node overrides or test-only helpers. The Phase A audit (`## Resource Declaration Source Audit (2026-05-11)`) walked every `Execute()` body and found one remaining execute-time fallback path — `DeferredLightingPass` binding IBL textures and shadow maps via raw `Renderer3D::Get*ID()` despite declaring the reads in Setup. Phase B (landed 2026-05-11) fixed that by extending the `SelectedInputs` struct to carry the handles through Setup and resolving them via `context.ResolveTexture(...)` in Execute. The only `Renderer3D::Get*` calls left in Execute bodies are configuration / camera / shader-library accesses, none of which are GPU resource bindings. | One canonical declaration path per pass. | Done | — |
| Graph ownership of outputs | Frame-local scene/post outputs are graph transient and their live attachments are now explicit views; the deferred G-buffer and shadow roots are graph-declared transients with explicit frame-local backing. The remaining imports are the backbuffer, IBL textures, and persistent histories — all deliberately external state per `## Final Recommendation`. | The graph owns all frame-local resources and explicitly imports only true external resources. | Done | — |
| Handle stability | Same-frame blackboard and builder handles stay stable across compile, and `EndScene()` no longer repairs them post-build. | Handles used for a frame remain stable for the life of that compiled frame. | Done | — |
| Resource versioning | Opt-in versioned write handles exist through `RGBuilder::WriteNewVersion(...)`; framebuffer attachment views plus single-mip, array-layer, cube-face, and multisample-resolve texture views are first-class; production has fifteen safe versioned-write seams (`SelectionOutlineRenderPass` -> `UICompositeRenderPass`, the `SSSRenderPass` producer-owned `SSSColor` handoff preferred by `AOApplyRenderPass` / `BloomRenderPass`, the `AOApplyRenderPass` producer-owned `AOApplyColor` handoff preferred by `BloomRenderPass`, the `BloomRenderPass` producer-owned `BloomColor` handoff preferred by `DOFRenderPass` / `MotionBlurRenderPass` / `TAARenderPass` / `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `DOFRenderPass` producer-owned `DOFColor` handoff preferred by `MotionBlurRenderPass` / `TAARenderPass` / `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `MotionBlurRenderPass` producer-owned `MotionBlurColor` handoff preferred by `TAARenderPass` / `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `TAARenderPass` producer-owned `TAAColor` handoff preferred by `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `PrecipitationRenderPass` producer-owned `PrecipitationColor` handoff preferred by `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `FogRenderPass` producer-owned `FogColor` handoff preferred by `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `ChromaticAberrationRenderPass` producer-owned `ChromAbColor` handoff preferred by `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `ColorGradingRenderPass` producer-owned `ColorGradingColor` handoff preferred by `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `ToneMapRenderPass` producer-owned `ToneMapColor` handoff preferred by `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `VignetteRenderPass` producer-owned `VignetteColor` handoff preferred by `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass`, the `FXAARenderPass` producer-owned `FXAAColor` handoff preferred by `SelectionOutlineRenderPass` / `UICompositeRenderPass`, and `UICompositeRenderPass` -> `FinalRenderPass`); and canonical base-name handle lookup now follows the latest explicit version for debug/external resolve paths. The post-process linear chain has full versioned-write coverage (`docs/rendergraph-versioned-write-seam-adoption.md`), and the remaining same-resource writer chains in `OloEngine/src/OloEngine/Renderer/Passes` (`SceneColor` RMW, `OITAccum`/`OITRevealage`) are pinned via name-based `builder.DependsOnPreviousWriter(...)` — same correctness outcome, no aliasing-version primitive needed. The remaining gap is debug affordance only: a feedback-aware aliasing-version handle would let frame-capture name each modifier's intermediate `SceneColor@Modifier` view distinctly, but it's not load-bearing for graph correctness. | Resource versions or views are explicit in graph semantics. | Done | — |
| Extraction semantics | Pre-build extraction already roots liveness, and `RegisterExternalTextureSink(...)` gives the graph a first-class external sink/update contract with typed handle registration, setup-time reachability, and dump/test coverage. Generic callback extraction remains for ad-hoc readback. | Extraction and external access are first-class graph roots with explicit external sink semantics. | Done | — |
| History semantics | Histories are imported/extracted explicitly, `RenderPipeline` owns the persistent textures, and graph-managed history sinks handle next-frame write-back without pass-local callbacks. The backing textures are still renderer-owned external state rather than graph-owned multi-frame resources — accepted out-of-scope per `## Final Recommendation` (multi-frame resource ownership is a future explicit-backend follow-up alongside placed-resource aliasing). | Histories are graph-level external resources with explicit ingress and egress contracts. | Done | — |
| Render bucket integration | Buckets are owned and replayed by bucket-backed graph nodes (`ScenePass` and the render-stream passes). The frontend/routing cleanup landed on 2026-05-11 in three slices (see `## Pipeline-Builder Pass-Centric Audit`): the SceneColor RMW + OIT contributor chains now resolve via name-based `builder.DependsOnPreviousWriter(...)` instead of typed pass-pointer setters, the post-process preferred-source ladder was replaced with `builder.GetGraph().GetFramebufferHandle(name)` + a name-list selector helper (deleted 80 setter calls + 14 setter families across 14 downstream passes), and `RenderPipelinePassInputs` is now a list of `RenderGraphNode*` instead of 29 named concrete-class pointers. `RegisterRenderStreamNodes` / `RegisterTransparencyAndAONodes` / `RegisterPostProcessNodes` now do pure `graph.AddNode` sequences with no class-specific wiring. | Buckets are a graph execution strategy owned by graph-native nodes. | Done | — |
| Runtime selection logic | Pass execution resolves canonical framebuffer targets and canonical secondary textures from setup-owned state rather than re-selecting them from the blackboard (full audit in row text and `## Pipeline-Builder Pass-Centric Audit`). Per-pass selection is closed: no direct `ResolveFramebuffer(board->...)`, no direct `ResolveTexture(board->...)`, no execute-time `GetBlackboard()` call sites left under `OloEngine/src/OloEngine/Renderer/Passes`. Frontend/routing cleanup landed alongside the pipeline-builder slices on 2026-05-11 — downstream consumers now discover upstream producers by resource base name through the graph's latest-version map, not through typed pass-pointer setters. | Input selection is resolved during setup; execute records commands against already-decided handles. | Done | — |
| Topology derivation robustness | Dependency derivation is strong, semantic-ordering diagnostics ignore redundant transitive edges, production-shaped tests mirror real `DependsOnPass(...)` contracts, and the Phase A audit (`## Topology Robustness Audit (2026-05-10)`) plus Phase B slices have pinned every same-resource writer chain in `OloEngine/src/OloEngine/Renderer/Passes`: post-process linear chain via the producer-owned-seam pattern (15 seams), `SceneColor` RMW chain via name-based `builder.DependsOnPreviousWriter("SceneColor")` (replaced the typed-pointer setter on 2026-05-11), and the `OITAccum`/`OITRevealage` Clear+RMW chain via `builder.DependsOnPreviousWriter("OITAccum")` + a hardcoded `DependsOnPass("OITPreparePass")` on each OIT contributor. Registration order in the scene/transparency/post builders is no longer load-bearing for production correctness. The remaining gap is future-proofing the primitive set (a feedback-aware aliasing-version handle would give frame-capture per-modifier visibility on shared RMW resources, but it's a debug-affordance argument rather than a correctness one). | Ordering correctness should come from resource or explicit dependency declarations, not registration order quirks. | Done | — |
| Barrier model | The barrier/transition planner is now its own module (`RenderGraphBarrierPlanner.h/.cpp`, landed 2026-05-11) with value-type inputs (`PlanInput { ExecutionOrder, PassAccessDeclarations, IsPassReachable }`) and outputs (`PlanResult { PlannedBarriers, PassBarrierFlags, Diagnostics }`). `RenderGraph::ComputeBarrierPlan` and `RenderGraph::GetResourceTransitions` are now thin delegating wrappers (~10 lines each). The GL backend executor remains decoupled: it consumes `MemoryBarrierFlags` (abstract) via `RGCommandContext::MemoryBarrier(flags)` and the OpenGL impl in `OpenGLRendererAPI::MemoryBarrier` translates to `GL_*_BARRIER_BIT`. Phase 7 module split is partially landed (barrier planner extracted); remaining responsibilities still inside the `RenderGraph` god-class are listed under `## Phase 7 Compiler Module Split (2026-05-11)`. | Backend-neutral transition planning separated cleanly from backend execution. | Done | — |
| Async compute | Compiler and IR support exist (the `RenderGraphSubmissionPlan` module emits `BatchBegin` / `BatchEnd` IR commands with wait/signal/input/output dependency metadata); the GL path replays serially and emits debug markers around each batch. True async overlap requires a multi-queue backend and is accepted out-of-scope per `## What Is Realistically Out of Scope for the GL Milestone`. The graph compiler is ready for an explicit backend to consume the same IR with real cross-lane semaphores. | True async on explicit backends; conservative single-queue replay on GL. | Done (GL-bounded) | — |
| Imported vs transient boundary | Current-frame blackboard roots and current-frame AO technique scratch stay on the graph-owned / explicit-external-backing side (`SceneColor`, deferred G-buffer, shadow maps, AO, OIT, the post chain, `GTAODenoisePing/Pong`, `GTAOEdge`, `HZBDepth`, `SSAORaw`, and `SSAOBlur`). The remaining boundary work is about deliberate multi-frame / externally-backed state (histories, IBL, backbuffer), not hidden current-frame scratch — accepted per the Final Recommendation. | External resources are rare and explicit; scratch and intermediate resources are graph-owned. | Done | — |
| Graph debugability | Good diagnostics already exist, dump/lifetime surfaces distinguish externally-backed transients from imports and pool-materialized transients, and the debugger/DOT export keep generic `RenderGraphNode` entries visible. | Keep and expand diagnostics to cover authoring misuse and extraction roots. | Done | — |
| Backend portability | The Phase 7 module split (landed 2026-05-11 — 2026-05-12 over eight slices, see `## Phase 7 Compiler Module Split`) extracted every compiler stage out of the `RenderGraph` god-class into its own backend-free module with value-type inputs/outputs: barrier planner, reachability scan, hazard validator, resource registry (Phase A), transient planner, submission-plan builder + async scheduler, plan executor (submission-IR walk), and handle allocator (Phase B reconciliation + slot allocation). `RenderGraph.cpp` is now an orchestration facade (-1,485 lines net since slice 1; 7,517 → 6,032) and each module can be replaced or unit-tested in isolation. The OpenGL backend binding lives one layer deeper inside `RGCommandContext::MemoryBarrier` → `OpenGLRendererAPI`. A future explicit backend (Vulkan / DX12) consumes the same submission IR + transition plan without touching any planner. | Clear split between graph compiler, scheduler, transition planner, allocator, and backend executor. | Done | — |

## Topology Robustness Audit (2026-05-10)

Generated as Phase A of the topology robustness workstream. Walks every same-resource writer chain in `OloEngine/src/OloEngine/Renderer/Passes` and classifies how its ordering is pinned today. Resources with multiple producers across distinct passes are the interesting cases — single-producer resources and intra-pass scratch are listed for completeness so the pattern coverage is auditable.

| Resource | Writers (in current registration order) | Pinning today | Status | Proposed fix |
|---|---|---|---|---|
| `SceneColor` (deferred path) | `DeferredLighting` (write) → `ForwardOverlay` (RMW) → `Foliage` (RMW) → `Decal` non-OIT (RMW) → `Water` (RMW) → `Particle` non-OIT (RMW) → `OITResolve` (RMW) | Explicit `DependsOnPass(previous_writer)` on each modifier (set by `RenderPipelineBuilderScene::RegisterRenderStreamNodes` via `SetPreviousSceneColorWriter`) + `AllowFeedback(SceneColor)` self-edges on each modifier | **Pinned (2026-05-10)** | None — see Phase B note below |
| `SceneColor` (forward path) | `Scene` (write) → `Foliage` (RMW) → `Decal` non-OIT (RMW) → `Water` (RMW) → `Particle` non-OIT (RMW) → `OITResolve` (RMW) | Same mechanism as deferred path (no `ForwardOverlay` in the chain) | **Pinned (2026-05-10)** | None |
| `OITAccum` | `OITPrepare` (Clear) → `Decal` OIT path (RMW) → `Particle` OIT path (RMW) | Explicit `DependsOnPass("OITPreparePass")` on each contributor's OIT path + `DependsOnPass("DecalPass")` on `Particle` via `SetPreviousOITContributor` (set by `RenderPipelineBuilderTransparency::RegisterTransparencyAndAONodes`) + `AllowFeedback(OITAccum)` self-edges on contributors | **Pinned (2026-05-10)** | None — the `OITPreparePass` name is hardcoded in both contributors' OIT path because there is exactly one OIT-prep pass; the contributor-to-contributor edge uses the same setter pattern as the SceneColor chain. |
| `OITRevealage` | `OITPrepare` (Clear) → `Decal` OIT path (RMW) → `Particle` OIT path (RMW) | Same mechanism as `OITAccum` (the `DependsOnPass` edges pin both resources together) | **Pinned (2026-05-10)** | None |
| `OITDepthAttachment` | `OITPrepare` (write) | Single producer; `Decal` and `Particle` declare explicit `Read(OITDepthAttachment, RenderTargetRead)` | **Already pinned** | None |
| `AOBuffer` | `SSAO` OR `GTAO` (mutually exclusive at runtime) | Single active producer per frame + explicit `Read(AOBuffer, ShaderSample)` from `AOApply` and `DeferredLighting` | **Already pinned** | None |
| `ShadowMapCSM` | `Shadow` (one write per cascade) | Single producer pass; layer writes are intra-`Setup()` with `Layer(cascade)` subresource | **Already pinned** | None — intra-pass |
| `ShadowMapSpot` | `Shadow` (one write per spot light) | Single producer pass; layer writes are intra-`Setup()` with `Layer(light)` subresource | **Already pinned** | None — intra-pass |
| `GBufferAlbedo`/`GBufferNormal`/`GBufferEmissive` (and MS variants), `SceneNormals`, `VelocityMS`, `SceneDepthMS` | `DeferredOpaqueDecal` (TransferDest blend) | Single producer pass; G-buffer-prep transfers happen entirely inside `DeferredOpaqueDecal::Setup()` | **Already pinned** | None — intra-pass multi-target write |
| `HZBDepth` (mip pyramid) | `GTAO` (intra-pass, per-mip with `AllowFeedback`) | Single producer pass; the mip chain is fully internal to `GTAORenderPass::Setup()` | **Already pinned** | None — intra-pass |
| `GTAOEdge`, `GTAODenoisePing`, `GTAODenoisePong` | `GTAO` (intra-pass scratch with `AllowFeedback`) | Single producer pass | **Already pinned** | None — intra-pass |
| `SSAORaw`, `SSAOBlur` | `SSAO` (intra-pass scratch with `AllowFeedback`) | Single producer pass | **Already pinned** | None — intra-pass |
| `JFAPing`, `JFAPong` | `SelectionOutline` (intra-pass ping-pong with `AllowFeedback`) | Single producer pass | **Already pinned** | None — intra-pass |
| `FogHalfRes` | `Fog` (intra-pass, half-res ray-march scratch with `AllowFeedback`) | Single producer pass | **Already pinned** | None — intra-pass |
| `BloomMips[i]` | `Bloom` (intra-pass mip chain with `AllowFeedback`) | Single producer pass; the threshold/downsample/upsample chain is fully internal to `BloomRenderPass::Setup()` | **Already pinned** | None — intra-pass |
| `WaterRefraction` | `Water` (intra-pass copy from `SceneColor` with `AllowFeedback`) | Single producer pass | **Already pinned** | None — intra-pass |
| `SSSColor`, `AOApplyColor`, `BloomColor`, `DOFColor`, `MotionBlurColor`, `TAAColor`, `PrecipitationColor`, `FogColor`, `ChromAbColor`, `ColorGradingColor`, `ToneMapColor`, `VignetteColor`, `FXAAColor`, `SelectionOutlineColor`, `UIComposite` | One producer per resource; consumers `SetPreferred*Source(producer)` | **Versioned-write seam** (`Resource@PassName` via `WriteNewVersion(...)` + `SetPreferred*Source(...)`) | **Already pinned** | None — see `docs/rendergraph-versioned-write-seam-adoption.md` |
| `Backbuffer` | `Final` (terminal write) | No consumers downstream of `Final` | **Already pinned** | None — terminal |

### Phase B progress

**Slice 1 (landed 2026-05-10) — `SceneColor` RMW chain pinned via explicit `DependsOnPass`.** On closer inspection the feedback-version seam I sketched in the original audit was the wrong tool: the existing `WriteNewVersion(...)` allocates a fresh transient framebuffer per version, but the six SceneColor modifiers all bind the same canonical SceneColor framebuffer object — versioning each modifier's output would require a new aliasing primitive (a versioned graph handle that doesn't allocate new backing). For SceneColor specifically, that primitive isn't needed: the goal is purely ordering, and the graph already knows the resource reads/writes — what's missing is just the tiebreaker between consecutive RMW writers. Each modifier got a `SetPreviousSceneColorWriter(RenderPass*)` setter (analogous to the post-chain `SetPreferred*Source` pattern) and now emits `builder.DependsOnPass(previous->GetName())` in `Setup()` only when it actually writes SceneColor that frame. The chain wiring lives in `RenderPipelineBuilderScene::RegisterRenderStreamNodes` and threads through both render-stream and transparency-stage modifiers. New focused test `RenderGraphBuildDiagnostics.SceneColorRMWChainIsRegistrationOrderIndependentWithExplicitDependsOnPass` registers the seven-pass chain in reverse and asserts zero registration-order-sensitivity diagnostics plus correct chain execution order.

**Slice 2 (landed 2026-05-10) — `OITAccum` + `OITRevealage` chain pinned via explicit `DependsOnPass` edges.** What looked like a one-line fix turned out to be a three-writer WAW chain (OITPrepare Clear → Decal RMW → Particle RMW), so just adding the contributor-to-contributor edge wasn't enough — the OITPrepare → contributor ordering also had to be pinned. Both `DecalRenderPass::Setup()` and `ParticleRenderPass::Setup()` now emit `builder.DependsOnPass("OITPreparePass")` in their OIT path (the name is stable — there is exactly one OIT-prep pass), and `ParticleRenderPass` also gets a `SetPreviousOITContributor(...)` setter that the transparency builder wires to `Decal`. New focused test `RenderGraphBuildDiagnostics.OITAccumContributorChainIsRegistrationOrderIndependentWithExplicitDependsOnPass` registers the three-pass chain in reverse and asserts zero registration-order-sensitivity diagnostics plus correct execution order.

**Phase B is complete.** All three writer chains identified by Phase A are now pinned (SceneColor RMW + OITAccum + OITRevealage). The audit table's `Status` column reflects this.

### Note on the deferred aliasing-version primitive

The audit originally floated `WriteFeedbackVersion(...)` (or extending `WriteNewVersion`) for the SceneColor chain. Slice 1 made that unnecessary by using explicit ordering edges instead — same correctness outcome, no new primitive, smaller blast radius. The case for the aliasing primitive remains open for future work where downstream consumers would benefit from per-modifier debug visibility (e.g. frame-capture wanting to inspect `SceneColor@ForwardOverlay` distinctly from `SceneColor@OITResolve`), but that's a debug-affordance argument rather than a correctness one and isn't urgent.

## Resource Declaration Source Audit (2026-05-11)

Generated as Phase A of the resource-declaration-source workstream. The gap-analysis row claimed "a smaller set of execute-time fallback paths still survives" — the audit walks every pass under `OloEngine/src/OloEngine/Renderer/Passes` and classifies each resource access pattern as either (a) setup-declared and execute-resolved-through-graph (good), (b) setup-declared but execute-uses-raw-IDs (the "fallback path" — graph knows the read for derivation purposes but the actual binding bypasses the graph), or (c) execute-only access with no setup declaration (worst — graph doesn't know about it at all).

| Pass | Resource | Setup declares? | Execute resolves through graph? | Status | Proposed fix |
|---|---|---|---|---|---|
| `DeferredLightingPass` | G-buffer attachments (Albedo/Normal/Emissive/Depth/Velocity, MSAA + resolved variants) | Yes (`builder.Read(m_SelectedInputs.X, ShaderSample)`) | Yes (`context.ResolveTexture(handle)` via `m_SelectedInputs.X`) | **Already pinned** | None |
| `DeferredLightingPass` | `ShadowMapCSM`, `ShadowMapSpot`, `ShadowMapPoint[4]` | Yes (`builder.Read(blackboard.ShadowMapCSM/Spot/Point[i], ShaderSample)`) | Yes (`context.ResolveTexture(m_SelectedInputs.ShadowMap*)`) | **Pinned (2026-05-11)** | None |
| `DeferredLightingPass` | `IrradianceMap`, `PrefilterMap`, `BrdfLut` (IBL textures, graph-imported from `Renderer3D` long-lived state) | Yes (`builder.Read(blackboard.X, ShaderSample)`) | Yes (`context.ResolveTexture(m_SelectedInputs.IrradianceMap/PrefilterMap/BrdfLut)`) | **Pinned (2026-05-11)** | None |
| `DeferredLightingPass` | `AOBuffer` | Yes (`builder.Read(blackboard.AOBuffer, ShaderSample)`) — stored in `m_SelectedInputs.AOBuffer` as of 2026-05-11 | Not directly bound in Execute (the AO sampler is wired through `AOApplyPass` before deferred lighting samples lit colour) | **Pinned (2026-05-11)** | None |
| `DeferredLightingPass`, `SceneRenderPass`, `ShadowRenderPass`, et al. | `Renderer3D::GetShadowMap()` for camera UBO setup, shadow-map config queries | Not a resource read | N/A | **Legitimate auxiliary access** (configuration / per-frame UBO setup, not GPU resource binding) | None |
| `Renderer3D::GetRendererSettings()`, `GetPostProcessSettings()`, `GetViewMatrix/ProjectionMatrix`, `GetShaderLibrary().Get(...)` | Settings / camera / shader lookups | N/A | N/A | **Legitimate frame-state access** | None — not resource bindings, no graph contract to declare |
| All other production passes | All resources sampled in Execute | Yes (`builder.Read` / `builder.Write` in Setup, handle stored in pass member) | Yes (`context.Resolve*` from stored handle) | **Already pinned** | None |

### Findings summary

The "execute-time fallback paths" mentioned in the gap-analysis row are concentrated in **one pass: `DeferredLightingPass`**. Six texture bindings (3 IBL + CSM + Spot + 4 Point) declare reads in Setup but bind raw IDs in Execute. Every other production pass either resolves through the graph or accesses non-resource state legitimately (settings, matrices, shader library).

The practical impact today is limited (the graph correctly tracks the read for derivation/culling/lifetime purposes; the actual GL bind matches the raw ID either way since both point to the same renderer-owned object). The cleanup is mostly about consistency — moving every resource-bind in Execute through the graph's resolve path means future barrier/transition logic, debug-capture, and resource-validation infrastructure all see the binding. It also closes the "graph thinks pass X reads Y but pass X binds raw Z" footgun for future readers.

### Phase B (landed 2026-05-11)

One slice. `DeferredLightingPass` extended its `SelectedInputs` struct with `IrradianceMap` / `PrefilterMap` / `BrdfLut` / `ShadowMapCSM` / `ShadowMapSpot` / `ShadowMapPoint[4]` / `AOBuffer` handles, populated them alongside the existing `builder.Read(...)` declarations in `Setup()`, and replaced the raw `Renderer3D::GetGlobal*MapID()` and `Renderer3D::GetShadowMap().Get*RendererID()` calls in Execute with `context.ResolveTexture(handle)`. The settings-related `Renderer3D::Get*` calls (IBL intensity, cascade debug flag, light-probe toggle) stayed — they're configuration, not resource bindings. The full RenderGraph + diagnostics + path-switch + hazard test suite (341 tests) passes unchanged, and the resolved IDs match the raw `Renderer3D::Get*` values bit-for-bit since the graph imports the same renderer-owned textures (see `RenderPipeline.cpp:1703-1719` plus `PopulateBlackboard`'s shadow-map import path).

**Phase B is complete.** Every resource access in `OloEngine/src/OloEngine/Renderer/Passes` Execute bodies now resolves through the graph's `context.Resolve*` path from a setup-stored handle. The gap-analysis row's "smaller set of execute-time fallback paths still survives" claim no longer applies — the only `Renderer3D::Get*` calls left in Execute bodies are legitimate configuration / camera-state / shader-library accesses, none of which are GPU resource bindings.

## Pipeline-Builder Pass-Centric Audit (2026-05-11)

Generated as Phase A of the pipeline-builder cleanup workstream. The `RenderPipelineBuilder*` frontend still names concrete pass classes when wiring three categories of pass-to-pass relationships:

| Wiring | Mechanism | Files |
|---|---|---|
| SceneColor RMW chain | `SetPreviousSceneColorWriter(RenderGraphNode*)` on `ForwardOverlay` / `Foliage` / `Decal` / `Water` / `Particle` / `OITResolve`; each modifier's `Setup` polls the stored pointer and emits `builder.DependsOnPass(pointer->GetName())`. | `RenderPipelineBuilderScene.cpp` lines 30-61 (6 setter calls) plus 6 pass headers. |
| OIT contributor chain | `Particle->SetPreviousOITContributor(Decal)`; `Particle::Setup` polls the pointer and emits the equivalent edge. | `RenderPipelineBuilderTransparency.cpp` lines 32-33 plus `ParticleRenderPass.h`. |
| Post-process preferred-source ladder | `Set Preferred{Bloom,DOF,MotionBlur,TAA,Precipitation,Fog,ChromAberration,ColorGrading,ToneMap,Vignette,FXAA,Vignette,SelectionOutline,UIComposite,SSS,AOApply}Source(UpstreamClass*)` on each downstream post-process pass. Setup polls each preferred source's `GetPrimaryOutput*Handle()` to pick the active upstream producer for the current frame, falling back to blackboard handles. | `RenderPipelineBuilderPost.cpp` lines 49-169 (~90 setter calls across 11 downstream passes). |
| `RenderPipelinePassInputs` | 29 typed concrete-class pointers (`SceneRenderPass*`, `ShadowRenderPass*`, ...). | `RenderPipelineBuilder.h`. |

### Phase B slice 1 — SceneColor RMW + OIT contributor chain (landed 2026-05-11)

The first two rows above are now resolved. `RenderGraph` tracks the most-recent writer per resource base name (`m_LastWriterPassNameByResource`, populated during BuildFrameGraph's Setup loop), exposed via `RenderGraph::GetLastWriterPassName(resource)` plus a `RGBuilder::DependsOnPreviousWriter(resource)` convenience. Each RMW modifier's `Setup` now calls `builder.DependsOnPreviousWriter("SceneColor")` (or `"OITAccum"` for the OIT contributor chain) to emit the predecessor edge from the graph's tracker rather than from a typed pointer.

To make name-based discovery resolve the producer correctly, `BuildRenderPipelineGraph` now registers `RegisterSceneAndLightingNodes` (Shadow + Scene + deferred opaque-decal + deferred lighting) **before** `RegisterRenderStreamNodes` (the SceneColor modifiers). With that ordering, every modifier's Setup runs after its predecessor's Setup completes and the last-writer map already contains the correct pass name.

Removed in the same slice:
- `SetPreviousSceneColorWriter` setters + `m_PreviousSceneColorWriter` storage from `ForwardOverlay`, `Foliage`, `Decal`, `Water`, `Particle`, `OITResolve`.
- `SetPreviousOITContributor` setter + `m_PreviousOITContributor` storage from `Particle`.
- The 7-setter SceneColor RMW wiring at the top of `RenderPipelineBuilderScene::RegisterRenderStreamNodes`.
- The OIT contributor wiring in `RenderPipelineBuilderTransparency::RegisterTransparencyAndAONodes`.

Acceptance: full test suite (2441/2441) passes; no remaining references to either setter family in `OloEngine/src`. The remaining row in the audit table (post-process preferred-source ladder + `RenderPipelinePassInputs` typed-pointer struct) is the next slice's target.

### Phase B slice 2 — post-process preferred-source ladder (landed 2026-05-11)

The third row of the audit is now resolved. The preferred-source ladder ran the same shape across 14 downstream post-process passes: each `Setup` polled a chain of typed pointers (`m_PreferredAOApplySource`, `m_PreferredBloomSource`, …) for whichever upstream had a valid `GetPrimaryOutputFramebufferHandle()`/`TextureHandle()`, then fell back through the canonical blackboard imports via the variadic `ReadFirstValidFramebufferTextureInputForPass`. The wiring was 80+ `SetPreferred*Source(upstream)` calls (~135 lines) in `RenderPipelineBuilderPost::RegisterPostProcessNodes`.

Replaced with a name-based selector. `RGBuilder` exposes `GetGraph()` so free helpers in `RenderPipelineBuilderInternal` can look up the latest versioned framebuffer/texture handle for a base name (and fall back to the canonical import). The new helper `ReadFirstValidVersionedInputForPass(builder, pass, { MakeCandidateBaseNames(fb, tex), … })` iterates a per-pass priority list of resource base names, picks the first pair whose handles are valid, stores them on the pass via `SetPrimaryInput{Framebuffer,Texture}Handle`, and emits a `ShaderSample` Read on the chosen texture. The graph's automatic dependency derivation (against the versioned name returned by `WriteNewVersion`) emits the ordering edge — no explicit `DependsOnPass` needed.

Knock-on cleanup:
- 14 pass `.cpp` files migrated (`AOApply` through `Final`). Each Setup body now invokes `ReadFirstValidVersionedInputForPass` once with the appropriate priority list, instead of 1–11 typed setter polls plus a variadic fallback call.
- 14 pass `.h` headers stripped of every `SetPreferredXxxSource(RenderGraphNode*)` setter and every `m_PreferredXxxSource` pointer field.
- `RenderPipelineBuilderPost::RegisterPostProcessNodes` collapsed to `graph.AddNode(...)` calls only — the entire 80-setter ladder is gone.
- 230 `SetPreferredXxxSource(...)` calls removed from `OloEngine/tests/Rendering/RenderGraphTest.cpp`.
- Bug fix in `RenderGraph::CreateFramebufferAttachmentView`: when the view name has an explicit `@version` qualifier the producer-versioned attachment view now registers in `m_LatestTextureHandlesByBaseName` so `GetTextureHandle("BloomColorTexture")` (etc.) resolves to the producer's view instead of the canonical import. The old typed-pointer path went around this map and masked the gap; the name-based path requires it.

Acceptance: full test suite (2441/2441) passes; no remaining `SetPreferred*` or `m_Preferred*` references anywhere under `OloEngine/src/OloEngine/Renderer/Passes`. The remaining audit row (`RenderPipelinePassInputs` typed-pointer struct in `RenderPipelineBuilder.h`) is the next slice's target — that struct still carries 29 named concrete-class pointers (`SceneRenderPass*`, `ShadowRenderPass*`, …) and forces every caller of `BuildRenderPipelineGraph` to instantiate one of every concrete pass class. Collapsing it into a generic node container is mostly a downstream API refactor in the renderer.

### Phase B slice 3 — `RenderPipelinePassInputs` type-erasure (landed 2026-05-11)

The fourth audit row is now resolved. All 29 fields in `RenderPipelinePassInputs` are typed `RenderGraphNode*` instead of concrete pass classes. The pipeline builder no longer forward-declares any of the concrete pass types, the stage `.cpp` files no longer include the per-pass headers (collectively dropped 33 `#include` lines: 10 from Scene, 10 from Transparency, 16 from Post-process, plus the `CommandBufferRenderPass.h` include from the internal header), and `AddExistingNode` now takes `RenderGraphNode*` directly so it doesn't enforce the bucket-backed constraint at the static type level (the constraint still holds dynamically — a node either uses a bucket or it doesn't).

Compilation impact: callers populating the struct via `inputs.Passes.Scene = FrameCorePasses.Scene.Raw();` keep working through the implicit upcast (`SceneRenderPass* → RenderGraphNode*`); no caller-side changes needed in `RenderPipeline.cpp` or `Renderer3DInternal.h`. The pipeline builder has gone from "29 concrete pass classes + 13 stage `#include`s" to "1 base class + 0 stage `#include`s for pass headers".

Acceptance: full test suite (2441/2441) passes. With slices 1–3 landed, the pipeline-builder cleanup workstream is essentially complete:
- `RenderPipelineBuilderScene.cpp`, `RenderPipelineBuilderTransparency.cpp`, `RenderPipelineBuilderPost.cpp` now collectively do `graph.AddNode` calls + an `AOTechnique` enum switch, nothing more.
- `RegisterPostProcessNodes` and `RegisterRenderStreamNodes` are pure node-registration sequences with no class-specific setters.
- The remaining `RenderPipelinePassInputs` struct could be further generalised into a generic name→node map, but that is purely cosmetic — it would reshape the call site in `Renderer3D::RenderPipeline::BuildInputs` without changing graph semantics.

## Phase 7 Compiler Module Split (2026-05-11)

`RenderGraph.cpp` was a 7,517-line god-class implementing every compiler stage (resource registry, transient planner, reachability/culling, hazard validation, barrier planner, scheduler, submission-plan builder, backend executor). The Phase 7 plan splits each stage into its own module with value-type inputs/outputs so the compiler becomes backend-portable and replaceable piece by piece.

### Slice 1 — barrier/transition planner extracted (landed 2026-05-11)

`OloEngine/src/OloEngine/Renderer/RenderGraphBarrierPlanner.h` and `.cpp` host the planner as a `OloEngine::RenderGraphBarrierPlanner` namespace. Public surface:
- `ResolveProducerBarrierFlags(RGWriteUsage) -> MemoryBarrierFlags` and `ResolveConsumerBarrierFlags(RGReadUsage) -> MemoryBarrierFlags` — pure flag-mapping helpers.
- `ComputePlan(PlanInput) -> PlanResult` — consumes execution order + per-pass access declarations + a `std::function<bool(const std::string&)> IsPassReachable` reachability oracle, returns the planned barriers, per-pass flag union, and diagnostics. The pass-reachable check is the only RenderGraph-aware injection point.
- `BuildResourceTransitions(TransitionInput) -> std::vector<RenderGraph::ResourceTransition>` — consumes the planned barriers + execution order + per-pass declarations + a `std::function<RenderGraphPassWorkType(const std::string&)> GetPassWorkType` lookup for cross-lane annotation.

The module is backend-free (no `RGCommandContext`, no `OpenGLRendererAPI`, no GL). The OpenGL backend remains the `OpenGLRendererAPI::MemoryBarrier` translator at the bottom of the call stack — `RenderGraph::Execute` invokes `RGCommandContext::MemoryBarrier(flags)` which dispatches to `RenderCommand::MemoryBarrier` → `OpenGLRendererAPI::MemoryBarrier` → `glMemoryBarrier(GLbitfield)`.

Impact in `RenderGraph.cpp`:
- 200-line `ComputeBarrierPlan` body → 10-line delegating wrapper.
- 100-line `GetResourceTransitions` body → 8-line delegating wrapper.
- 40-line `ResolveProducerBarrierFlags` / `ResolveConsumerBarrierFlags` static methods → 3-line delegating wrappers (kept for ABI/header compatibility).
- `RenderGraph.cpp` line count: 7,517 → 7,214 (-303 lines net after slice; +357 in the new module, -660 in the god-class).

Acceptance: full test suite (2441/2441) passes; the barrier planner now has zero references to `RenderGraph`'s internal state beyond its declared `PlanInput` / `TransitionInput` value types.

### Slice 2 — reachability scan extracted (landed 2026-05-11)

`OloEngine/src/OloEngine/Renderer/RenderGraphReachability.h` and `.cpp` host the pure BFS / iterative read→writer expansion as a `OloEngine::RenderGraphReachability` namespace. Public surface is a single `ComputeReachableSet(ScanInput) -> std::unordered_set<std::string>`:
- Inputs: `HasExplicitFinalPass`, `FinalPassName`, `InsertionOrder` (span), `PassAccessDeclarations` (map ref), `Dependencies` (map ref), and `ExtractedResourceNames` (span of resource names from texture extracts / framebuffer extracts / temporal-history contracts / external-sink contracts).
- Output: the reachable pass set.

The mutating tail of `RenderGraph::ComputeReachability` — refreshing `m_TemporalHistoryContracts` / `m_ExternalTextureSinkContracts`, folding side-effecting passes back into the reachable set, building `m_CulledPasses`, and emitting the digest log — stays on the graph because it touches state outside the reachability scope. The wrapper now:
1. Gathers extraction / contract roots into a `std::vector<std::string>`.
2. Calls the module to compute the reachable set.
3. Runs the Refresh + culling + diagnostic-emit tail.

Impact in `RenderGraph.cpp`:
- 200-line `ComputeReachability` body → ~100-line wrapper (gather roots + delegate + tail).
- 130 lines of pure BFS logic moved to the module.

Acceptance: full test suite passes (2441 ran; 2436 pass + the 5 known-flaky `PerfRegressionTest.*` timing tests that pass in isolation). Reachability module has zero references to `RenderGraph`'s internal state beyond its declared `ScanInput` value type.

### Slice 3 — resource-hazard validator extracted (landed 2026-05-11)

`OloEngine/src/OloEngine/Renderer/RenderGraphHazardValidator.h` and `.cpp` host the validator as a `OloEngine::RenderGraphHazardValidator` namespace. Public surface is a single `Validate(ValidatorInput) -> std::vector<RenderGraph::Hazard>`. Inputs:
- Predicates / functors injected by the graph: `IsPassReachable`, `ResolveTexture`, `ResolveBuffer`, `ResolveFramebuffer`.
- Already-up-to-date topology: `ExecutionOrder` span + `Dependencies` map ref.
- Setup-time declarations: `PassAccessDeclarations` + `PassFeedbackDeclarations` map refs.
- Registry-stage outputs: `RegistryDiagnostics` span + `RegisteredResources` span.

The validator performs five passes in order: (1) forward registry-diagnostics filtered for reachable passes, (2) build transitive dependency closure, (3) same-pass feedback validation (Feedback hazards for read+write overlap without `AllowFeedback`), (4) imported-resource lifetime misuse check (uses the resolve functors), (5) cross-pass RAW/WAW/WAR validation.

The wrapper in `RenderGraph::ValidateResourceHazardsInternal` keeps the registry-rebuild + topology-rebuild + cycle-hazard emission (those mutate graph state), then calls the module with the populated inputs.

Impact in `RenderGraph.cpp`:
- 320-line `ValidateResourceHazardsInternal` body → 40-line delegating wrapper.
- `RenderGraph.cpp` line count: 7,214 → 6,837 (-377 lines net; +321 in the new module).

Acceptance: full test suite (2441/2441) passes. Hazard validator module has zero references to `RenderGraph`'s internal state beyond its declared `ValidatorInput` value type.

### Slice 4 — resource-registry builder (Phase A) extracted (landed 2026-05-11)

`OloEngine/src/OloEngine/Renderer/RenderGraphResourceRegistry.h` and `.cpp` host the pure registry build as a `OloEngine::RenderGraphResourceRegistry` namespace. Public surface is a single `Build(BuildInput) -> BuildResult`:
- Inputs: const refs to `ImportedResources` / `TransientResourceDescs` / `TextureViewResourceDescs` descriptor maps, the `InsertionOrder` span, the `PassAccessDeclarations` map ref, and an `IsExternallyBackedTransientResource(name)` predicate functor.
- Output: `BuildResult { Registry, Sorted, Diagnostics }` — the canonical name→ResourceInfo map, the lexicographically-sorted vector view downstream stages consume, and the kind-mismatch diagnostics produced during access-declaration walking.

The split between Phase A (this module) and Phase B (typed-handle slot reconciliation) is deliberate: Phase A is a pure data transformation that doesn't mutate any handle allocator, while Phase B touches the per-type handle-slot tables (`m_TextureHandleSlots`, etc.), free-index lists, physical-resource arrays, and lookup maps. Phase B stays on the graph because the handle allocators are read by many other graph subsystems by raw index, and reorganising that is a separate slice (it would naturally land alongside the eventual handle-allocator module).

The wrapper in `RenderGraph::EnsureResourceRegistryBuilt` keeps Phase B unchanged — it just gathers Phase A's outputs into the graph's registry members and runs the existing reconciliation loops.

Impact in `RenderGraph.cpp`:
- 150-line Phase A body → 30-line delegating wrapper.
- `RenderGraph.cpp` line count: 6,837 → 6,723 (-114 lines net; +150 in the new module).
- Cumulative Phase 7 reduction from the original god-class: **7,517 → 6,723 (-794 lines** of in-class implementation, replaced by four focused modules totalling ~970 lines).

Acceptance: full test suite (2441/2441) passes. Resource-registry builder has zero references to `RenderGraph`'s internal state beyond its declared `BuildInput` value type.

### Slice 5 — transient-resource planner extracted (landed 2026-05-11)

`OloEngine/src/OloEngine/Renderer/RenderGraphTransientPlanner.h` and `.cpp` host the planner as a `OloEngine::RenderGraphTransientPlanner` namespace. Public surface:
- Pure descriptor helpers (no graph state): `BuildAliasGroup(desc)`, `EstimateBytes(desc)`, `IsAllocatable(desc)`, `GetSkipReason(desc)`.
- `ComputePlan(PlanInput) -> std::vector<RenderGraph::TransientPlanEntry>` — runs the per-resource lifetime scan, classifies each descriptor into allocatable / skip-with-reason, canonically sorts the plan (alias-group, first-use-pass-index, resource-name), and assigns alias slots per group so non-overlapping lifetimes share pool-allocated backing.

Inputs come in as the transient descriptor map ref + execution order span + access declarations map ref + two functors (`IsPassReachable`, `IsExternallyBackedTransientResource`). Output is the canonical `TransientPlanEntry` vector.

The runtime `TransientPool` that materialises the assigned slots stays on the graph — this module is pure planning, not resource lifetime ownership. Two existing `RenderGraph::ToImageFormat` / `ToFramebufferFormat` static helpers had to move from the private section to public so the module can call them without friending; they remain `static` and have no instance dependency.

The wrapper in `RenderGraph::RebuildTransientPlan` is now an 8-line delegate. The four `RenderGraph::Build/Estimate/Is/GetTransient...` static methods become 3-line forwarders for ABI compatibility.

Impact in `RenderGraph.cpp`:
- ~270 lines of in-class implementation moved to the module (lifetime scan + plan classification + alias-slot assignment + four descriptor helpers).
- `RenderGraph.cpp` line count: 6,723 → 6,452 (-271 lines net; +327 in the new module).
- Cumulative Phase 7 reduction since slice 1: **7,517 → 6,452 (-1,065 lines** of god-class body), replaced by five focused modules totalling ~1,300 lines.

Acceptance: full suite — 2441 ran; 2436 pass + 5 `PerfRegressionTest.*` failures (all pass in isolation; known timing flake under full-suite load).

### Slice 6 — submission-plan builder & async-compute scheduler extracted (landed 2026-05-11)

`OloEngine/src/OloEngine/Renderer/RenderGraphSubmissionPlan.h` and `.cpp` host the scheduler + IR builder as a `OloEngine::RenderGraphSubmissionPlan` namespace. Two public entry points:
- `ComputeBatches(BatchesInput) -> std::vector<AsyncComputeBatch>` — groups consecutive `AsyncComputeCandidate` passes into batches, derives each batch's wait/signal nodes from the dependency graph + execution order, then scans before/after the batch range to compute input/output resource dependencies.
- `BuildPlan(PlanInput) -> std::vector<SubmissionCommand>` — walks the execution order and interleaves batch begin/end commands, memory-barrier commands (one per pass that has barriers planned against it), and pass commands. The resulting IR is what the backend executor replays verbatim.

Dependencies injected as functors:
- `IsGraphEntryAsyncComputeCandidate(name)` — used by `ComputeBatches`.
- `GetPassWorkType(name)` — used by `BuildPlan` to map work types to queue lanes.
- `ResolveNodePointer(name)` — used by `BuildPlan` to populate the `NodePointer` field; lets the executor invoke `Execute()` without re-walking the node lookup.

The wrappers in `RenderGraph::GetAsyncComputeBatches` and `RenderGraph::GetSubmissionPlan` are 8 and 14 lines respectively — they just gather the inputs and forward.

Impact in `RenderGraph.cpp`:
- ~330 lines moved to the module (190 for batch scheduling, 140 for the IR walk).
- `RenderGraph.cpp` line count: 6,452 → 6,146 (-306 lines net; +346 in the new module).
- Cumulative Phase 7 reduction since slice 1: **7,517 → 6,146 (-1,371 lines** of god-class body), replaced by six focused modules totalling ~1,700 lines.

Acceptance: full suite — 2441 ran; 2436 pass + 5 `PerfRegressionTest.*` failures (all pass in isolation; known timing flake under full-suite load).

### Slice 7 — submission-plan executor extracted (landed 2026-05-12)

`OloEngine/src/OloEngine/Renderer/RenderGraphPlanExecutor.h` and `.cpp` host the IR walk loop as a `OloEngine::RenderGraphPlanExecutor` namespace. Single public entry point:
- `ExecutePlan(ExecuteInput) -> std::vector<RenderGraph::ExecutionTiming>` — iterates the precomputed submission plan, dispatches each command kind (`BatchBegin` / `BatchEnd` / `MemoryBarrier` / `Pass`) to the abstract `RGCommandContext`, fires the optional batch-event + post-pass hooks, and returns per-pass CPU timings. Skipped passes (failing the `IsPassReachable` predicate or with no `NodePointer`) produce no timing entry.

The module is backend-free: every action dispatches through `RGCommandContext` methods. The OpenGL bindings stay one level deeper (in `OpenGLRendererAPI`). This closes the original architectural-cleanup goal — the planner stages, the submission IR, and the executor are all separable from any backend specifics.

Dependencies injected as functors / hook fields on the `ExecuteInput` struct:
- `IsPassReachable(name)` — culling check.
- `BatchEventHook` — fires on each `BatchBegin` / `BatchEnd`.
- `PostPassHook` + `GraphForPostPassHook` — fires after every `EndPass`, before the next command. The graph pointer is forwarded so debug-tooling hooks (like `RenderGraphFrameCapture`) can snapshot intermediate state.

The wrapper in `RenderGraph::Execute` keeps:
- Pre-execution prep (clear timings, reset placeholder-warned flags).
- Topology/plan rebuild gate (if `m_DependencyGraphDirty`: `UpdateDependencyGraph` → `ResolveFinalPass` → `ComputeReachability` → `ComputeBarrierPlan` → `RebuildTransientPlan` → cache `GetSubmissionPlan`).
- `MaterializeTransientResources`.
- The command-context setup/teardown around the executor call.
- `FlushExtractions` + transient-pool cleanup.

Impact in `RenderGraph.cpp`:
- ~65-line IR walk → 17-line delegating call.
- `RenderGraph.cpp` line count: 6,146 → 6,101 (-45 lines net; +79 in the new module).
- Cumulative Phase 7 reduction since slice 1: **7,517 → 6,101 (-1,416 lines** of god-class body), replaced by seven focused modules totalling ~1,820 lines.

Acceptance: full suite — 2441 ran; 2436 pass + 5 `PerfRegressionTest.*` failures (all pass in isolation; known timing flake under full-suite load).

### Slice 8 — handle allocator extracted (landed 2026-05-12)

`OloEngine/src/OloEngine/Renderer/RenderGraphHandleAllocator.h` hosts the Phase B registry-rebuild logic as a header-only `OloEngine::RenderGraphHandleAllocator` namespace with two templated free functions:
- `Reconcile<HandleT, SlotT>(handlesByName, slots, freeIndices, activeNames)` — kills handle entries whose names left the active set, bumps slot generations, returns indices to the free list.
- `Allocate<HandleT, SlotT, PhysicalT, MakeHandleFn>(name, handlesByName, slots, physicals, freeIndices, makeHandle)` — returns an existing handle by name or allocates a new slot (reusing a free index when available, growing otherwise) and keeps the parallel physical-resource array sized to the slot table.

Header-only because the body is templated and the three handle families (`Texture` / `Buffer` / `Framebuffer`) each instantiate with a different `SlotT` / `PhysicalT`. The wrapper in `RenderGraph::EnsureResourceRegistryBuilt` is now a straightforward sequence: gather active-name sets for each family, call `Reconcile` three times, then walk the registered resources calling `Allocate` per kind.

Impact in `RenderGraph.cpp`:
- ~100 lines of templated lambdas + their callers → 65 lines of plain orchestration.
- `RenderGraph.cpp` line count: 6,101 → 6,032 (-69 lines net; +107 in the new module header).
- Cumulative Phase 7 reduction since slice 1: **7,517 → 6,032 (-1,485 lines** of god-class body), replaced by eight focused modules totalling ~1,930 lines.

Acceptance: full suite — 2441 ran; 2436 pass + 5 `PerfRegressionTest.*` failures (all pass in isolation; known timing flake under full-suite load).

### Phase 7 wrap-up

With slice 8 landed the original Phase 7 plan is **fully complete**. Every compiler stage with a clear pure-data input/output contract has its own backend-free module:
- `RenderGraphBarrierPlanner` (slice 1) — transition planner + flag-resolution + resource-transition records.
- `RenderGraphReachability` (slice 2) — backward BFS from final pass + extract roots, iterative Read→Writer expansion.
- `RenderGraphHazardValidator` (slice 3) — RAW/WAW/WAR + feedback + imported-resource-lifetime hazards.
- `RenderGraphResourceRegistry` (slice 4) — pure registry build, kind-mismatch diagnostics, canonical sort.
- `RenderGraphTransientPlanner` (slice 5) — lifetime scan + alias-slot assignment + four descriptor helpers.
- `RenderGraphSubmissionPlan` (slice 6) — async-compute batch scheduler + submission-IR builder.
- `RenderGraphPlanExecutor` (slice 7) — backend-free IR walk loop dispatching to `RGCommandContext`.
- `RenderGraphHandleAllocator` (slice 8) — templated reconcile + allocate for the three handle families.

`RenderGraph.cpp` is now an orchestration facade that wires these modules together with the graph's mutable state (descriptor maps, dependency edges, handle slot tables, pools, hooks). The Phase 8 (OpenGL-backend hardening) and Phase 9 (legacy pass framework removal) workstreams from the original plan were already largely landed in earlier slices (see `## Legacy Declaration Removal` and `## Pipeline-Builder Pass-Centric Audit`).

## Legacy Declaration Removal (2026-05-11)

The static `GetDeclaredReads()` / `GetDeclaredWrites()` virtuals on `RenderGraphNode` — the pre-Setup declaration surface — were removed in this milestone along with every consumer in `RenderGraph.cpp`. The validator and registry now read from `m_PassAccessDeclarations` exclusively (the setup-time `builder.Read/Write` records). `ValidateResourceHazards()` and `ValidateCompiledResourceHazards()` auto-trigger `BuildFrameGraph()` so the access declarations are always materialised before validation; the previous "pre-build" code path that fell back to the static virtuals is gone.

Knock-on cleanups in the same milestone:
- `FindLegacyDeclaredResourceKind` helper removed.
- `FrameBuildStats::LegacyDeclarationPasses` / `LegacyDeclarationOnlyPasses` fields removed (and their JSON/log emitters in `DumpToJson` + `Renderer3DFrameExecution`).
- `NodeAuthoringDiagnostics` fields `HasLegacyDeclarations`, `UsesLegacyDeclarationsOnly`, `LegacyReadCount`, `LegacyWriteCount` removed plus their `passAuthoring` JSON keys.
- Three legacy-only tests deleted: `RenderGraph.DumpToJsonIncludesHybridAuthoringDiagnostics`, `RenderGraph.DeclarationOnlyProducerChainRemainsReachable`, `RenderGraph.GraphNodeDeclarationOnlyProducerChainRemainsReachable`, plus `RenderGraph.CompiledHazardValidationIgnoresLegacyOnlyDeclarationsAfterBuild`, `RenderGraphQueueAwareScheduler.ComputeHoistPrefersCompiledDeclarationsAfterBuild`, `RenderGraphResourceHazards.ParallelWritesToSameResourceAreFlagged`, `RenderGraphResourceHazards.SamePassReadAndWriteIsLegal`, `RenderGraphResourceHazards.ResourceKindMismatchIsFlagged`, `RenderGraphConfigureTopology.MissingDecalToWaterEdgeIsFlagged` / `MissingSceneToFoliageEdgeIsFlagged` / `MissingFoliageToDecalEdgeIsFlagged` / `MissingWaterToParticleEdgeIsFlagged` / `Slice34_DualAOWritersWithoutExplicitEdgeIsWAWHazard`, and the negative branch of `DecalNodeOrdering`. Each test was exercising the legacy pre-Setup declaration surface or the auto-ordering WAW path that `BuildFrameGraph` now subsumes via `RegistrationOrderSensitivity` build diagnostics.
- `LegacyDeclarativeRenderPass` (test-only helper) and `TestGraphNode` (in `RenderGraphTest.cpp`) keep their `DeclareLegacyRead/Write` and `DeclareRead/Write` storage but now flush through `builder.CreateTexture/Framebuffer/Buffer` + `builder.Read/Write` during `Setup`. They're convenience shims for the test stubs; the production engine has zero static-declaration code paths.

## Concrete Code-Backed Gaps

### 1. The graph compiler is real, and the remaining authoring gap is no longer adapter-based

Current evidence:

- `RenderPass` still owns the legacy declaration surface and the pass-centric lifecycle.
- the post/fullscreen chain now uses the same attachment-view model for color sampling too: `PopulateBlackboard()` publishes explicit color views for the graph-owned current-frame post outputs (`SSSColorTexture` through `UICompositeTexture`, plus `PostProcessColorTexture` as the canonical alias), setup declares direct reads of those texture views, and `BuildFrameGraph()` now expands parent-framebuffer access and feedback declarations onto matching attachment-view resources during compilation so parent writers remain visible to attachment-view readers across dependency, barrier, transition, lifetime, and reachability stages;
- the graph now also exposes single-mip, array-layer, and cube-face texture views via `CreateTextureMipView(...)`, `CreateTextureArrayLayerView(...)`, and `CreateTextureCubeFaceView(...)`: `RenderPipeline::PopulateBlackboard()` publishes `HZBDepthMipN` handles for the graph-owned HZB pyramid plus explicit shadow-map layer/face handles over graph-declared shadow roots with explicit frame-local backing, and `GTAORenderPass::Setup()` / `ShadowRenderPass::Setup()` declare those per-mip/per-layer/per-face contracts against the view handles instead of expressing the whole contract only as root-handle subresource ranges;
- `DecalRenderPass::Setup()` now declares the `SceneDepth` shader-sample read used for decal projection, so the compiled frame derives the `SceneDepth` producer -> `DecalPass` edge from setup declarations rather than discovering that dependency only in execute-time texture resolution;
- `OITPrepareRenderPass::Setup()` now declares the `SceneDepthAttachment` transfer-source read and `OITDepthAttachment` transfer-destination write used to seed the graph-owned OIT depth buffer, while OIT particle/decal contributors declare `OITDepthAttachment` as a render-target read for fixed-function depth testing, so the compiled frame can derive `ScenePass -> OITPreparePass -> transparent contributor` ordering from setup declarations instead of execute-time framebuffer binds alone;
- The runtime still thinks in terms of concrete pass classes more than graph-native descriptors, even though execution and setup are already node-native.
- `RGBuilder::AllowFeedback(...)` now exposes explicit same-pass feedback declarations too, and the production scratch/accumulation paths that intentionally read and rewrite the same resource in one node (`BloomMips`, `FogHalfRes`, `SSAORaw`, `WaterRefraction`, `JFA*`, `GTAO` scratch, OIT accum/revealage contributors, plus the `SceneColor` read-modify-write nodes in water and OIT resolve) now annotate that intent directly in setup;
- remaining Phase 5 work is broader versioned-write adoption; shadow-map array layers and point-light cubemap faces now follow the same explicit-view model as attachment and HZB mip consumers, deferred canonical single-sample `SceneDepth` / `SceneNormals` / `GBuffer*` / `Velocity` handles now model their MSAA path as explicit resolve views over the `*MS` sources, and the old paired parent-framebuffer bridge read is no longer part of fullscreen/post setup.

Implication:

- OloEngine has a frame-graph compiler with direct node registration, but not yet a fully graph-native authoring surface.
- The remaining cost is conceptual: the old pass lifecycle still coexists with graph-native setup, even though static legacy declarations are already quarantined to explicit test or dummy nodes instead of `RenderPass`.

Target:

- each node is authored directly as a graph node or graph pass descriptor;
- there is no second older pass contract that must be kept in sync.

### 2. Resource ownership is split between graph resources and pass-owned resources

Target:

- keep OIT, deferred G-buffer attachments, and current-frame post outputs on the explicit-view path while extending the same model to the remaining non-attachment view families.

Current evidence:

- `RenderPipeline::PopulateBlackboard()` still imports a targeted set of resources, but no longer needs to import the main scene MRT and now exposes both the live `SceneColor` attachment set and the deferred G-buffer attachment set through explicit views.
- `AOBuffer` is now graph-owned for both GTAO and SSAO, and the remaining AO technique scratch is graph-visible too (`GTAODenoisePing`, `GTAODenoisePong`, `GTAOEdge`, `HZBDepth`, `SSAORaw`, and `SSAOBlur`). `SceneColor` is now a graph-owned transient scene MRT, its live attachments are explicit graph views (`SceneColorTexture`, `SceneEntityID`, `SceneViewNormals`, `SceneDepthAttachment`), the semantic scene-derived inputs (`SceneDepth`, `SceneNormals`, `Velocity`) remain graph-visible by snapshot/view depending on path, the deferred single-sample and multisample attachment sets are now explicit views of graph-declared `GBufferResolved` / `GBufferMS` roots with explicit frame-local backing, and the fullscreen/post chain publishes graph-owned current-frame outputs plus explicit color-attachment views across `SSS`, `AOApply`, `Bloom`, `DOF`, `MotionBlur`, `TAA`, `Precipitation`, `Fog`, `ChromAb`, `ColorGrading`, `ToneMap`, `Vignette`, `FXAA`, `SelectionOutline`, and `UIComposite`.
- OIT, the deferred G-buffer attachment sets, and the current-frame post outputs have one explicit framebuffer/view representation (`OITBuffer` now publishes `OITAccum`, `OITRevealage`, and `OITDepthAttachment`);
- parent-framebuffer writers feed attachment-view readers through compiled declarations, so direct texture-view setup reads do not require paired bridge declarations;
- TAA and Fog histories are imported into the graph from renderer-owned persistent textures, and those external histories are now updated through graph-managed history sinks registered by `RenderPipeline` rather than pass-local copy-back callbacks.

Implication:

- the graph is now the sole owner of the frame-local scene/post/shadow resource lifetimes and of the attachment/view contracts used by scene/deferred/post/shadow consumers, while some runtime surfaces (notably the prepared deferred G-buffer and live shadow-map textures) are still supplied as explicit frame-local external backing rather than pool-materialized graph allocations;
- history handling is graph-aware and no longer pass-private, but not graph-owned end to end.

UE-level target:

- all frame-local scratch and scene/deferred/post intermediates are graph-created, and the remaining imports are limited to explicit long-lived/external resources plus persistent histories;
- histories and other cross-frame resources are modeled as graph-external resources with explicit ingress and egress, not hidden pass internals.

### 3. Same-frame handle repair is no longer a runtime requirement

Current evidence:

- `Renderer3D::EndScene()` no longer calls `RefreshBlackboardHandles()` as a correctness step.
- `RenderGraphTransientPool.SameFrameImportsAndBuilderTransientsKeepStableHandles` verifies that same-frame imports and builder-declared transients survive `BuildFrameGraph()` without handle repair.

Implication:

- the current frame no longer depends on a post-build repair step;
- the remaining semantic gap is first-class versioned writes and views, not current-frame handle survival.

UE-level target:

- frame handles remain stable once the frame graph is built;
- future work focuses on explicit versioning semantics rather than ad hoc repair.

### 4. Extraction roots already exist, and sink modeling is now explicit

Current evidence:

- `ComputeReachability()` seeds producer reachability from `m_TextureExtracts`, `m_FramebufferExtracts`, and `m_TemporalHistoryContracts`.
- `RenderGraph.ExtractTextureBeforeBuildRootsProducerAndInvokesCallback` and `RenderGraph.ExtractFramebufferBeforeBuildRootsProducerAndInvokesCallback` verify that pre-build extraction keeps producer-only subgraphs alive.
- `RenderGraph.BuilderExtractTextureRootsProducerAndInvokesCallback` and `RenderGraph.BuilderExtractFramebufferRootsProducerAndInvokesCallback` verify the same behavior through `RGBuilder` setup-time extraction.
- `RenderGraph::RegisterExternalTextureSink(...)` now records explicit external sink contracts, and typed texture/framebuffer overloads plus `RGBuilder::RegisterExternalTextureSink(...)` let setup-time code root producer reachability without stringly-typed resource names.
- `RenderGraphExternalTextureSinks.*` tests cover typed sink registration, producer reachability, sink invalidation, and dump metadata (`externalTextureSinkContractCount`, `externalTextureSinkContracts`, `externalTextureSinks=` in the graph digest).
- `FlushExtractions()` still diagnoses extraction of already-culled resources when a caller registers extraction too late (after build/reachability).

Implication:

- extraction declared during setup or before build already preserves the producing subgraph;
- persistent sink/update behavior is now modeled explicitly in the compiled graph, while callback extraction remains as an ad-hoc readback/inspection path.

UE-level target:

- extraction or external-access requests keep the producing subgraph alive and compile into explicit external sink semantics instead of callback-only bookkeeping.

### 5. Runtime fallback logic still duplicates setup intent

Current evidence:

- many fullscreen/post passes already consume the primary input handle chosen during `Setup()` via `GetPrimaryInputFramebufferHandle()`;
- `DeferredLightingPass` now chooses its canonical G-buffer/depth/velocity handle family during `Setup()` and only resolves that preselected family in `Execute()`, keeps the raw `m_GBuffer` accessors as a physical fallback instead of re-deciding the MSAA-vs-resolved path on the fly, and now resolves the scene target from the setup-selected `SceneColor` handle instead of re-reading the blackboard there;
- `SceneRenderPass` now also carries the canonical `SceneColor` framebuffer handle selected during `Setup()` into `Execute()` rather than re-deriving the same scene target from the blackboard at runtime;
- `RenderPass` now exposes setup-owned primary output framebuffer state too, and the single-output fullscreen/post family (`AOApply`, `SSS`, `DOF`, `MotionBlur`, `TAA`, `Precipitation`, `ToneMap`, `FXAA`, `ColorGrading`, `ChromaticAberration`, `Vignette`, `UIComposite`) uses that stored output handle in `Execute()` instead of re-resolving the same blackboard output every frame;
- the remaining special-case scratch framebuffers now follow the same pattern too: `SSAORenderPass` stores its `SSAORaw` scratch framebuffer during setup, `BloomRenderPass` stores its bloom-mip framebuffer chain during setup-owned state, and `FogRenderPass` / `SelectionOutlineRenderPass` store their half-res and JFA ping-pong scratch framebuffers during `Setup()`, so pass execution no longer re-reads framebuffer targets from the blackboard;
- the simple scene-modifier passes now follow that same pattern for their main scene target: `ForwardOverlayRenderPass`, `FoliageRenderPass`, `WaterRenderPass`, `ParticleRenderPass`, and `DecalRenderPass` all store `SceneColor` as a setup-selected framebuffer handle and resolve that stored handle in `Execute()` instead of replaying the same blackboard lookup ladder every frame;
- the OIT prep/contributor path now carries its OIT target selection through setup too: `OITPrepareRenderPass`, `ParticleRenderPass`, and `DecalRenderPass` store the chosen `OITBuffer` handle during `Setup()` and resolve that stored handle in `Execute()` rather than querying the blackboard again for the same target;
- `SelectionOutlineRenderPass` now also carries its secondary `SceneColor` / entity-ID source as a setup-selected handle, and `OITResolveRenderPass` now carries one canonical OIT framebuffer handle chosen during setup instead of re-deciding between blackboard aliases in `Execute()`;
- the same execute-time cleanup now covers canonical secondary texture inputs too: `AOApply`, `DOF`, `MotionBlur`, `TAA`, `SSS`, `SSAO`, `Fog`, `SelectionOutline`, `Water`, `Decal`, `OITResolve`, and `GTAO` store their scene/OIT/history/AO/entity/refraction/shadow/HZB texture handles during `Setup()` and resolve those stored handles in `Execute()` instead of re-reading them from the blackboard;
- public graph output declaration for the remaining optional fullscreen stages is readiness-gated (`IsReadyForExecution()`) instead of following settings alone, and there are now no direct `ResolveFramebuffer(board->...)`, no direct `ResolveTexture(board->...)`, and no execute-time `GetBlackboard()` call sites left under `OloEngine/src/OloEngine/Renderer/Passes`.

Implication:

- canonical pass-local resource selection is now setup-owned end-to-end across the production pass layer, with execute paths no longer depending on blackboard access for those canonical resources;
- the remaining desync risk is narrower and centered on broader authoring/frontend seams rather than per-pass blackboard re-selection.

UE-level target:

- setup decides the actual inputs;
- execute only records commands using resolved handles chosen by setup.

### 6. Registration-order sensitivity is narrower now, but not gone

Current evidence:

- the live `SceneColor` modifier passes are now closer to the graph-first target: `ForwardOverlay`, `Foliage`, classic `Decal`, and classic `Particle` declare explicit render-target read/modify/write feedback in `Setup()`, and the old per-frame upstream-command-availability side channel for foliage/decal/water ordering has been removed from `RenderPipeline::ConfigurePassesForFrame()`;
- setup-declared pass dependencies (`builder.DependsOnPass(...)`) now count as semantic ordering truth in both production code and focused L5 coverage, so production-shaped scene/deferred chains no longer rely on registration-order folklore in tests;
- `RenderGraph::BuildFrameGraph()` now compares simulated semantic ordering relations rather than raw differing direct-edge sets when it emits `RegistrationOrderSensitivity` diagnostics, which filters redundant transitive-edge noise out of production-shaped graphs;
- remaining diagnostics still appear for genuinely ambiguous same-resource writer chains or intentionally under-constrained tests, which means registration order has not been eliminated as a tie-breaker everywhere.

Implication:

- the graph compiler is stronger and less noisy than before, especially when explicit/setup-declared dependencies already pin the order;
- the remaining work is to eliminate truly ambiguous ordering cases, not to keep chasing redundant-edge diagnostics.

UE-level target:

- insertion order may remain a deterministic tie-breaker, but not a correctness dependency.

### 7. Async compute is planned correctly, but not realized on GL

Current evidence:

- GTAO is marked compute and async candidate.
- `RenderGraph` hoists compute candidates, batches them, and emits batch metadata.
- `RGCommandContext::BeginAsyncBatch()` and `EndAsyncBatch()` are only debug-group labels on GL.

Implication:

- the scheduling IR is future-ready, but current overlap is conceptual only.

UE-level target under GL constraints:

- preserve the IR and dependency model;
- replay conservatively on a single queue;
- treat real overlap as explicit-backend follow-up work, not a blocker for the architectural refactor.

## Constraints That Must Shape the Plan

### OpenGL 4.6 constraints

What GL prevents:

- true queue families and real queue ownership transfers;
- explicit image layouts;
- precise heap residency management;
- explicit aliasing barriers in the Vulkan or DX12 sense.

What GL still allows:

- graph-owned transient resources;
- conservative logical transition planning;
- GL memory barrier planning and replay;
- resource lifetime analysis and alias-slot planning;
- imported and extracted resource contracts;
- compute and copy node classification;
- backend-portable scheduler and transition IR.

### Render bucket constraints

What must stay:

- command bucket recording for draw-heavy work;
- packet sorting and replay strategy;
- existing draw submission APIs as the frontend contract.

What must change:

- buckets can no longer imply pass ownership;
- buckets must be owned by graph-native stream nodes or graph-native raster nodes;
- graph setup must declare the resources a bucket node reads and writes;
- graph execution must be the only place where buckets replay.

## Target End State Under These Constraints

The achievable target is:

```text
Renderer frontend
    -> records render intent and packets into frame-owned graph streams
        -> pipeline builder creates a graph-native frame description
            -> graph compiler derives dependencies, culling, lifetimes, transitions, and submission plan
                -> GL backend executor replays the compiled plan conservatively
                -> future Vulkan/DX12 executors map the same plan to explicit queues and transitions
```

Key properties of the target state:

1. No pass uses `SetSetupCallback()` because graph-native setup is the pass contract.
2. No production pass exposes or uses `DeclareRead()` / `DeclareWrite()` as a second declaration path; any remaining static declarations live in explicit test or diagnostic node helpers.
3. No post-build blackboard handle refresh is required for correctness.
4. Extraction and history write-back are part of graph liveness.
5. Render buckets are replayed only by graph nodes.
6. Registration order is a deterministic tie-breaker only, never a correctness dependency.
7. GL remains conservative, but the graph IR is explicit-backend-ready.

## Refactor Plan

## Phase 0: Stabilize Scope and Add Migration Guards

Goals:

- stop adding new hybrid patterns during the refactor;
- make current graph behavior observable and regression-resistant.

Work items:

1. Keep any remaining static declaration helpers explicitly marked test or diagnostic only, and do not reintroduce `DeclareRead()` / `DeclareWrite()` onto production `RenderPass`.
2. Add debug-only counters or dumps that track:
   - passes using setup callbacks;
   - passes using legacy declarations only;
   - passes that still resolve fallback inputs at execute time;
   - imported resources that are actually frame-local intermediates.
3. Extend render-graph JSON dumps to label:
   - graph-created transient resources;
   - imported external resources;
   - extracted resources;
   - history ingress and egress;
   - bucket-backed nodes.

Acceptance criteria:

- the engine can report how much of the frame is still hybrid;
- no new graph work is added without being visible in diagnostics.

## Phase 1: Introduce Stable Per-Frame Resource Handles

Goals:

- remove the need for `RefreshBlackboardHandles()` as a correctness step;
- make handles stable for the life of the compiled frame.

Work items:

1. Split resource identity from physical backing identity.
2. Change `GetTextureHandle()`, `GetFramebufferHandle()`, and `GetBufferHandle()` semantics so the frame's compiled graph owns a stable handle table.
3. Rebuild physical backing independently from logical handle identity.
4. Move any generation-changing registry rebuild behavior behind the compile step, not into runtime resolve behavior.
5. Keep stale-handle diagnostics for truly invalid old-frame or user misuse cases, but stop requiring an in-frame repair step.

Design note:

- It is acceptable to keep generation counters for safety.
- It is not acceptable for a normal same-frame handle captured during authoring to require repair after compile.

Acceptance criteria:

- `Renderer3D::EndScene()` no longer needs `RefreshBlackboardHandles()` for current-frame correctness;
- setup-selected handles remain valid throughout `Execute()`.

## Phase 2: Make Graph Setup the Only Declaration Surface

Goals:

- remove dual declaration paths;
- make setup and execute speak one contract.

Work items:

1. Introduce graph-native pass base types:
   - `RasterGraphNode`
   - `ComputeGraphNode`
   - `CopyGraphNode`
   - `PresentGraphNode`
   - `BucketGraphNode`
2. Move setup logic from pipeline-builder lambdas into node-owned `Setup()` implementations.
3. Remove `RenderPass::SetSetupCallback()` from normal production usage.
4. Keep static declaration support quarantined to explicit test or dummy nodes once all runtime nodes declare access through `RGBuilder` only.
5. Convert fullscreen and compute passes first because they have the least bucket coupling.

Recommended conversion order:

1. Final
2. UIComposite
3. ToneMap
4. ColorGrading
5. ChromaticAberration
6. Fog
7. TAA
8. MotionBlur
9. DOF
10. Bloom
11. AO passes

Acceptance criteria:

- the converted nodes have no setup callbacks;
- setup is owned by the node class;
- execute paths consume handles chosen by setup, not execute-time fallback policy.

## Phase 3: Convert Render Buckets into Graph-Owned Render Streams

Goals:

- preserve the render bucket system while removing pass ownership from it;
- make graph nodes the only executors of bucket work.

Work items:

1. Introduce frame-owned render streams, for example:
   - Geometry
   - ForwardOverlay
   - Foliage
   - Decal
   - Water
   - Debug
2. Route draw submission APIs into stream-owned bucket contexts rather than concrete pass instances.
3. Replace direct pass bucket ownership with bucket-backed graph nodes.
4. Define graph-native raster bucket nodes that:
   - declare render targets, depth inputs, scene textures, and side effects in `Setup()`;
   - replay packet buckets in `Execute()`.
5. Keep `CommandDispatch` as the packet executor.
6. Keep packet format and sorting behavior unchanged initially.

Important design choice:

- Command buckets are not the old system that survives next to the graph.
- Command buckets become one execution strategy inside the graph.

Acceptance criteria:

- draw packets are recorded without naming concrete pass classes;
- graph execution is the only bucket replay path;
- raster work appears in the submission plan as graph-native nodes.

## Phase 4: Make the Graph Own Histories, Scratch, and Frame-Local Intermediates

Goals:

- push the imported-vs-transient boundary outward until only true external resources remain imported;
- remove pass-private scratch and history persistence.

Work items:

1. Formalize resource classes:
   - transient graph resources;
   - external imported resources;
   - external persistent history resources;
   - extracted resources;
   - resource views.
2. Replace pass-owned scratch framebuffers and textures with graph-created resources.
3. Finish replacing legacy history-storage plumbing with first-class graph-managed history resource contracts.
4. Add explicit builder or graph APIs for:
   - import external history;
   - extract history for next frame;
   - convert extracted graph resources into next-frame imported histories.
5. Make extraction and history write-back liveness roots.

Acceptance criteria:

- TAA and Fog history persistence no longer depends on pass-private framebuffer ownership;
- scratch resources such as Bloom mips, JFA ping-pong, HZB, GTAO denoise ping-pong, GTAO edge, SSAO raw, SSAO blur, and fog half-res are graph-owned end to end;
- extraction keeps required producers alive.

## Phase 5: Add First-Class Resource Views and Versioned Write Semantics

Goals:

- remove remaining ambiguity around framebuffer-wide names vs attachment views;
- reduce reliance on canonical mutable names.

Current status:

- framebuffer attachment views now exist as first-class graph texture handles via `CreateFramebufferAttachmentView(...)` / `CreateFramebufferDepthAttachmentView(...)`, and weighted-blended OIT plus the deferred G-buffer now use them to model one parent framebuffer resource with explicit attachment views (`OITBuffer` -> `OITAccum` / `OITRevealage` / `OITDepthAttachment`; `GBufferResolved` / `GBufferMS` -> `SceneDepth`, `SceneDepthMS`, `SceneNormals`, `GBuffer*`, `Velocity*`);
- the post/fullscreen chain now uses the same attachment-view model for color sampling too: `PopulateBlackboard()` publishes explicit color views for the graph-owned current-frame post outputs (`SSSColorTexture` through `UICompositeTexture`, plus `PostProcessColorTexture` as the canonical alias), and the runtime execute paths from `AOApply` through `Final` resolve those setup-selected texture views instead of indexing color attachment 0 from the chosen input framebuffer by hand;
- `RGBuilder::WriteNewVersion(...)` now creates graph-owned version handles for textures, framebuffers, and buffers by cloning the source descriptor into a stable pass-derived resource name, so sequential rewrite chains can opt into explicit producer-owned versions today; production has started using that path too, with `SelectionOutlineRenderPass::Setup()` publishing a versioned `SelectionOutlineColor` output for `UICompositeRenderPass`, `SSSRenderPass::Setup()` publishing a versioned `SSSColor` output that `AOApplyRenderPass` / `BloomRenderPass` prefer when later seams are absent, `AOApplyRenderPass::Setup()` publishing a versioned `AOApplyColor` output that `BloomRenderPass` prefers, `BloomRenderPass::Setup()` publishing a versioned `BloomColor` output that `DOFRenderPass` / `MotionBlurRenderPass` / `TAARenderPass` / `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `DOFRenderPass::Setup()` publishing a versioned `DOFColor` output that `MotionBlurRenderPass` / `TAARenderPass` / `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `MotionBlurRenderPass::Setup()` publishing a versioned `MotionBlurColor` output that `TAARenderPass` / `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `TAARenderPass::Setup()` publishing a versioned `TAAColor` output that `PrecipitationRenderPass` / `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `PrecipitationRenderPass::Setup()` publishing a versioned `PrecipitationColor` output that `FogRenderPass` / `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `FogRenderPass::Setup()` publishing a versioned `FogColor` output that `ChromaticAberrationRenderPass` / `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `ChromaticAberrationRenderPass::Setup()` publishing a versioned `ChromAbColor` output that `ColorGradingRenderPass` / `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `ColorGradingRenderPass::Setup()` publishing a versioned `ColorGradingColor` output that `ToneMapRenderPass` / `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `ToneMapRenderPass::Setup()` publishing a versioned `ToneMapColor` output that `VignetteRenderPass` / `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `VignetteRenderPass::Setup()` publishing a versioned `VignetteColor` output that `FXAARenderPass` / `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when later seams are absent, `FXAARenderPass::Setup()` publishing a versioned `FXAAColor` output that `SelectionOutlineRenderPass` / `UICompositeRenderPass` prefer when available, and `UICompositeRenderPass::Setup()` publishing a versioned `UIComposite` output for `FinalRenderPass`;
- `RenderGraph::CreateTextureMipView(...)`, `CreateTextureArrayLayerView(...)`, `CreateTextureCubeFaceView(...)`, and `CreateTextureMultisampleResolveView(...)` now expose single-mip, array-layer, cube-face, and multisample-resolve texture views as first-class typed handles, and production uses all four: `PopulateBlackboard()` publishes `HZBDepthMipN` handles for the graph-owned HZB pyramid, explicit shadow-map layer/face handles over graph-declared shadow roots with explicit frame-local backing, and deferred canonical single-sample `SceneDepth` / `SceneNormals` / `GBuffer*` / `Velocity` handles as resolve views over their `*MS` sources when MSAA is active;
- `RGBuilder::AllowFeedback(...)` now exposes explicit same-pass feedback declarations too, and the production scratch/accumulation paths that intentionally read and rewrite the same resource in one node (`BloomMips`, `FogHalfRes`, `SSAORaw`, `WaterRefraction`, `JFA*`, `GTAO` scratch, and OIT accum/revealage contributors) now annotate that intent directly in setup;
- the OIT depth path is now setup-visible too: `PopulateBlackboard()` publishes `OITDepthAttachment`, `OITPrepareRenderPass::Setup()` declares the scene-depth seed into it, and transparent OIT contributors declare the fixed-function depth-test read against that view;
- remaining Phase 5 work is broader versioned-write adoption; current fullscreen/post color-input attachment-view adoption is now in place alongside OIT + deferred G-buffer, HZB-style mip views plus shadow-map array-layer/cube-face views are on that same explicit-view path, deferred canonical single-sample exports now model the MSAA bridge as explicit resolve views instead of unrelated sibling names, and canonical base-name handle lookup now tracks the latest explicit version so external/debug name-based resolve stays aligned with those producer-owned outputs.

Work items:

1. Introduce explicit write-version semantics or versioned handles for resources that are logically rewritten in sequence.
2. Model OIT, G-buffer attachments, and post outputs with explicit views instead of mixed framebuffer and attachment naming shortcuts.
3. Add feedback declarations for legal same-resource read-modify-write cases.

Acceptance criteria:

- OIT and the deferred G-buffer attachment sets have one explicit framebuffer/view representation, including the OIT depth attachment view;
- write-after-write ordering no longer depends on mutable-name conventions alone;
- more dependencies can be derived without insertion-order sensitivity.

## Phase 6: Remove Insertion-Order Correctness Dependencies

Goals:

- make graph correctness depend only on declarations and explicit non-resource edges.

Work items:

1. Audit every place where registration order currently matters.
2. Strengthen dependency derivation for:
   - same-resource multi-writer chains;
   - attachment-view writes;
   - subresource overlap;
   - imported-to-transient handoff;
   - extracted resource roots.
3. Add validation that explicitly reports when registration order changed the result.
4. Keep insertion order only as a deterministic topo tie-break when all semantic dependencies are already known.

Acceptance criteria:

- comments that warn about insertion-order correctness hacks can be removed;
- graph rebuilds remain correct even if independent nodes are registered in a different order.

## Phase 7: Split Compiler Responsibilities out of `RenderGraph`

Goals:

- keep the graph API coherent as the system grows;
- make the compiler backend-portable rather than monolithic.

Work items:

1. Split `RenderGraph` internals into dedicated modules:
   - resource registry;
   - transient planner;
   - reachability and culling;
   - hazard validation;
   - barrier or transition planner;
   - scheduler;
   - submission-plan builder;
   - backend executor.
2. Keep `RenderGraph` as the orchestration facade, not the owner of every algorithm.
3. Define a backend executor interface so GL replay is one implementation.
4. Preserve JSON and DOT dump support as compiler products.

Acceptance criteria:

- graph compile stages have clear boundaries;
- backend replay is no longer tightly coupled to the compiler data structures.

## Phase 8: Harden the OpenGL Backend Contract

Goals:

- make the GL path conservative, explicit in intent, and future-backend-friendly.

Work items:

1. Keep `GetResourceTransitions()` and submission-plan metadata authoritative even if GL only consumes part of it.
2. Map resource transitions to conservative GL operations:
   - `glMemoryBarrier()` categories;
   - framebuffer bind or unbind boundaries;
   - texture update and copy ordering;
   - debug labels for pseudo-async batches.
3. Introduce a backend-state tracker for GL so graph transitions can be validated against actual GL usage.
4. Keep async batch metadata in the plan even though GL replays serially.

Acceptance criteria:

- GL replay uses the same compiled submission and transition plan expected by future backends;
- no GL-only shortcuts leak back into authoring semantics.

## Phase 9: Remove the Legacy Parallel Pass Framework

Goals:

- eliminate the hybrid architecture once graph-native parity exists.

Work items:

1. Remove production reliance on legacy `RenderPass` setup callbacks.
2. Remove production reliance on static declaration metadata and keep any remaining legacy declaration helpers test-only.
3. Move remaining pass-private framebuffer creation into graph resource declarations or backend-owned external resource imports.
4. Replace pass-centric pipeline composition with graph-node-centric composition.
5. Keep only thin helper wrappers where needed for editor UX or tooling, not as a second runtime execution system.

Acceptance criteria:

- there is one renderer execution model;
- every frame operation is graph-authored and graph-executed.

## Recommended Implementation Order

Use this order to reduce churn and avoid repeated rewrites:

1. Stable handles and extraction roots.
2. Fullscreen and compute node conversion.
3. History and scratch ownership cleanup.
4. Render bucket stream conversion.
5. Resource views and versioned writes.
6. Insertion-order cleanup.
7. Compiler module split.
8. Legacy pass framework removal.

Rationale:

- Stable handles and extraction roots fix foundational correctness problems first.
- Fullscreen and compute passes are easiest to convert and validate.
- History cleanup removes a major source of hidden ownership.
- Bucket migration should happen after the graph contract is strong enough to own raster work without a second pass framework.

## Acceptance Criteria for the Whole Refactor

The refactor is complete when all of the following are true. **All nine criteria are met as of 2026-05-12** — see the per-criterion status below.

1. **`Renderer3D::EndScene()` does not repair graph handles after compile.** ✓ Done. The post-build `RefreshBlackboardHandles()` step is gone; same-frame handles stay stable across compile.
2. **Production passes no longer use setup callbacks.** ✓ Done. Every production pass implements `Setup(RGBuilder&, FrameBlackboard&)` directly as a `RenderGraphNode` override; the `SetSetupCallback` mechanism is restricted to test/diagnostic nodes.
3. **Production passes no longer rely on legacy declaration metadata for correctness.** ✓ Done. The `GetDeclaredReads()` / `GetDeclaredWrites()` virtuals and `m_PassAccessDeclarations` legacy fallback were removed on 2026-05-11; setup-time `builder.Read` / `builder.Write` is the only declaration path.
4. **Extraction and history write-back are graph roots.** ✓ Done. `RegisterExternalTextureSink(...)` and the temporal-history sink machinery both ground reachability in `ComputeReachability` so producers are kept alive.
5. **Bucket replay occurs only through graph-native nodes.** ✓ Done. `CommandBufferRenderPass` is a `RenderGraphNode` subclass; `RenderPipelineBuilder*` registers nodes only via `graph.AddNode`.
6. **Graph correctness does not rely on registration order hacks.** ✓ Done. Every same-resource writer chain in production is pinned via either the producer-owned-seam pattern (post chain) or `builder.DependsOnPreviousWriter(...)` (SceneColor RMW, OIT contributors). Registration order is a deterministic tie-break only.
7. **All frame-local scratch and post-process intermediates are graph-owned.** ✓ Done. Bloom mips, JFA ping-pong, HZB, GTAO denoise ping-pong + edge, SSAO raw + blur, fog half-res, water refraction, and the post-process linear chain outputs are all graph-declared transients (or graph-owned views over them).
8. **The GL backend replays a compiled submission plan and transition plan, even if it serializes everything.** ✓ Done. `RenderGraphPlanExecutor::ExecutePlan` walks the cached IR and dispatches each command kind (`BatchBegin` / `BatchEnd` / `MemoryBarrier` / `Pass`) to the abstract `RGCommandContext`; the OpenGL bindings live one layer deeper in `OpenGLRendererAPI::MemoryBarrier`.
9. **Adding a future explicit backend does not require changing pass authoring semantics again.** ✓ Done. Phase 7 split (2026-05-11 — 2026-05-12, eight slices) extracted the compiler into eight backend-free modules (`RenderGraphBarrierPlanner`, `RenderGraphReachability`, `RenderGraphHazardValidator`, `RenderGraphResourceRegistry`, `RenderGraphTransientPlanner`, `RenderGraphSubmissionPlan`, `RenderGraphPlanExecutor`, `RenderGraphHandleAllocator`) with value-type input/output contracts. A Vulkan / DX12 backend consumes the same `SubmissionCommand` IR + `ResourceTransition` records without touching any planner.

## What Is Realistically Out of Scope for the GL Milestone

These are explicit-backend follow-ups, not blockers for reaching UE-level architecture quality:

- true async compute overlap;
- queue ownership transfers;
- explicit image layout transitions;
- placed-resource aliasing and residency control;
- backend-specific heap tuning.

The key rule is:

- do not design the authoring or compiler model around GL's limitations;
- only constrain the replay backend around GL's limitations.

## Final Recommendation

OloEngine should not try to become a literal clone of UE RDG.

It should instead do three things:

1. finish the transition from hybrid pass system to graph-native authoring;
2. keep render buckets as graph-owned raster execution streams;
3. keep OpenGL as a conservative backend under a richer graph compiler contract.

That path is achievable.

It gets OloEngine to the same practical level as UE in authoring, dependency derivation, culling, resource lifetime ownership, extraction semantics, and backend-portable planning, while accepting that true multi-queue and explicit-memory parity must wait for a future explicit backend.

## Final Status (2026-05-12)

The refactor described in this document is **functionally complete**. The trajectory was:

- **Phases 1–4** (handle stability, graph-native pass authoring, bucket-as-strategy, history/scratch ownership) landed over the months leading up to the 2026-05 sprint.
- **Phase 5** (versioned writes + resource views) landed with the 15-seam producer-owned-version pattern in the post-process chain plus first-class attachment / mip / array-layer / cube-face / multisample-resolve views.
- **Phase 6** (insertion-order cleanup) landed alongside the pipeline-builder slices in May 2026: the SceneColor RMW + OIT contributor chains migrated to name-based `builder.DependsOnPreviousWriter(...)`, the post-process preferred-source ladder was replaced with name-based lookups, and `RenderPipelinePassInputs` was type-erased to `RenderGraphNode*`.
- **Phase 7** (compiler module split) landed in eight slices over 2026-05-11 — 2026-05-12, extracting **1,485 lines** from the `RenderGraph` god-class into eight focused modules (`Barrier­Planner`, `Reachability`, `Hazard­Validator`, `Resource­Registry`, `Transient­Planner`, `Submission­Plan`, `Plan­Executor`, `Handle­Allocator`). `RenderGraph.cpp` shrank from 7,517 to 6,032 lines and is now an orchestration facade that walks the modules in canonical compile-stage order.
- **Phase 8** (GL backend hardening) was largely landed inline with the other phases — `GetResourceTransitions` is authoritative, async batch metadata persists in the plan, and the GL `MemoryBarrier` translator stays decoupled from the planner.
- **Phase 9** (legacy parallel pass framework removal) landed via the legacy-declaration removal (2026-05-11) and the `RenderPass`→`RenderGraphNode` collapse (also 2026-05-11): the engine has a single graph-node base class, zero legacy declaration code paths, and a name-based pipeline-builder API.

All **nine acceptance criteria** above are met. All P1 rows in the gap table are **Done**. P2 rows are either Done outright or Done-with-deliberately-accepted-limitation per the explicit out-of-scope list (true async overlap, queue ownership transfers, explicit image layout transitions, placed-resource aliasing, backend-specific heap tuning — all gated on a future explicit backend).

What remains beyond this milestone is genuinely outside the GL-target scope: multi-queue parity needs a Vulkan / DX12 backend, and frame-capture debug affordances like per-modifier aliasing views are debug-tooling features not graph-correctness work. The compiler architecture is ready for both whenever the engine reaches them.