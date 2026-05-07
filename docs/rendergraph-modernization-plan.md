# RenderGraph Modernization Plan

## Goal

Move OloEngine to a graph-first renderer inspired by Unreal Engine and Frostbite-style render graph systems while keeping two project constraints:

- The backend remains OpenGL 4.6 for now.
- The Molecule-style command bucket system remains the draw submission and replay mechanism for draw-heavy work.

The final renderer should not expose a separate older pass framework. Every frame operation should be represented as a graph node with an explicit resource contract and one canonical execution path.

## Target Architecture

The desired frame model is:

```text
Renderer frontend
    -> records scene and render intents
        -> render pipeline builds graph nodes
            -> render graph compiles resources, lifetimes, barriers, and ordering
                -> render graph executor runs graph nodes
                    -> command bucket nodes replay draw packets
                    -> fullscreen, compute, copy, UI, and present nodes run backend work
```

Command buckets remain important. The change is that buckets become an execution strategy owned by graph nodes instead of being tied to a parallel pass architecture.

## Design Principles

1. Every render operation is a graph node.
2. Every graph node declares all resources it reads and writes.
3. The graph owns frame scheduling, culling, resource lifetimes, transient allocation, and barrier planning.
4. Explicit ordering edges are reserved for non-resource dependencies and side-effect constraints.
5. OpenGL backend behavior is conservative and correct; future explicit APIs can map the same graph metadata to richer queue and layout semantics.
6. Renderer-facing APIs submit intent and draw packets, but they do not manually schedule frame execution.
7. Resource handoff happens through graph handles and the frame blackboard, not hidden raw renderer IDs.
8. Diagnostic names, comments, and tests describe behavior rather than project history.

## Desired Module Layout

Suggested organization:

```text
OloEngine/src/OloEngine/Renderer/
    RenderGraph/
        RenderGraph.h/.cpp
        RenderGraphBuilder.h/.cpp
        RenderGraphExecutor.h/.cpp
        RenderGraphNode.h
        RenderGraphResource.h/.cpp
        RenderGraphRegistry.h/.cpp
        RenderGraphBlackboard.h
        RenderGraphDiagnostics.h/.cpp
        RenderGraphTransientPool.h/.cpp
        RenderGraphBarrierPlanner.h/.cpp
        RenderGraphScheduler.h/.cpp
        RenderGraphDump.h/.cpp

    GraphNodes/
        GeometryNode.h/.cpp
        ShadowNode.h/.cpp
        DeferredLightingNode.h/.cpp
        ForwardOverlayNode.h/.cpp
        DecalNode.h/.cpp
        WaterNode.h/.cpp
        ParticleNode.h/.cpp
        AONodes.h/.cpp
        PostProcessNodes.h/.cpp
        UINodes.h/.cpp
        PresentNode.h/.cpp

    Commands/
        CommandBucket.h/.cpp
        CommandPacket.h/.cpp
        CommandDispatch.h/.cpp
        FrameDataBuffer.h/.cpp
```

The exact folder split can change during implementation, but the final design should clearly separate graph infrastructure, graph nodes, and command packet infrastructure.

## Graph Node Contract

Introduce one graph-native node interface:

```cpp
class RenderGraphNode
{
public:
    virtual ~RenderGraphNode() = default;

    [[nodiscard]] virtual std::string_view GetName() const = 0;
    virtual void Setup(RenderGraphBuilder& builder, RenderGraphBlackboard& blackboard) = 0;
    virtual void Execute(RenderGraphContext& context) = 0;
    [[nodiscard]] virtual RenderGraphNodeFlags GetFlags() const = 0;
};
```

Recommended flags:

- Graphics
- Compute
- Copy
- Present
- Readback
- NeverCull
- UsesCommandBucket
- ExternalSideEffect
- DebugOnly
- AsyncCandidateMetadata

On OpenGL, async metadata is a scheduler and diagnostics concept. It should not imply true GPU queue overlap.

## Command Bucket Integration

### Current shape

```text
Renderer3D creates packets
    -> packets go into pass-owned buckets
        -> pass executes bucket
```

### Target shape

```text
Renderer frontend creates packets
    -> submission context routes packets to graph-owned render streams
        -> graph executor reaches the owning node
            -> node executes its command bucket
```

Required work:

1. Add graph-node bucket ownership.
2. Replace direct concrete pass bucket access with render stream routing.
3. Preserve packet sorting, packet metadata, frame data buffers, and parallel submission.
4. Keep `CommandDispatch` as the packet executor.
5. Make graph execution the only place where buckets are replayed.

Suggested render streams:

- Geometry
- Decals
- Foliage
- ForwardOverlay
- Water
- Debug

## Renderer3D End State

`Renderer3D` should become a renderer frontend and facade.

It should keep:

- Public draw APIs.
- Scene/frame entry points.
- Camera, light, environment, and settings submission.
- Material and shader selection where frontend policy is needed.
- Collection of render intent for the current frame.

It should lose:

- Manual graph topology wiring.
- Per-node resource declaration lambdas.
- Direct graph node ownership.
- Most per-node execution configuration.
- Direct blackboard population for every resource.
- Direct knowledge of post-chain internals.

Introduce a dedicated frame pipeline builder:

```cpp
class RenderPipeline
{
public:
    void BuildFrame(RenderGraph& graph, const RenderFrameInputs& inputs);
};
```

Path-specific composition should live in the pipeline builder, not inside `Renderer3D`.

## Pipeline Composition

Split graph construction into focused modules:

- Shadow graph builder
- Geometry graph builder
- Deferred graph builder
- Forward graph builder
- AO graph builder
- Transparency graph builder
- Post-process graph builder
- UI graph builder
- Debug graph builder

Example composition:

```text
Forward:
    Shadows -> forward geometry -> transparency -> optional AO -> post -> UI -> present

Forward+:
    Shadows -> light culling -> forward geometry -> transparency -> optional AO -> post -> UI -> present

Deferred:
    Shadows -> G-buffer geometry -> deferred decals -> deferred lighting -> forward overlay -> transparency -> optional AO -> post -> UI -> present
```

Resource declarations should derive most ordering. Explicit execution edges should remain available for side effects, debug capture, presentation, and cases where ordering is not represented by a resource.

## Resource Model Improvements

All frame resources should be expressed as graph handles or views:

- Texture
- Texture view
- Framebuffer
- Framebuffer attachment view
- Buffer
- History texture
- External texture
- External framebuffer
- Transient texture
- Transient framebuffer

Add first-class resource views for:

- G-buffer attachments
- OIT accumulation and revealage targets
- Cubemap faces
- Texture array layers
- Mip chains
- Multisample resolves

OIT should use one consistent representation. Prefer either:

```text
OIT framebuffer
    attachment 0: accumulation
    attachment 1: revealage
```

or:

```text
OIT accumulation texture
OIT revealage texture
OIT framebuffer view
```

Do not mix whole-framebuffer and loose-attachment semantics without explicit view handles.

## Feedback and Scratch Resources

Add explicit feedback declarations for intentional same-resource read/write behavior:

```cpp
builder.ReadWrite(resource, RGUsage::FramebufferFeedback);
builder.AllowFeedback(resource, subresourceRange);
```

Scratch resources should remain graph-owned so the graph can plan lifetime and allocation. Expensive multi-step effects can later be split into smaller nodes if graph-level scheduling or diagnostics justify it.

Good candidates for later finer granularity:

- Bloom downsample and upsample work
- HZB mip generation
- Selection outline jump-flood work
- GTAO denoise work

## Transient Resource Plan

Target behavior:

1. The graph owns transient scratch textures, framebuffers, and buffers.
2. The graph computes lifetimes and compatible alias groups.
3. The graph materializes only resources needed by reachable nodes.
4. External or owner-backed resources are imported rather than allocated as transients.
5. Culled resources are not allocated.

Required fixes:

1. Register external backing before transient planning.
2. Skip pool allocation when a valid external backing exists.
3. Add diagnostics for graph-owned, external, history, and culled resources.
4. Add tests for external-backed non-allocation, alias reuse, history reuse, extraction after culling, and stale handle rejection.

## Graph Execution API

Resolve the current split between setup callbacks and pass-object execution.

Target API:

```cpp
auto& node = graph.AddNode<GeometryNode>("Geometry");
node.Configure(...);

graph.Compile(frameInputs);
graph.Execute();
```

Each node has exactly one setup path and one execution path:

- `Setup()` declares resources.
- `Execute()` performs backend work.
- Command bucket nodes replay buckets.
- Fullscreen nodes bind graph resources and draw fullscreen geometry.
- Compute nodes dispatch compute work and rely on graph-planned barriers.

Remove any API where an execute callback can be registered but not invoked.

## OpenGL Backend Plan

The graph should keep backend-agnostic concepts, while OpenGL execution remains practical and conservative.

OpenGL graph backend responsibilities:

- Framebuffer binding
- Draw buffer setup
- Texture and image visibility barriers
- Shader storage barriers
- Compute-to-graphics synchronization points
- Debug groups
- Timer queries
- Resource labels
- Transient pool object creation

Example usage mapping:

```text
Render target write -> shader sample read:
    framebuffer barrier + texture fetch barrier

Shader storage write -> shader storage read:
    shader storage barrier

Image write -> texture sample read:
    shader image access barrier + texture fetch barrier

Compute write -> graphics read:
    relevant image, storage, texture, and framebuffer barriers
```

The OpenGL backend should expose capability metadata:

```cpp
BackendCapabilities::SupportsAsyncComputeQueues == false
```

Async graph groups remain useful for diagnostics and future backend work, but the OpenGL executor should never promise real queue overlap.

## Specific Fixes

### Unused graph execute callbacks

1. Add graph-native nodes.
2. Replace callback registration with node registration.
3. Make the executor call node execution only.
4. Add tests proving graph-native node execution runs.
5. Reject setup declarations for names that are not present as graph nodes.

### Registered names skipped by topology

1. Make every registered graph node part of topology.
2. Add diagnostics for orphan declarations during the transition.
3. Remove the orphan-declaration path once all production nodes are migrated.

### Validation timing

1. Keep topology validation after graph composition.
2. Add post-compile validation after dynamic declarations.
3. Gate per-frame validation behind diagnostics settings.
4. Include validation status in graph dumps.

### Owner-backed transient allocation

1. Track external backing in the resource registry.
2. Apply external backing before transient allocation planning.
3. Skip pool acquisition for externally backed resources.
4. Add tests and diagnostics.

### OIT resource consistency

1. Choose one OIT resource representation.
2. Update transparency, particles, decals, water, and resolve nodes to use it.
3. Remove duplicate OIT naming.
4. Add resource declaration and ordering tests.

### Water transparency behavior

1. Decide whether water writes transparency targets.
2. If yes, enable and validate that path.
3. If no, declare only the resources water actually writes.
4. Add tests proving declarations match water settings.

### Same-node scratch work

1. Mark scratch resources as internal graph-owned resources.
2. Keep node-internal barriers inside the node where necessary.
3. Split large nodes only where finer graph scheduling provides a measurable benefit.

### Async compute expectations

1. Keep async metadata.
2. Add backend capability reporting.
3. Make OpenGL diagnostics clear that async groups are metadata/debug groups.
4. Keep tests focused on metadata and synchronization planning, not real hardware overlap.

## Cleanup Rules

The final codebase should use graph-first terminology:

- Render graph
- Graph node
- Graph builder
- Graph resource
- Graph context
- Graph executor
- Graph scheduler
- Render pipeline
- Render stream
- Command bucket
- Submission context
- Transient resource
- Resource lifetime
- Resource transition
- Hazard diagnostic
- Present node

Behavior-based names should replace project-history names.

Examples:

```text
Bad:
    TemporaryWaterTransparencyFallback
    OldPassBridge
    PreviousFramebufferPath

Good:
    WaterWritesTransparencyTargets
    GeometryNodeUsesCommandBucket
    SceneColorImportedFromMainFramebuffer
```

Add a text hygiene check for render graph source, tests, and docs. The check should catch historical milestone wording without blocking unrelated engine systems that legitimately use similar domain language.

## Testing Strategy

### Unit and structural tests

Add or keep tests for:

- Graph node setup and execution.
- Graph-native culling.
- Side-effect node retention.
- Command bucket node execution.
- Resource declaration validity.
- External-backed transient non-allocation.
- OIT resource consistency.
- Water transparency declaration correctness.
- Path switching.
- Resource lifetime correctness.
- Stale handle rejection.
- Graph dump schema.

### Production-shaped tests

Cover:

- Forward graph.
- Forward+ graph.
- Deferred graph.
- Deferred with AO.
- Deferred with transparency.
- Post-chain variants.
- UI and present chain.
- Runtime path switching.
- Runtime AO switching.
- Runtime post setting toggles.

### Graph dump checks

Use stable graph dumps for key configurations. Compare behavior-level fields:

- node list,
- resource list,
- derived edges,
- culled nodes,
- external resources,
- transient resources,
- barrier categories,
- final output chain.

Do not compare timing values.

### Text hygiene check

Add a script that scans render graph source, tests, and docs for banned historical wording. It should:

- ignore vendor and generated output,
- allow unrelated systems with real domain terminology,
- fail on render graph comments, test names, diagnostics, and docs that encode project history instead of behavior.

## Implementation Stages

### Stage 1: Graph-native foundation

Deliverables:

- `RenderGraphNode` interface.
- Node registration path.
- Node setup and execution tests.
- Diagnostics for orphan setup declarations.
- Text hygiene check added but scoped to render graph areas.

Exit criteria:

- A graph-native node can setup and execute without callback registration.
- The executor has one canonical graph-node path for new work.
- Historical milestone wording is prevented in render graph code.

### Stage 2: Command bucket nodes

Deliverables:

- Geometry node.
- Decal node.
- Foliage node.
- Forward overlay node.
- Water node.
- Render stream submission API.

Exit criteria:

- Draw submission routes through render streams.
- Buckets still sort and execute packets.
- Parallel submission still works.

Current implementation status:

- `RenderGraphNode` lifecycle hooks now cover framebuffer setup, resize, render-viewport propagation, and submission-model reporting.
- Production `ScenePass`, `DecalPass`, `FoliagePass`, `ForwardOverlayPass`, and `WaterPass` setup/execution now route through graph-native `PassGraphNode` adapters, so render-stream buckets hang off the same node type as the rest of the graph.
- `Renderer3D` draw-call creation and packet submission now route through a render-stream API instead of directly reaching into concrete pass buckets.
- Geometry parallel submission still reuses the existing worker/bucket flow, but it now resolves the bucket through the geometry render stream instead of hard-coding `ScenePass` ownership.
- Remaining legacy work is now concentrated in deferred-lighting/deferred-decals, OIT/SSS/particles, and shadows.

### Stage 3: Fullscreen and post nodes

Deliverables:

- AO composite node.
- Bloom node.
- Tone map node.
- FXAA node.
- TAA node.
- Fog node.
- UI composite node.
- Present node.

Exit criteria:

- Post chain is graph-owned.
- UI and present nodes choose inputs through graph resources.
- Hidden raw output fallbacks are removed.

Current implementation status:

- `PassGraphNode` is now the single render-pass adapter for the graph; it covers both generic render passes and command-bucket-backed render streams.
- `SSAOPass`, `GTAOPass`, `AOApplyPass`, `BloomPass`, `DOFPass`, `MotionBlurPass`, `TAAPass`, `PrecipitationPass`, `FogPass`, `ChromAberrationPass`, `ColorGradingPass`, `ToneMapPass`, `VignettePass`, `FXAAPass`, `SelectionOutlinePass`, `UICompositePass`, and `FinalPass` now execute through graph-native nodes instead of the legacy `AddPass + RegisterGraphPass` split.
- Late-added graph nodes now synchronize current framebuffer dimensions and dynamic-resolution viewport state when they are registered after initialization, which keeps path switches aligned with the active render scale.
- The remaining bridge-based work after this stage is deferred lighting/deferred decals, particles, OIT resolve, SSS, and shadow scheduling.

### Stage 4: Deferred and transparency cleanup

Deliverables:

- Deferred lighting node.
- Deferred decal node.
- OIT model cleanup.
- Particle transparency node.
- Water transparency declarations fixed.

Exit criteria:

- Deferred path has no hidden resource dependencies.
- OIT resources are represented consistently.
- Water declarations match execution.

Current implementation status:

- `DeferredOpaqueDecalPass`, `DeferredLightingPass`, `ParticlePass`, `OITResolvePass`, and `SSSPass` now execute through graph-native `PassGraphNode` registrations instead of the legacy `AddPass + RegisterGraphPass` bridge split.
- Deferred-path lighting/decal ordering and the transparency tail (`Particle -> OITResolve -> SSS -> AOApply`) are now driven by node-owned setup declarations plus pass self-resolution through `RGCommandContext`.
- `Renderer3D` no longer uses `RegisterGraphPass()` for production frame topology; that cleanup completed when the shadow path moved to graph-native layered declarations.

### Stage 5: Shadows and compute

Deliverables:

- Shadow node.
- GTAO and HZB compute nodes clarified.
- Forward+ light culling node.
- Backend capability metadata for async compute.

Exit criteria:

- Shadow resources are graph-owned or imported consistently.
- Compute barriers are graph-planned and backend-applied.
- OpenGL capability reporting is honest.

Current implementation status:

- `ShadowPass` now executes through a graph-native `PassGraphNode` registration, with layered setup declarations for CSM and spot arrays plus per-light point-shadow writes.
- `SSAOPass` and `GTAOPass` are already graph-native nodes, and `GTAOPass` carries compute/async-candidate metadata through `RenderGraphNodeFlags` rather than the old bridge path.
- `Renderer3D.cpp` no longer contains `RegisterGraphPass()` calls, and path rebuilds rely on `ResetTopology()` alone.
- `RenderGraphTest.cpp` now uses graph-native test-node helpers across barrier planning, derived-edge ordering, transient planning, temporal history, async batch metadata, transition/lifetime reporting, subresource-range propagation, cross-lane sync, queue-aware scheduling, SceneColor RMW coverage, and reset-time declaration invalidation.
- `RenderGraph::RegisterGraphPass()`, `RenderGraph::ClearGraphPasses()`, and the unused `SetGraphFinalPass()` alias have been removed; `BuildFrameGraph()` now compiles per-frame declarations exclusively from graph-native nodes.
- `RGPassSetup.h` was removed because its callback typedefs and setup structs only served the deleted compatibility bridge.
- Remaining modernization work is now about higher-level ownership cleanup, forward+ graph nodes, and shrinking adapter/frontend ownership rather than keeping any legacy bridge execution alive.

### Stage 6: Remove old architecture

Deliverables:

- Node names and files replace older pass naming.
- Bridge adapters removed.
- Old registration APIs removed.
- Stale comments and diagnostics removed.
- Tests and docs updated.

Exit criteria:

- Every render operation is represented as a graph node.
- Graph execution is the only frame scheduler.
- Command buckets remain as graph-node execution payloads.
- The renderer frontend no longer owns concrete frame nodes directly.
- Build, tests, catalogue checks, and text hygiene checks pass.

Current implementation status:

- The legacy compatibility registration seam is gone: `RenderGraph::RegisterGraphPass()`, `RenderGraph::ClearGraphPasses()`, the unused `SetGraphFinalPass()` alias, and `RGPassSetup.h` have all been removed.
- `RenderGraph::BuildFrameGraph()` now compiles per-frame setup declarations exclusively from graph-native nodes; pass-only entries still participate through their static `RenderPass` declarations where applicable.
- `PassGraphNode`-backed passes now surface their wrapped `RenderPass::DeclareRead()` / `DeclareWrite()` metadata to the resource registry, typed-handle lookup, hazard validator, async/work-type metadata queries, compute-hoist ordering, reachability analysis, and resource-lifetime reporting, so wrapped passes behave like first-class graph entries throughout scheduling and diagnostics.
- The raw pass registry is gone: `RenderGraph::AddPass()`, `RenderGraph::GetPass()`, `RenderGraph::GetAllPasses()`, and the internal `m_PassLookup` store have been removed. `RenderGraphDebugger` now discovers wrapped render passes through `PassGraphNode` entries instead of a parallel pass-only API.
- The compiled submission IR is node-only now as well: `SubmissionCommand` no longer carries a raw `RenderPass*`, and `RenderGraph::Execute()` dispatches exclusively through registered graph nodes.
- `RenderGraphTest.cpp` now routes generic wrapped-pass coverage through `PassGraphNode` helpers, and the old compatibility-only `AddPass()` test surface has been retired rather than preserved as a second execution model.
- The duplicate adapter split is gone too: `BucketGraphNode` has been retired, `Renderer3D` render streams now store `PassGraphNode` directly, and command buckets remain reachable through the unified node surface instead of a separate wrapper type.
- `Renderer3D::ConfigureRenderGraph()` now delegates node registration and setup-callback composition to `Renderer3DFrameGraphBuilder`, shrinking the renderer facade's topology ownership and creating a dedicated landing zone for future pipeline-module extraction.
- `Renderer3DFrameGraphBuilder` now owns its concrete pass/settings dependencies directly instead of pulling them in transitively through `Renderer3D.h`, and its post-chain framebuffer preference ordering is expressed through shared helper-driven selection rather than repeated open-coded fallback ladders.
- `Renderer3D` now assembles `Renderer3DFrameGraphInputs` through a dedicated private helper, further reducing `ConfigureRenderGraph()` to path selection, graph delegation, diagnostics, and validation.
- `ConfigureRenderGraph()` now hands its post-build summary logging, hazard validation, and active-path bookkeeping to `FinalizeConfiguredRenderGraph()`, continuing the shift from an all-in-one topology function toward a thinner renderer facade.
- `Renderer3DFrameGraphBuilder.cpp` is no longer the single implementation dumping ground: shared helpers now live in `Renderer3DFrameGraphBuilderInternal.h`, while scene/deferred, transparency/AO, and post/UI/final registration logic each live in their own focused translation unit.
- `SetupRenderGraph()` now delegates primary pass creation, render-stream node creation, OIT hookup, and post-chain pass construction to dedicated renderer helpers instead of carrying the whole bootstrap sequence inline.
- The render-graph bootstrap/configuration cluster (`SetupRenderGraph()`, frame-graph input assembly, path rebuild orchestration, and validation bookkeeping) now lives in a dedicated `Renderer3DRenderGraphSetup.cpp` translation unit instead of staying buried inside the main `Renderer3D.cpp` implementation file.
- `SetupFrameBlackboard()` and the per-frame graph resource import/declaration path now live in `Renderer3DFrameBlackboardSetup.cpp`, shrinking `Renderer3D.cpp` further while preserving the existing renderer-owned blackboard population contract for now.
- `EndScene()` now delegates per-frame pass configuration, GPU execution-state upload, and post-build blackboard handle refresh to dedicated helpers in `Renderer3DFrameExecution.cpp`, so the main renderer implementation keeps less frame-graph orchestration inline.
- Render-stream routing and the parallel-submission entry points (`BeginParallelSubmission()`, `EndParallelSubmission()`, worker-context acquisition, and packet submission) now live in `Renderer3DRenderStreamSubmission.cpp`, keeping graph-owned stream submission logic out of the main renderer monolith.
- `BeginSceneCommon()`, the three public `BeginScene(...)` entry points, and the frame-setup TAA jitter/bootstrap path now live in `Renderer3DFrameSetup.cpp`, pairing begin-of-frame orchestration with the earlier `EndScene()` extraction.
- Renderer state/control plumbing — light/camera setters, global IBL/light-probe uploads, scene-light enumeration, culling/depth/occlusion toggles, frustum helpers, stats access, and `ApplyRendererSettings()` — now lives in `Renderer3DState.cpp`, further isolating frontend state management from draw submission code.
- Specialized draw builders for decals, foliage, and water now live in `Renderer3DSpecializedDraws.cpp`, backed by reusable shared sort/render-state helpers in `Renderer3DDrawHelpers.h` so future draw-path extractions no longer depend on `Renderer3D.cpp` file-local utilities.
- Mesh submission now lives in `Renderer3DMeshSubmission.cpp`, including deferred-capability testing, renderer-ID validation, POD material packing, the full serial mesh / instanced mesh / animated-mesh submission path, animated-scene traversal helpers, and the parallel `DrawMeshParallel` / `DrawAnimatedMeshParallel` / `SubmitMeshesParallel` path, leaving `Renderer3D.cpp` focused more tightly on non-mesh draw helpers and renderer orchestration.
- Non-mesh utility and gizmo submission now lives in `Renderer3DUtilityDraws.cpp`, covering quad / skybox / light-cube / infinite-grid / terrain / voxel packets plus line, sphere, camera, light, audio, collider, world-axis, and skeleton visualization helpers.
- Camera/light UBO uploads, dynamic-resolution propagation, scene/global resource binding, shader-registry helpers, and shader-library access now live in `Renderer3DResourceBindings.cpp`, leaving `Renderer3D.cpp` centered on initialization, asset reload, resize handling, and renderer orchestration.
- Startup, shutdown, asset-reload, resize lifecycle, and the shared static renderer storage now live in `Renderer3DLifecycle.cpp`; the old `Renderer3D.cpp` monolith has been retired entirely while façade behavior continues moving into focused translation units.
- Graph-owned render-stream node ownership is now bundled under `Renderer3DData::StreamNodes`, centralizing stream lookup plus per-frame allocator/reset iteration across setup, execution, submission, and shutdown instead of keeping five separate node fields scattered through the renderer façade state.
- The core scene-binding UBO trio now lives under `Renderer3DData::SharedSceneUBOs` (`Camera`, `Material`, `LightProperties`), tightening the ownership boundary between `Renderer3DResourceBindings.cpp` and `Renderer3DLifecycle.cpp` and removing three more top-level renderer-state fields.
- Post-process GPU-facing UBO/data ownership now lives under `Renderer3DData::PostProcessGPU` (`PostProcess`, `MotionBlur`, `SSAO`, `GTAO` plus their upload structs), consolidating the setup-time bindings and per-frame upload state shared across `Renderer3DRenderGraphSetup.cpp`, `Renderer3DFrameExecution.cpp`, and `Renderer3DLifecycle.cpp`.
- Snow / SSS / fog / fog-volume / dynamic-resolution GPU ownership now lives under `Renderer3DData::SceneEffectsGPU`, consolidating another low-blast-radius block of setup-time bindings plus per-frame upload state across `Renderer3DRenderGraphSetup.cpp`, `Renderer3DFrameSetup.cpp`, `Renderer3DFrameExecution.cpp`, `Renderer3DResourceBindings.cpp`, and `Renderer3DLifecycle.cpp`.
- The deferred/AO/transparency composition block now lives under `Renderer3DData::SceneCompositePasses`, bundling deferred lighting, deferred opaque decals, SSAO, GTAO, particles, and OIT resolve so those mid-frame pass owners no longer sprawl across the renderer façade state.
- The screen-space / post / present tail now lives under `Renderer3DData::PostProcessPasses`, bundling `SSS`, `AOApply`, `Bloom`, `DOF`, `MotionBlur`, `TAA`, `Precipitation`, `Fog`, the late post effects, `UIComposite`, and `Final` so the renderer façade no longer carries that pass-owner block as a dozen-plus separate top-level fields.
- The remaining modernization work is no longer about preserving bridge behavior. It is about ownership cleanup, reducing adapter/frontend surface area, and continuing to collapse concrete pass scheduling into graph-native nodes.

## Acceptance Criteria

The modernization is complete when:

- Every frame operation is a graph node.
- Every graph node declares its resource contract.
- The graph executor is the only scheduler for frame rendering.
- Command buckets are retained but owned by graph nodes.
- Renderer frontend code no longer directly schedules concrete frame nodes.
- Resource handoff uses graph handles and frame blackboard entries.
- Transients are allocated only when needed.
- OIT uses one consistent resource model.
- OpenGL barriers are generated from graph usage.
- Async metadata is clearly backend-limited on OpenGL.
- Graph dumps accurately describe compiled frames.
- Path switching does not leak nodes, resources, or edges.
- Tests cover forward, forward+, deferred, AO, transparency, post, UI, present, shadows, and compute.
- Render graph source, tests, and docs avoid historical milestone wording.

## Recommended Execution Order

1. Land the graph-native node interface and tests.
2. Move command bucket ownership under graph nodes.
3. Move resource declarations into nodes.
4. Shrink renderer frontend responsibilities.
5. Clean up resource modeling, especially OIT and owner-backed transients.
6. Convert remaining fullscreen, compute, UI, present, and shadow work to nodes.
7. Remove obsolete architecture APIs and terminology.
8. Keep the text hygiene check in CI/pre-commit so the codebase stays clean.
