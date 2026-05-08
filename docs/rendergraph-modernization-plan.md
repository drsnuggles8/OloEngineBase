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

    [[nodiscard]] virtual const std::string& GetName() const = 0;
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
- Production `ScenePass`, `DecalPass`, `FoliagePass`, `ForwardOverlayPass`, and `WaterPass` are now registered directly through the `RenderPass` / `CommandBufferRenderPass` graph-node hierarchy, so render-stream buckets live on the concrete graph entries instead of a wrapper adapter.
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

- `SSAOPass`, `GTAOPass`, `AOApplyPass`, `BloomPass`, `DOFPass`, `MotionBlurPass`, `TAAPass`, `PrecipitationPass`, `FogPass`, `ChromAberrationPass`, `ColorGradingPass`, `ToneMapPass`, `VignettePass`, `FXAAPass`, `SelectionOutlinePass`, `UICompositePass`, and `FinalPass` now register directly as `RenderPass` graph nodes instead of using any wrapper or legacy `AddPass + RegisterGraphPass` split.
- Late-added graph nodes now synchronize current framebuffer dimensions and dynamic-resolution viewport state when they are registered after initialization, which keeps path switches aligned with the active render scale.
- The remaining work after this stage was deferred lighting/deferred decals, particles, OIT resolve, SSS, and shadow scheduling; those areas have since moved onto the same direct node path as well.

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

- `DeferredOpaqueDecalPass`, `DeferredLightingPass`, `ParticlePass`, `OITResolvePass`, and `SSSPass` now register directly as `RenderPass` graph nodes instead of any `AddPass + RegisterGraphPass` bridge split.
- Deferred-path lighting/decal ordering and the transparency tail (`Particle -> OITResolve -> SSS -> AOApply`) are now driven by node-owned setup declarations plus pass self-resolution through `RGCommandContext`.
- Weighted-blended OIT now uses one graph-owned transient framebuffer contract (`OITBuffer`) with accumulation, revealage, and depth attachments. Transparent contributors and `OITResolvePass` resolve it through the frame blackboard, while `OITPreparePass` performs the per-frame clear/depth-seed work explicitly inside the graph.
- The old owner-backed OIT wrapper and transient-framebuffer override seam have been retired: `OITBuffer.{h,cpp}` is gone, `DeclareTransientFramebuffer(...)` no longer accepts owner-backed patching, and `OverrideTransientFramebuffer(...)` has been removed from `RenderGraph`.
- The dormant water OIT compatibility branch has now been removed outright: `WaterRenderPass`, `DrawWaterCommand`, command dispatch, and shader warmup no longer carry `Water_OIT` override plumbing, and the water node declares only the resources it actually uses (`SceneColor` + `WaterRefraction`).
- `TAARenderPass` and `FogRenderPass` no longer keep pass-owned current-frame output fallbacks, and fog no longer keeps a pass-owned half-resolution scratch framebuffer. `TAAColor`, `FogColor`, and `FogHalfRes` are now required graph-owned execution surfaces; only the persistent temporal histories remain pass-owned/imported across frames.
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

- `ShadowPass` now executes as a direct `RenderPass` graph node, with layered setup declarations for CSM and spot arrays plus per-light point-shadow writes.
- `SSAOPass` and `GTAOPass` are direct graph nodes, and `GTAOPass` carries compute/async-candidate metadata through the shared node surface rather than any bridge path.
- `Renderer3D.cpp` no longer contains `RegisterGraphPass()` calls, and path rebuilds rely on `ResetTopology()` alone.
- `RenderGraphTest.cpp` now uses graph-native test-node helpers across barrier planning, derived-edge ordering, transient planning, temporal history, async batch metadata, transition/lifetime reporting, subresource-range propagation, cross-lane sync, queue-aware scheduling, SceneColor RMW coverage, and reset-time declaration invalidation.
- `RenderGraph::RegisterGraphPass()`, `RenderGraph::ClearGraphPasses()`, and the unused `SetGraphFinalPass()` alias have been removed; `BuildFrameGraph()` now compiles per-frame declarations exclusively from graph-native nodes.
- `RGPassSetup.h` was removed because its callback typedefs and setup structs only served the deleted compatibility bridge.
- Remaining modernization work is now about higher-level ownership cleanup, forward+ graph nodes, and eventually shrinking the remaining pass-style class surface, rather than keeping any legacy bridge execution alive.

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
- `RenderGraph::BuildFrameGraph()` now compiles per-frame setup declarations exclusively from graph-native nodes, and the graph core no longer needs a separate pass-only metadata path alongside that frame-build contract.
- `RenderGraphNode` now owns the graph's static metadata surface too (`GetDeclaredReads()` / `GetDeclaredWrites()`, work type, async-candidate state, and side-effect state), so `RenderGraph` consumes node-native scheduling/resource metadata directly instead of RTTI-peeking through `PassGraphNode` to interrogate wrapped `RenderPass` objects.
- `RenderPass` and `CommandBufferRenderPass` now inherit `RenderGraphNode` directly, own setup callbacks themselves, and surface declaration/work-type/async/side-effect metadata without an adapter layer. `PassGraphNode.h` has been deleted.
- `RenderGraph` now uses the node-native scheduler enums (`RenderGraphSubmissionModel`, `RenderGraphPassWorkType`) in its public submission/inspection structs and compiled submission IR instead of routing that metadata through `RenderPass` aliases.
- The public scheduler/inspection surface now speaks graph-native execution terminology too: `GetNodeSubmissionInfo()`, `GetExecutionOrder()`, `GetLastExecutionTimings()`, `NodeSubmissionInfo`, `ExecutionTiming`, `SubmissionCommand::NodeName`, and the async-batch node fields (`ComputeNodes`, `WaitNodes`, `SignalNodes`, `ExternalNode`) replace the older pass-typed API names. Debug/editor/test consumers now follow that C++ surface, while the JSON dump schema intentionally remains pass-keyed for this slice.
- The raw pass registry is gone: `RenderGraph::AddPass()`, `RenderGraph::GetPass()`, `RenderGraph::GetAllPasses()`, and the internal `m_PassLookup` store have been removed. `RenderGraphDebugger` now discovers render passes through direct `RenderPass` graph entries instead of a parallel pass-only API.
- The compiled submission IR is node-only now as well: `SubmissionCommand` no longer carries a raw `RenderPass*`, and `RenderGraph::Execute()` dispatches exclusively through registered graph nodes.
- `RenderGraphTest.cpp` now routes generic render-pass coverage through direct `RenderPass` registration helpers, and the old compatibility-only `AddPass()` test surface has been retired rather than preserved as a second execution model.
- The duplicate adapter split is gone too: `BucketGraphNode` has been retired, `Renderer3D` render streams now store `CommandBufferRenderPass` directly, and command buckets remain reachable on the concrete graph nodes instead of any wrapper type.
- `Renderer3D::ConfigureRenderGraph()` now delegates node registration and setup-callback composition through `BuildRenderPipelineGraph(...)`, shrinking the renderer facade's topology ownership and creating a dedicated landing zone for future pipeline-module extraction.
- The dedicated render-pipeline builder surface now owns its concrete pass/settings dependencies directly instead of pulling them in transitively through `Renderer3D.h`, and its post-chain framebuffer preference ordering is expressed through shared helper-driven selection rather than repeated open-coded fallback ladders.
- `Renderer3D.h` no longer has to forward-declare the pipeline build-input bag, and `ConfigureRenderGraph()` no longer assembles `RenderPipelineInputs` locally either: the private `RenderPipeline` now exports `BuildInputs(...)`, keeping builder-facing raw-pointer wiring out of the renderer façade.
- `ConfigureRenderGraph()` now hands its post-build summary logging, hazard validation, and active-path bookkeeping to `FinalizeConfiguredRenderGraph()`, continuing the shift from an all-in-one topology function toward a thinner renderer facade.
- `RenderPipelineBuilder.cpp` is no longer the single implementation dumping ground: shared helpers now live in `RenderPipelineBuilderInternal.h` under `RenderPipelineBuilderInternal`, while scene/deferred, transparency/AO, and post/UI/final registration logic each live in their own focused translation unit.
- `SetupRenderGraph()` now delegates concrete pass creation, render-stream node creation, OIT shader hookup, and post-chain pass construction directly to `RenderPipeline::Setup(...)`, moving frame-pipeline bootstrap ownership into `RenderPipeline.cpp` instead of leaving it as renderer-local helpers.
- The render-graph bootstrap/configuration cluster (`SetupRenderGraph()`, path rebuild orchestration, diagnostics, and validation bookkeeping) now lives in a dedicated `Renderer3DRenderGraphSetup.cpp` translation unit instead of staying buried inside the main `Renderer3D.cpp` implementation file.
- With that extraction in place, `Renderer3DRenderGraphSetup.cpp` is down to graph initialization specs, path rebuild orchestration, diagnostics, and validation bookkeeping; concrete pass/node construction now lives behind the private `RenderPipeline` boundary.
- The old `SetupFrameBlackboard()` façade helper and its standalone `Renderer3DFrameBlackboardSetup.cpp` translation unit are gone: per-frame graph resource import/declaration now lives directly behind `RenderPipeline::PopulateBlackboard(...)`.
- `EndScene()` now hands per-frame pass configuration, blackboard population, GPU execution-state upload, and post-build blackboard-handle refresh straight to `RenderPipeline` (`ConfigurePassesForFrame(...)`, `PopulateBlackboard(...)`, `UploadExecutionState(...)`, `RefreshBlackboardHandles(...)`), leaving `Renderer3DFrameExecution.cpp` focused on frame orchestration plus graph execution.
- Render-stream routing and the parallel-submission entry points (`BeginParallelSubmission()`, `EndParallelSubmission()`, worker-context acquisition, and packet submission) now live in `Renderer3DRenderStreamSubmission.cpp`, keeping graph-owned stream submission logic out of the main renderer monolith.
- The three public `BeginScene(...)` entry points now stop at camera-state capture in `Renderer3DFrameSetup.cpp`, then hand shared begin-of-frame bootstrap (allocator wiring, TAA jitter, shared-scene UBO upload, CommandDispatch setup, and parallel-context prep) straight to `RenderPipeline::PrepareFrame(...)`.
- Renderer state/control plumbing — light/camera setters, global IBL/light-probe uploads, scene-light enumeration, culling/depth/occlusion toggles, frustum helpers, stats access, and `ApplyRendererSettings()` — now lives in `Renderer3DState.cpp`, further isolating frontend state management from draw submission code.
- Specialized draw builders for decals, foliage, and water now live in `Renderer3DSpecializedDraws.cpp`, backed by reusable shared sort/render-state helpers in `Renderer3DDrawHelpers.h` so future draw-path extractions no longer depend on `Renderer3D.cpp` file-local utilities.
- Mesh submission now lives in `Renderer3DMeshSubmission.cpp`, including deferred-capability testing, renderer-ID validation, POD material packing, the full serial mesh / instanced mesh / animated-mesh submission path, animated-scene traversal helpers, and the parallel `DrawMeshParallel` / `DrawAnimatedMeshParallel` / `SubmitMeshesParallel` path, leaving `Renderer3D.cpp` focused more tightly on non-mesh draw helpers and renderer orchestration.
- Non-mesh utility and gizmo submission now lives in `Renderer3DUtilityDraws.cpp`, covering quad / skybox / light-cube / infinite-grid / terrain / voxel packets plus line, sphere, camera, light, audio, collider, world-axis, and skeleton visualization helpers.
- Camera/light UBO uploads, dynamic-resolution propagation, and shader-library access now live in `Renderer3DResourceBindings.cpp`, while per-frame global resource application now runs through `RenderPipeline.cpp`, leaving `Renderer3D.cpp` centered on initialization, asset reload, resize handling, and renderer orchestration.
- Shared scene-resource rebinding is no longer a public `Renderer3D` façade wrapper: `SceneRenderPass`, `ForwardOverlayRenderPass`, and `DeferredLightingPass` now rebind camera/light/Forward+ baseline state through `CommandDispatch::BindSceneResources()`, deleting `Renderer3D::BindSceneUBOs()` and moving that execution-time responsibility into the packet/runtime layer that already owns the relevant UBO state.
- Deferred blackboard population no longer falls back to placeholder `ScenePass` attachments either: `ConfigurePassesForFrame()` now prepares `SceneRenderPass` deferred resources up front, and `PopulateBlackboard()` imports `SceneDepth`, `SceneNormals`, the G-buffer attachments, and deferred velocity from a required prepared `GBuffer` instead of quietly tolerating a missing one.
- Shader-resource registry ownership no longer hangs off `Renderer3D` state: `ShaderResourceRegistry` now owns the program-ID lookup table used by `OpenGLShader`, `CommandDispatch`, and `RenderPipeline` global-resource fan-out, so the renderer façade no longer carries a parallel shader-registry map or forwarding API.
- Startup, shutdown, asset-reload, resize lifecycle, and the shared static renderer storage now live in `Renderer3DLifecycle.cpp`; the old `Renderer3D.cpp` monolith has been retired entirely while façade behavior continues moving into focused translation units.
- Graph-owned render-stream node ownership is now bundled under the private `RenderPipeline::StreamNodes`, centralizing stream lookup plus per-frame allocator/reset iteration across setup, execution, submission, and shutdown instead of keeping five separate node fields scattered through the renderer façade state.
- The core scene-binding UBO trio now lives under `Renderer3DData::SharedSceneUBOs` (`Camera`, `Material`, `LightProperties`), tightening the ownership boundary between `Renderer3DResourceBindings.cpp` and `Renderer3DLifecycle.cpp` and removing three more top-level renderer-state fields.
- Post-process GPU-facing UBO/data ownership now lives under `Renderer3DData::PostProcessGPU` (`PostProcess`, `MotionBlur`, `SSAO`, `GTAO` plus their upload structs), consolidating the setup-time bindings and per-frame upload state now driven primarily through `RenderPipeline.cpp` plus the surrounding lifecycle/resource-binding code.
- Snow / SSS / fog / fog-volume / dynamic-resolution GPU ownership now lives under `Renderer3DData::SceneEffectsGPU`, consolidating another low-blast-radius block of setup-time bindings plus per-frame upload state now driven primarily through `RenderPipeline.cpp` together with `Renderer3DFrameSetup.cpp`, `Renderer3DResourceBindings.cpp`, and `Renderer3DLifecycle.cpp`.
- The deferred/AO/transparency composition block now lives under the private `RenderPipeline::SceneCompositePasses`, bundling deferred lighting, deferred opaque decals, SSAO, GTAO, particles, and OIT resolve so those mid-frame pass owners no longer sprawl across the renderer façade state.
- The remaining non-geometry stream-backed concrete passes now live under `RenderPipeline::RenderStreamPasses`, bundling `ForwardOverlay`, `Foliage`, `Water`, and `Decal` so the pass owners behind `StreamNodes` no longer remain as a separate cluster of top-level façade fields.
- The frame-root pass owners now live under `RenderPipeline::FrameCorePasses`, bundling `Shadow` and `Scene` so the renderer façade no longer carries *any* concrete pass instances as standalone top-level fields.
- `RenderPipelineInputs` no longer exposes one giant flat raw-pointer bag; it now groups builder-facing data into nested `Nodes`, `Passes`, and `Runtime` views so the frame-graph builder tracks the same ownership boundaries as the renderer façade.
- The builder stage registrars no longer accept the whole `RenderPipelineInputs` bundle either: scene, transparency/AO, post, and render-stream registration now consume dedicated stage-input views, shrinking the remaining adapter surface between `Renderer3D` and the render-pipeline builder.
- External editor/scene/debug code no longer needs to pull `ScenePass` / `ShadowPass` just to read scene framebuffer, G-buffer, or shadow-availability state; those sites now go through narrower renderer-level accessors, and the unused `GetForwardOverlayPass()` surface has been removed.
- Scene traversal now submits terrain / voxel / foliage / mesh / skinned shadow casters through narrow `Renderer3D` shadow-submission helpers instead of reaching into `ShadowRenderPass` directly, further reducing concrete pass leakage out of the renderer façade.
- Editor and scene code no longer pull `SelectionOutlinePass`, `UICompositePass`, or `ParticlePass` just to feed selected IDs or render callbacks; those interactions now go through narrow façade setters on `Renderer3D`.
- The editor-side live command-bucket inspector no longer needs a dedicated renderer escape hatch either: `CommandPacketDebugger` now resolves the geometry stream bucket from the graph-owned `ScenePass` node, and the public `Renderer3D::GetCommandBucket()` debug accessor has been removed.
- Live render-graph inspection no longer tunnels through the renderer façade either: `RenderGraphDebugRuntime` now tracks the active graph for editor/debug consumers, `EditorLayer` and `RendererSettingsPanel` use that graph-owned debug surface directly, and the public `Renderer3D::GetRenderGraph()` accessor has been removed.
- `RenderGraphFrameCapture` now resolves post-process, UI-composite, AO, HZB, and G-buffer debug surfaces from canonical render-graph resource names instead of concrete pass objects, and the stale `Renderer3D::Get*Pass()` / `GetSceneGBuffer()` external call sites have been fully retired.
- Editor viewport/picking and frame-capture scene timeline inspection no longer reach for the live scene-pass target either: those paths now resolve the canonical `SceneColor` framebuffer through `ResolveFrameGraphFramebuffer(ResourceNames::SceneColor)` instead of using `Renderer3D::GetSceneFramebuffer()` as a general debug/editor escape hatch.
- Soft-particle submission now resolves the canonical `SceneDepth` texture from the graph too, queries its extent directly from GL, and no longer reaches into a live scene framebuffer for depth/size data. With that last production caller gone, the old `Renderer3D::GetSceneFramebuffer()` scene-target accessor and the unused `ReadEntityIDFromFramebuffer()` façade hook have both been removed.
- `SSSRenderPass` no longer performs a blur-shader-unavailable scene passthrough blit. `ConfigurePassesForFrame()` / `PopulateBlackboard()` now only materialize `SSSColor` when the blur shader is actually ready, so `PostProcessColor` falls back to `SceneColor` explicitly by handle selection instead of a hidden runtime copy.
- `AOApplyRenderPass` no longer performs an enabled-but-misconfigured passthrough blit. `ConfigurePassesForFrame()` / `PopulateBlackboard()` now only materialize `AOApplyColor` when the shader and graph inputs are actually ready, so the post chain falls back by explicit upstream aliasing to `SSSColor` / `SceneColor` instead of a hidden runtime copy.
- `BloomRenderPass` now follows the same graph-owned contract: `BloomColor` is only declared when bloom shaders are ready and a valid graph-owned mip chain can exist at the current scene resolution, so downstream passes fall back to `PostProcessColor` explicitly instead of relying on a runtime input-to-output blit when bloom prerequisites are missing.
- `SelectionOutlineRenderPass` now follows that same contract too: `SelectionOutlineColor` only materializes when the effect is enabled, selected entities exist, and the JFA shaders/UBOs are ready. Missing scratch or unavailable shaders now fail the pass contract instead of blitting the upstream scene through, and `UIComposite` falls back to the upstream late-post handle chain explicitly when `SelectionOutlineColor` is absent.
- The render-graph resource-descriptor helper has dropped one more history-shaped name too: `RGResourceDesc::FromLegacy(...)` was retired in favor of `RGResourceDesc::FromHandleKind(...)`, and the renderer/tests sweep no longer uses the old symbol anywhere.
- `RenderGraph` no longer synthesizes a final-pass name from sink nodes when none was explicitly requested. Production already sets `FinalPass` explicitly through `BuildRenderPipelineGraph(...)`; ad-hoc graphs without `SetFinalPass(...)` still keep all registered passes reachable, but they no longer pretend an inferred sink is a canonical final output.
- Render-graph resolve-failure telemetry now uses behavior-based naming end to end: `ResolveFailure`, `RecordResolveFailure(...)`, `GetResolveFailures()`, and dump fields `resolveFailureCount` / `resolveFailures` replaced the old “fallback” vocabulary, and the JSON dump schema moved to version 14 to reflect the renamed contract.
- The screen-space / post / present tail now lives under the private `RenderPipeline::PostProcessPasses`, bundling `SSS`, `AOApply`, `Bloom`, `DOF`, `MotionBlur`, `TAA`, `Precipitation`, `Fog`, the late post effects, `UIComposite`, and `Final` so the renderer façade no longer carries that pass-owner block as a dozen-plus separate top-level fields.
- With the debug/editor migration complete, the remaining unused concrete pass getters were removed from `Renderer3D`, leaving the façade focused on graph resources, render-intent submission, and narrow per-feature control hooks.
- The late single-attachment post chain (`ChromAb`, `ColorGrading`, `ToneMap`, `Vignette`, `FXAA`, and `SelectionOutline`) now resolves its *write* framebuffer from the frame blackboard during graph execution instead of always binding pass-owned outputs. The simple fullscreen stages (`ChromAb`, `ColorGrading`, `ToneMap`, `Vignette`, `FXAA`) no longer keep pass-owned current-frame output framebuffers at all; `GetTarget()` now reports the last resolved runtime surface instead.
- `SelectionOutline` and `UIComposite` now follow the same ownership rule for their current-frame outputs: both resolve graph-owned framebuffers from the blackboard at execute time, and selection-outline scratch sizing now comes from stable framebuffer specs instead of a live target handle. `SelectionOutline` now treats missing JFA execution resources as a graph-contract failure rather than rescuing the frame with a passthrough blit.
- `SelectionOutline` is no longer a topology-time special case: the pass is created and registered unconditionally like the rest of the late post chain, while per-frame blackboard handle validity decides whether `SelectionOutlineColor`/JFA scratch materialize. `UIComposite` now always prefers `SelectionOutlineColor` first and naturally falls through when that handle is invalid, removing the old selection-outline side-band boolean from the builder.
- `Renderer3D` now stages particle/UI callbacks plus selection-outline entity IDs in renderer-owned frame state and applies them during `ConfigureFrameGraphPassesForFrame()`. That trims another direct public-header dependency on concrete pass behavior without changing the external scene/editor call sites.
- `Renderer3D.h` no longer leaks concrete frame-pass ownership either: the grouped stream/pass/node state now lives behind a private `Renderer3DInternal.h` `RenderPipeline`, renderer implementation files route through `s_Data.Pipeline`, and the shadow-submission API's `NoBounds` default now comes from `BoundingVolume.h` instead of arriving transitively through `ShadowRenderPass.h`.
- Frame setup and post-resource declaration now use `SceneRenderPass::GetFramebufferSpecification()` rather than `SceneRenderPass::GetTarget()->GetSpecification()` when they only need frame dimensions (post transient sizing, TAA jitter, post-process inverse screen size, SSS blur resolution). That keeps dimension queries on stable metadata instead of a live framebuffer handle.
- Those same late outputs are now declared as graph-materialized transients from frame dimensions rather than borrowing pass-owned framebuffers through `DeclareTransientFramebuffer(..., ownerFB)`, which moves a concrete chunk of post-processing ownership from the legacy pass layer into the render graph itself.
- The earlier full-resolution pre-tonemap post chain (`SSS`, `AOApply`, `Bloom`, `DOF`, `MotionBlur`, `TAA`, `Precipitation`, and `Fog`) now resolves its *write* framebuffer from the frame blackboard during graph execution too, so those passes no longer bind pass-owned outputs as their primary render surface.
- Among those earlier fullscreen stages, `SSS`, `AOApply`, `Bloom`, `DOF`, `MotionBlur`, and `Precipitation` now match `TAA`/`Fog`: their current-frame output surface is graph-owned only, while resize bookkeeping is driven by framebuffer specs and `GetTarget()` reflects the last resolved graph surface instead of an owner-backed fallback framebuffer. Bloom's mip chain remains graph-owned scratch throughout.
- Those earlier full-resolution outputs are now declared as graph-materialized transients from scene dimensions rather than borrowing pass-owned framebuffers.
- `TAA` and `Fog` still keep their persistent history buffers pass-owned/imported, but both passes now read previous-frame history through the render-graph blackboard and write next-frame history back through explicit `ExtractHistoryTexture(...)` contracts instead of mutating history storage invisibly inside the main pass body.
- `FogPass` also exposes its half-resolution integration result as a graph-known scratch framebuffer (`FogHalfRes`), so the temporal-reprojection source for volumetric fog is now part of the graph resource model rather than a purely hidden pass-local surface.
- The render-graph format vocabulary now includes integer single-channel framebuffer support (`R32Int` -> `RED_INTEGER` / `GL_R32I`), which lets mixed MRT framebuffers stay graph-described instead of falling back to pass-owned declarations.
- `UIComposite` is now fully graph-described as a mixed MRT transient (`RGBA8` color + `R32Int` entity ID + `RG16Float` normal) instead of being the last owner-backed exception in the late post/UI tail.
- The OIT path now follows the same rule: its MRT + depth target is graph-materialized and prepared by an explicit graph pass rather than borrowed from a pass-owned helper object.
- Water submission no longer bakes viewport-sized `screenParams` through `Scene.cpp` by querying the scene framebuffer up front: `CommandDispatch` now derives the water UBO screen-size terms from the active `SetViewport` state at execution time, removing another frontend dependency on a live pass-owned framebuffer.
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
