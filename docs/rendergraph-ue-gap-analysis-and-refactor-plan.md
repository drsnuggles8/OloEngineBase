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

- `Renderer3D::EndScene()` runs `BuildFrameGraph()`, refreshes blackboard handles, then executes the compiled plan.
- `RenderGraph::BuildFrameGraph()` runs per-node setup callbacks, derives dependencies from declared reads and writes, updates topo order, computes reachability, computes barriers, rebuilds the transient plan, and caches a submission IR.
- `RenderGraph::Execute()` replays a compiled submission plan rather than traversing topology ad hoc.
- `RenderGraph` supports imports, transient declarations, resource resolve, extraction, history contracts, hazard validation, barrier diagnostics, transition records, and resource lifetime records.
- `RenderGraph` already models pass work type and async-compute candidacy.
- The transient planner already computes alias groups and alias slots.
- Some graph setup is subresource-aware, for example the GTAO HZB mip chain.

What is still hybrid:

- the primary authoring object is still `RenderPass`, a long-lived legacy pass type with `Init()`, framebuffer ownership, `Execute()`, optional setup callback, and legacy `DeclareRead()` and `DeclareWrite()` metadata;
- the production graph is built by adapting existing pass instances into graph nodes via `PrepareGraphPass()` and `SetSetupCallback()`;
- a large share of important resources are still imported from pass-owned or renderer-owned objects rather than created as graph-owned resources;
- histories are still persisted by pass-owned framebuffers and explicit copy-back logic inside pass code;
- blackboard handles must be refreshed after `BuildFrameGraph()` because handle generations can change when the registry rebuilds;
- pass runtime code still contains fallback chains and resource-selection logic that ideally belongs in graph setup;
- insertion order still matters for correctness in at least one place, which is a sign that the graph is not yet the sole source of execution truth.

## Gap Analysis

| Area | Current OloEngine | UE-level target | Gap | Priority |
|---|---|---|---|---|
| Pass authoring model | `RenderPass` remains the main abstraction. Graph setup is bolted on through `SetupCallback`. | Graph pass declaration is the primary source of truth. No parallel legacy pass contract. | High | P0 |
| Resource declaration source | Mixed across `DeclareRead/Write`, setup lambdas, blackboard imports, and execute-time fallback logic. | One canonical declaration path per pass. | High | P0 |
| Graph ownership of outputs | Many outputs are now graph transient, but several core and history resources remain pass-owned or renderer-owned. | The graph owns all frame-local resources and explicitly imports only true external resources. | High | P0 |
| Handle stability | Blackboard handles must be refreshed after build because generations can change. | Handles used for a frame remain stable for the life of that compiled frame. | High | P0 |
| Resource versioning | Canonical names plus last-writer tracking. No first-class versioned handle model for write-renaming. | Resource versions or views are explicit in graph semantics. | High | P1 |
| Extraction semantics | Extraction is a post-execute callback queue and does not currently root liveness. | Extraction and external access are first-class graph roots. | High | P0 |
| History semantics | Histories are imported into the graph but persisted by pass-owned copy-back code. | Histories are graph-level external resources with explicit ingress and egress contracts. | High | P1 |
| Render bucket integration | Buckets are still associated with legacy pass or stream objects. | Buckets are a graph execution strategy owned by graph-native nodes. | High | P0 |
| Runtime selection logic | Some passes still decide their true inputs at execute time via fallback ladders. | Input selection is resolved during setup; execute records commands against already-decided handles. | Medium | P1 |
| Topology derivation robustness | Dependency derivation is strong, but insertion order still acts as a semantic tie-break for correctness. | Ordering correctness should come from resource or explicit dependency declarations, not registration order quirks. | High | P0 |
| Barrier model | Logical barrier planning exists and maps to GL memory barriers. | Backend-neutral transition planning separated cleanly from backend execution. | Medium | P1 |
| Async compute | Compiler and IR support exist, but GL path only emits debug markers for batches. | True async on explicit backends; conservative single-queue replay on GL. | Medium | P2 |
| Imported vs transient boundary | Boundary is improving, but some graph-visible resources are still really pass internals. | External resources are rare and explicit; scratch and intermediate resources are graph-owned. | High | P1 |
| Graph debugability | Good diagnostics already exist. | Keep and expand diagnostics to cover authoring misuse and extraction roots. | Low | P2 |
| Backend portability | Submission IR and transition metadata are promising but still live inside one monolithic `RenderGraph` type. | Clear split between graph compiler, scheduler, transition planner, allocator, and backend executor. | Medium | P2 |

## Concrete Code-Backed Gaps

### 1. The graph compiler is real, but the authoring model is still adapter-based

Current evidence:

- `RenderPass` owns the setup callback hook and legacy declarations.
- `PrepareGraphPass()` wraps an existing pass and injects a setup lambda.
- The production builder registers existing pass instances and only sets the final pass explicitly.

Implication:

- OloEngine has a frame-graph compiler, but not yet a fully graph-native pass surface.
- The design still pays for two mental models: old pass lifecycle plus new graph setup.

UE-level target:

- each node is authored directly as a graph node or graph pass descriptor;
- there is no second older pass contract that must be kept in sync.

### 2. Resource ownership is split between graph resources and pass-owned resources

Current evidence:

- `RenderPipeline::PopulateBlackboard()` imports many resources from existing pass targets.
- TAA and Fog histories are imported into the graph but persisted by pass-owned history framebuffers and manual `glCopyImageSubData()` callbacks.

Implication:

- the graph is not yet the sole owner of frame-local resource lifetimes;
- history handling is graph-aware, but not graph-owned end to end.

UE-level target:

- all frame-local scratch and post-process intermediates are graph-created;
- histories and other cross-frame resources are modeled as graph-external resources with explicit ingress and egress, not hidden pass internals.

### 3. Handle generations leaking into frame logic are a structural smell

Current evidence:

- `RefreshBlackboardHandles()` exists because registry rebuilds can invalidate generations captured earlier in the frame.

Implication:

- the runtime must repair authoring-time handles after compile;
- this is workable but not the clean graph contract UE-level authoring expects.

UE-level target:

- frame handles are stable once the frame graph is built;
- no post-build handle repair step is needed to keep runtime code correct.

### 4. Extraction is not yet a liveness root

Current evidence:

- `FlushExtractions()` diagnoses extraction of culled resources instead of making extraction a root that preserves producers.

Implication:

- a pass producing only an extracted output can still be culled if nothing in the final-pass chain consumes it;
- that is weaker than a mature render graph.

UE-level target:

- extraction or external-access requests must keep the producing subgraph alive.

### 5. Runtime fallback logic still duplicates setup intent

Current evidence:

- several fullscreen passes choose the first valid upstream framebuffer at execute time;
- the builder performs a similar selection during setup via `ReadFirstValidFramebuffer()`.

Implication:

- declaration and execution are not fully aligned;
- there is duplicated policy and more room for desync.

UE-level target:

- setup decides the actual inputs;
- execute only records commands using resolved handles chosen by setup.

### 6. Insertion order still affects correctness

Current evidence:

- `RenderPipelineBuilderScene.cpp` explicitly warns that registration order must stay aligned to avoid a derived dependency cycle.

Implication:

- the graph compiler is strong, but not yet fully authoritative;
- registration order still carries semantic weight.

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
2. No pass uses `DeclareRead()` or `DeclareWrite()` as a second legacy declaration path.
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

1. Mark `RenderPass::SetSetupCallback()` and `DeclareRead()` or `DeclareWrite()` as transitional APIs in comments and diagnostics.
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
4. Deprecate `DeclareRead()` and `DeclareWrite()` once all runtime nodes declare access through `RGBuilder` only.
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
3. Replace pass-owned history framebuffers with graph-managed history resource contracts.
4. Add explicit builder or graph APIs for:
   - import external history;
   - extract history for next frame;
   - convert extracted graph resources into next-frame imported histories.
5. Make extraction and history write-back liveness roots.

Acceptance criteria:

- TAA and Fog history persistence no longer depends on pass-private framebuffer ownership;
- scratch resources such as Bloom mips, JFA ping-pong, HZB, GTAO edge, SSAO raw, and fog half-res are graph-owned end to end;
- extraction keeps required producers alive.

## Phase 5: Add First-Class Resource Views and Versioned Write Semantics

Goals:

- remove remaining ambiguity around framebuffer-wide names vs attachment views;
- reduce reliance on canonical mutable names.

Work items:

1. Add first-class graph resource views for:
   - framebuffer attachment views;
   - mip views;
   - array-layer views;
   - cube-face views;
   - multisample resolve views.
2. Introduce explicit write-version semantics or versioned handles for resources that are logically rewritten in sequence.
3. Model OIT, G-buffer attachments, and post outputs with explicit views instead of mixed framebuffer and attachment naming shortcuts.
4. Add feedback declarations for legal same-resource read-modify-write cases.

Acceptance criteria:

- OIT and G-buffer resources have one consistent representation;
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
2. Remove production reliance on `DeclareRead()` and `DeclareWrite()`.
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

The refactor is complete when all of the following are true:

1. `Renderer3D::EndScene()` does not repair graph handles after compile.
2. Production passes no longer use setup callbacks.
3. Production passes no longer rely on legacy declaration metadata for correctness.
4. Extraction and history write-back are graph roots.
5. Bucket replay occurs only through graph-native nodes.
6. Graph correctness does not rely on registration order hacks.
7. All frame-local scratch and post-process intermediates are graph-owned.
8. The GL backend replays a compiled submission plan and transition plan, even if it serializes everything.
9. Adding a future explicit backend does not require changing pass authoring semantics again.

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