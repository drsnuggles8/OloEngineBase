# RenderGraph Roadmap

> **Status:** Living design doc. Reflects the `feature/rendergraph_rework`
> branch. Owned by the rendering subsystem — keep
> in sync with [renderer-testing.md](renderer-testing.md) and
> [deferred-renderer.md](deferred-renderer.md) whenever the graph contract
> changes.

This document tracks the long-term evolution of OloEngine's RenderGraph from
its current incarnation — a statically-wired pass list with a hazard validator
— towards a true *adaptive* render graph in the mould of Frostbite's
FrameGraph and Unreal Engine 5's RDG. It exists because the current design
serves us well today but is architecturally capped, and future features
(GPU-driven rendering, transient memory, async compute) will need the richer
model documented below.

---

## 1. Where we are today

The current `OloEngine::RenderGraph`
(`OloEngine/src/OloEngine/Renderer/RenderGraph.h`) is best described as a
**topologically-ordered, statically-wired pass list with a hazard
validator**:

* Passes are `Ref<RenderPass>` instances. `Renderer3D::s_Data` keeps a
  long-lived strong reference to each pass while it is registered, and
  `RenderGraph::m_PassLookup` is a `std::unordered_map<std::string,
  Ref<RenderPass>>` — also a strong reference. `RenderGraph::ResetTopology()`
  drops the graph's references but external owners (s_Data) continue to keep
  the pass alive.
* Wiring is done manually during `Renderer3D::ConfigureRenderGraph` via
  explicit calls to:
  * `AddPass(ref)` — register a pass in insertion order.
  * `AddExecutionDependency(producer, consumer)` — ordering-only edges.
  * `ConnectPass(producer, consumer)` — framebuffer piping edges.
  * `SetFinalPass(name)` — designate the swap-chain writer.
* `ValidateResourceHazards()` walks each pass's `DeclareRead` /
  `DeclareWrite` sets, performs a transitive-closure over the dependency
  edges, and reports RAW / WAW / WAR / Cycle problems.
* Execution is a straight linear walk of the cached topological order.
  Resources (framebuffers, textures, SSBOs) are owned and managed by the
  passes themselves; the graph never allocates GPU memory.
* Mode switching (Forward ↔ Forward+ ↔ Deferred) is handled by
  `ResetTopology()` + rebuilding via `ConfigureRenderGraph` — the current
  mechanism that made this roadmap worth writing.

### Current architecture audit: ownership and data flow

The dynamic-graph refactor must preserve several current contracts that are
not obvious from the `RenderGraph` API alone:

* **Pass lifetime is owned outside the graph.** `Renderer3D::s_Data` owns
  long-lived `Ref<>` instances for the production passes. `RenderGraph` also
  keeps strong references while a pass is registered, then builds a cached
  execution array of non-owning `RenderPass*` values. That cache is valid only
  until the next topology rebuild. Any RDG compiler must either keep this
  invariant explicit or replace it with per-frame compiled pass records whose
  lifetime is owned by the graph.
* **Passes are mutable after graph construction.** `Renderer3D::EndScene()`
  updates pass settings, texture IDs, UBO pointers, G-Buffer pointers, OIT
  toggles, and callbacks immediately before `RGraph->Execute()`. A true RDG
  cannot treat pass setup as immutable unless these values move into an
  explicit frame context / graph blackboard.
* **Resource declarations are partial metadata today.** `DeclareRead` /
  `DeclareWrite` are opt-in string declarations. They are useful for the
  current hazard validator, but they do not carry descriptors, access modes,
  subresources, usage states, queue ownership, or lifetime intent. The `Kind`
  enum in `ResourceHandle` is diagnostic metadata only; identity is currently
  name-based.
* **Several resources are pass-owned and persistent.** Examples include
  `SceneRenderPass`'s scene framebuffer, its lazily-created `GBuffer`,
  `PostProcessRenderPass`'s ping-pong framebuffers, bloom mip chain,
  fog half-res/history buffers and TAA history, `SSAORenderPass`'s raw/blur
  framebuffers, `GTAORenderPass`'s AO/edge/HZB textures, `SSSRenderPass`'s
  blur output, `UICompositeRenderPass`'s target, and `OITResolveRenderPass`'s
  `OITBuffer`. `WaterRenderPass` also owns a raw GL refraction texture.
* **Some important resources are external to passes.** `Renderer3D::s_Data`
  owns global UBOs / SSBOs, the `ShadowMap`, global IBL texture IDs, the
  Forward+ light grid, frame-data buffers, and temporal transform caches.
  These are real producer/consumer dependencies even though most are not
  represented as graph resources yet.
* **Framebuffer handoff is a side channel.** Graph `ConnectPass()` invokes
  `SetInputFramebuffer()` each frame, while many other handoffs are wired
  directly by `Renderer3D` through setters such as `SetSceneFramebuffer`,
  `SetSceneDepthFramebuffer`, `SetGBuffer`, `SetSSAOTexture`,
  `SetVelocityTextureID`, and `SetOITBuffer`. The dynamic graph must retire
  this implicit passing in favour of typed handles resolved at execute time.
* **Command buckets are per-frame payloads, not graph resources.** Draw calls
  are recorded into pass-owned `CommandBucket`s before the graph executes.
  The RDG should schedule the passes that consume those buckets, but it should
  not try to replace the bucket / `FrameDataBuffer` architecture. Instead,
  command buckets become pass payloads whose GPU resource accesses are still
  declared to the graph.
* **Command submission is split today.** Some passes replay sorted draw
  packets through `CommandBucket` / `CommandDispatch` (`SceneRenderPass`,
  foliage, decals, water, forward overlay), while many fullscreen, compute,
  copy, resolve, and state-management operations call `RenderCommand` or raw
  GL directly inside `Execute()`. Examples include post-process effects,
  deferred lighting, SSAO/GTAO/HZB compute, OIT resolve, shadow rendering,
  G-Buffer resolves, draw-buffer changes, blits, memory barriers, and
  per-pass prologue/epilogue state restores. This is not automatically wrong,
  but it is currently invisible to the graph and can leak state unless guarded.
* **Some current passes are mini-graphs.** `PostProcessRenderPass` internally
  builds an effect chain with ping-pong buffers, bloom mips, fog history,
  TAA history, AO application, and optional effects. `GTAORenderPass` owns an
  HZB subgraph. `OITResolveRenderPass` owns accumulation targets that Water,
  Decal, and Particle passes write. A true RDG should eventually express
  these as graph nodes and resources, not opaque internals.
* **Passthrough targets are dynamic.** `PostProcessRenderPass`, `SSSRenderPass`,
  and `OITResolveRenderPass` can return the input framebuffer when their work
  is disabled or no accumulation was produced. In RDG terms this should be an
  explicit alias / forwarded handle, not a runtime `GetTarget()` surprise.

The current roadmap therefore needs more than typed handles: it needs an
ownership model, an explicit frame-building API, and a plan for migrating
every side-channel handoff into graph-visible resources.

### What this design is good at

* **Simple to read and debug.** The wiring is a single flat block of calls
  in `Renderer3D.cpp`, with inline comments documenting every edge.
* **Zero per-frame allocation overhead.** Pass instances and their
  framebuffers persist; only the cached pipe map is rebuilt when the
  dependency set changes.
* **Hazard validation at setup time.** A mistake in the wiring fails loudly
  in Debug rather than producing subtle corruption at runtime.
* **Fits the engine's render-command model.** Passes enqueue into
  `CommandBucket`s; the graph only orders pass execution, not individual
  GPU operations.

### What this design can't do

1. **Resource lifetime is the pass's problem.** Framebuffers, cubemaps,
   SSBOs, UBOs are owned by individual passes and live for the full app
   lifetime. There is no concept of a "transient" resource that the graph
   could alias across non-overlapping pass ranges to reduce VRAM.
2. **No automatic barriers / transitions.** We lean on GL's implicit
   synchronisation today, but a Vulkan/DX12 backend would need explicit
   barrier insertion that the graph currently cannot derive on its own.
3. **Passes must be topologically present to participate.** Even the
   per-mode rebuild is coarse: we switch between fully-preset
   topologies, not evolve one. A pass that is cheap in one scene and
   heavy in another can't scale dynamically.
4. **No pass culling from the final output.** Every registered pass runs
   every frame (subject to internal early-outs). If the Bloom stage writes
   a buffer no one reads, we still pay for it.
5. **View / slice decoupling is manual.** A "render this scene twice from
   different cameras into different targets" workflow (shadow cascades,
   cubemap faces, planar reflections, stereo VR) requires either bespoke
   code in each pass or multiple pass instances — the graph can't express
   "run this subgraph with these inputs substituted".
6. **Async compute isn't expressible.** All work is serial across a single
   timeline; there's no way to hint that two passes with no data
   dependency should overlap on different queues.
7. **Graph visualisation was out-of-band.** We have `ValidateResourceHazards`
   reporting and `RenderGraph::DumpToDot(path)` now emits a GraphViz DOT
   snapshot of the current topology — useful when tracking down correctness
   bugs or verifying per-path rebuilds. Live per-frame metrics (timings,
   resource-access traces) are still out of scope.
8. **Backend details leak through pass code.** `RenderCommand` currently hides
  the active `RendererAPI`, but pass code still uses OpenGL concepts directly:
  `GLenum` state, raw GL calls, named framebuffer operations, image barriers,
  and texture IDs. That is manageable for a GL-only renderer, but it is the
  wrong public contract for a graph that should lower cleanly to Vulkan/DX12.

---

## 2. Where we want to go — true adaptive RDG

A true adaptive RenderGraph reframes the design so that **resources are
first-class graph entities** and **passes declare intent against virtual
resources** which the graph compiler then realises into physical GPU
memory and execution orderings per frame.

### 2.1 Core concepts

* **Resource handles.** `RGTextureHandle`, `RGBufferHandle`,
  `RGFramebufferHandle` — opaque integer handles allocated by the graph,
  not by passes. Each handle carries a *descriptor* (format, dimensions,
  bind flags, sample count, mip count, ...).
* **Virtual resources.** When a pass calls
  `builder.CreateTransient(desc)` the graph records a resource *description*
  but does not allocate anything. Physical memory is assigned at compile
  time based on lifetime analysis.
* **Declarative reads/writes.** Each pass, during a *setup* callback, uses
  a `RGBuilder` to declare which handles it reads and writes. The *execute*
  callback receives resolved `TextureHandle`s / `FramebufferHandle`s and
  issues GPU work.
* **The graph compiler**:
  1. Traces backwards from the final output (the swap-chain writer) to
     determine which passes are **reachable**. Unreached passes are
     culled — their resources are never allocated.
  2. Computes per-resource **lifetimes** (first-write pass index to
     last-read pass index across the linearised execution).
  3. Allocates resources from a **transient pool** using lifetime overlap
     to alias memory between resources that don't co-exist. On Vulkan
     this is a single `VkDeviceMemory` backing many `VkImage`s; on GL
     4.6 it can be a simpler "pool of FBOs by dimension/format" cache.
  4. Inserts **barriers / layout transitions** automatically based on
     the last-used state of each resource vs. the next pass's usage.
  5. Emits a final **command list** that passes execute against.

### 2.2 Target data model and ownership contracts

The target design should make the following ownership rules explicit:

* **`Renderer3D` owns renderer systems; the graph owns frame products.**
  Renderer systems keep shaders, materials, command buckets, persistent
  history objects, and editor callbacks. The per-frame `RenderGraph` owns
  virtual resources, compiled pass records, transient allocation decisions,
  and resolved resource views for that frame.
* **Handles are typed, generational, and descriptor-backed.** A handle should
  identify a graph resource plus a generation, so stale handles from a previous
  compile are rejected. Texture and buffer handles carry descriptors:
  dimensions, format, samples, mip count, array layers, bind flags, clear
  value, and debug name.
* **Resource access is usage-specific.** Reads and writes must declare an
  access mode such as shader sample, colour attachment, depth read,
  depth write, storage read/write, transfer source/destination, indirect
  argument, or uniform/constant read. These modes are what barrier synthesis
  and validation operate on.
* **Subresources are first-class.** Mips, array layers, cube faces, and MSAA
  resolve targets need to be described independently. Shadow cascades, point
  shadow cubemap faces, bloom mips, and future reflection probes should be
  materialised as views of a resource, not separate ad-hoc pass instances.
* **External resources are imported; histories are extracted.** Swap-chain
  images, asset textures, global IBL maps, existing shadow atlases, persistent
  TAA/fog histories, picking/readback buffers, and long-lived UBO/SSBO objects
  enter the graph through `Import*` calls. Resources that must survive the
  frame are queued for extraction at the end of execution.
* **Transient resources are never exposed as raw GL IDs during setup.** Setup
  declares intent only. Execute receives resolved `RGTextureView` /
  `RGBufferView` / framebuffer bindings produced by the compiler. Raw
  renderer IDs may only be observed inside backend execution code.
* **The graph API is backend agnostic.** Public RDG descriptors, resource
  states, load/store actions, barrier scopes, queue flags, and command-context
  operations must use OloEngine-owned enums and structs, not `GLenum`, raw GL
  texture IDs, `VkImage`, `ID3D12Resource`, or backend-specific binding
  layouts. The compiler produces an API-neutral intermediate command stream
  which the active backend lowers to GL 4.6 now and Vulkan/DX12 later.
* **Pass data is immutable at execute time.** Per-frame settings currently
  pushed through pass setters should be captured into pass parameter structs
  or graph-blackboard entries during setup. Execute should not query global
  mutable renderer state except through an explicit frame context.
* **Command emission goes through a graph-visible context.** The RDG does not
  require every GPU operation to become a sortable `CommandBucket` packet.
  Fullscreen passes, compute dispatches, clears, resolves, copies, barriers,
  and pass-local state setup can remain immediate-style work, but they should
  be issued through an API-neutral `RGCommandContext` / backend encoder
  provided by the graph. The context records pass labels, validates resource
  accesses, installs compiler-generated barriers, tracks/restores boundary
  state, and can replay existing `CommandBucket`s inside the same pass scope.
* **The graph blackboard names canonical frame resources.** Standard handles
  such as scene colour, scene depth, velocity, G-Buffer attachments, AO,
  post-process output, UI composite, final colour, TAA history, fog history,
  and shadow outputs should live in a typed blackboard rather than stringly
  typed pass lookups.
* **Legacy pass objects are a short-term bridge, not a strategy.** Early
  phases may temporarily wrap existing `RenderPass` instances only when a
  direct refactor would block delivery. The target architecture remains
  stateless pass setup + execute callbacks similar to UE's `AddPass`, and
  each temporary bridge must carry an explicit removal milestone.

### 2.3 Unreal / Frostbite feature parity target

The final product does not need every UE implementation detail, but it should
offer the important RDG capabilities and use them in OloEngine's renderer
where they fit:

| RDG feature | OloEngine target | First production users |
|---|---|---|
| Typed virtual textures / buffers | `RGTextureHandle`, `RGBufferHandle`, descriptors, subresource views | G-Buffer, scene colour/depth, AO, bloom, OIT, HZB |
| Imported / extracted resources | Register external textures, buffers, swap-chain images, histories; extract next-frame histories | Backbuffer, IBL maps, shadow atlases, TAA history, fog history |
| Backend-agnostic graph IR | RDG public API uses engine enums/descriptors and lowers to GL/Vulkan/DX12 backends | All migrated passes; barrier compiler; transient allocator |
| Graph blackboard | Typed frame-resource slots instead of pass-name lookups | Scene/deferred/post/UI/final chain |
| Setup / execute split | Pass setup declares resources; execute receives resolved views and pass parameters | All render passes; especially post-process and AO |
| Graph command context | Execute receives an encoder that wraps `RenderCommand`, raw backend calls, and `CommandBucket` replay in one pass scope | Fullscreen passes, compute passes, blits/resolves, scene command buckets |
| Pass culling | Backward reachability from final outputs plus feature-toggle pruning | Disabled bloom/SSAO/GTAO/SSS/OIT/selection-outline branches |
| Transient pooling / aliasing | Lifetime-based aliasing of compatible textures/framebuffers | Post-process ping-pong, bloom chain, SSAO scratch, SSS blur, OIT accum |
| Automatic barriers | Backend-specific transitions from declared access modes | GL memory barriers for compute/image paths; Vulkan/DX12 later |
| Async compute flags | Mark compute-only passes and schedule when dependencies allow | HZB, GTAO, denoise, bloom downsample/upsample candidates |
| View / slice instancing | Materialise one logical pass over multiple views/subresources | CSM cascades, point-light cube faces, probes, planar reflections, VR |
| Temporal resource support | Explicit previous/current frame resources and resize invalidation | TAA, motion vectors, fog history, temporal AO, future TSR |
| Readback / extraction passes | Explicit copy/readback nodes with lifetime tracking | Entity picking, screenshots, tests, GPU counters |
| Debug/profiling introspection | DOT/JSON dumps, resource-lifetime table, aliasing report, pass timings | Editor RenderGraph panel, RenderDoc/Superluminal labels, tests |

### 2.4 Concrete benefits

| Area | Benefit |
|---|---|
| VRAM | Transient aliasing for render targets: Bloom's half-res chain, SSAO's noisy buffer, GTAO's scratch, post-process intermediates — none need to live simultaneously. Real engines see 30-50% render-target memory savings from this alone. |
| CPU | No per-frame "hook up framebuffer X to pass Y" boilerplate in our code; it's derived from declarations. |
| Correctness | Barriers become impossible to forget. Dev adds `builder.Read(handle, RGReadUsage::ShaderSample)` and the graph inserts the transition. |
| Async compute | Two compute-only passes (e.g. GTAO denoise + bloom downsample) can be marked async; the graph splits the submission across queues when their reads/writes don't overlap. |
| View decoupling | Shadow cascades express as `for (i = 0..3) AddPass(ShadowPass, {view: cascadeView[i], output: cascadeTex[i]})` — same pass, four materialised instances. Same trick handles cubemap faces, planar reflections, stereo VR without 6 copies of every shadow pass. |
| Debug | Dumping the compiled graph as DOT / JSON is trivial: we already know every handle, every read/write, every pass. Drop-in integration with RenderDoc / Superluminal overlays. |
| Pass culling | Toggle "Bloom off" in editor → `PostProcessPass` stops writing bloom handle → Bloom subgraph becomes unreachable → automatically skipped. No `if (!bloomEnabled) return;` scattered through pass Execute. |
| GPU-driven | When we get to GPU-driven rendering (indirect draws, mesh shaders, cluster culling), the graph's understanding of which passes produce/consume which buffers becomes the scaffold for transitioning between CPU-scheduled and GPU-scheduled work. |

### 2.5 Prior art worth studying

* **Frostbite FrameGraph** — Yuriy O'Donnell, GDC 2017, *"FrameGraph:
  Extensible Rendering Architecture in Frostbite."* The canonical
  introduction; introduced the setup/execute split and the transient
  resource allocator. Slides are still the best-written single reference.
* **Unreal Engine 5 RDG** — UE's `FRDGBuilder`. Production-grade, adds
  pooled buffers, async compute scheduling, culling by visibility,
  automatic parallel command-list recording.
* **bgfx views** — simpler model; views are numbered execution slots with
  state attached. Good reference for "start small" ergonomics.
* **Granite** — Hans-Kristian Arntzen's personal Vulkan engine.
  Open-source, readable, and its RenderGraph is closer in scope to what
  OloEngine needs than UE's.
* **"Render graphs" chapter in *GPU Zen 2*** — survey of multiple
  implementations.
* **RenderDoc's FrameGraph view** — instructive for the kind of
  introspection we'd get for free.

---

## 3. Migration plan

### Phase 0 — Close the current contract gaps

Before changing ownership, make the current Option 3 graph accurately describe
what it already does:

* Complete production `DeclareRead` / `DeclareWrite` coverage for all shared
  resources: scene colour/depth/normals/velocity, G-Buffer attachments,
  deferred lighting inputs, AO outputs, OIT accumulation/revealage, SSS
  output, post-process output, selection outline, UI composite, shadow maps,
  global IBL, HZB/GTAO textures, and relevant UBO/SSBO resources.
* Replace ad-hoc string literals in tests with canonical `ResourceNames`
  entries. Add names for resources that exist today but are not represented
  yet: G-Buffer albedo/normal/emissive/velocity/depth, HZB, SSAO raw/blur,
  GTAO AO/edge, SSS colour, TAA history, fog history, light grid, scene
  velocity, water refraction, and swap-chain/backbuffer.
* Add structural tests for every side-channel currently wired by setters:
  AO texture selection, OIT enabled/disabled paths, deferred decal insertion,
  selection-outline insertion, TAA velocity source selection, and post-process
  passthrough behaviour.
* Treat direct `RenderCommand` / raw GL usage as Phase 0 containment work, not
  as something to discover later. Inventory every production pass that calls
  `RenderCommand` or raw GL directly, classify each call as pass setup/clear,
  fullscreen draw, compute dispatch, copy/resolve/blit, barrier,
  debug/readback, or temporary state mutation, and mark whether it should
  become a graph command-context call, a resource declaration/barrier, or a
  backend-only implementation detail.
* Define the temporary command-boundary rule: no new production render pass
  should introduce unclassified direct `RenderCommand`,
  `RenderCommand::GetRendererAPI`, or raw GL usage. Until `RGCommandContext`
  exists, unavoidable direct calls should use the smallest possible
  compatibility scope with a debug label, state guard, owner, and planned
  deletion release. Bridge code must be tracked as migration debt, not
  normalized as a permanent layer.
* Define API-neutral graph vocabulary before adding typed resources: formats,
  load/store actions, access modes, pipeline stages, queue types, sampler/image
  binding intent, and clear values should be represented by OloEngine types.
  GL enums are allowed only in the GL backend translation layer.
* Document which current resources are permanent, temporal histories,
  transient candidates, imported assets, or pass payloads. This inventory is
  the input to the real RDG resource registry.

Exit criterion: the existing graph still executes exactly as today, but the
validator can no longer be bypassed accidentally by an undeclared production
pass touching a shared resource, and no unclassified direct backend command
path remains in production pass code.

### Phase A — Typed handles and a resource registry

* Introduce typed, generational `RGTextureHandle`, `RGBufferHandle`, and
  `RGFramebufferHandle` wrappers. Keep only a narrow compatibility shim that
  maps old `ResourceHandle` names to typed handles while passes migrate, and
  remove it incrementally as each subsystem switches to typed declarations.
* Add `RGResourceDesc` records for textures and buffers. Descriptors include
  dimensions, format, sample count, mip count, layer count, bind flags, clear
  values, ownership class, and debug label. These descriptors use the
  API-neutral vocabulary defined in Phase 0; backend-specific formats and
  flags are produced only while lowering to the active renderer backend.
* Add `RGResourceRegistry` to map handles to either existing physical
  resources (imported) or virtual resources (transient). At this stage,
  imported resources can still be the current pass-owned framebuffers.
* Extend validation to check descriptor compatibility, duplicate writers,
  subresource overlap, resource-kind mismatches, and reads of unproduced
  non-imported resources.

Exit criterion: pass wiring may still be manual, but every graph-visible
resource has a type, descriptor, producer/consumer set, and ownership class.

### Phase B — Blackboard, imports, and extraction ✅ Complete

* Add a typed frame blackboard for canonical frame resources. `Renderer3D`
  should publish scene colour/depth, velocity, G-Buffer handles, AO, post
  colour, UI composite, final colour, history inputs/outputs, and imported
  global resources through this blackboard.
* Add `ImportTexture`, `ImportBuffer`, `ImportFramebuffer`, and `ImportHistory`
  calls for swap-chain targets, existing shadow/IBL resources, persistent
  histories, global UBOs/SSBOs, and asset textures.
* Add queued extraction for resources that persist across frames: TAA history,
  fog history, future temporal AO/SSR histories, readback buffers, and editor
  picking results.
* Replace direct pass-to-pass setters as a priority. If a compatibility
  bridge still calls legacy setters during execute, setup must declare the
  handle that will be resolved and the bridge must have an owner + removal
  milestone.

**Implemented:**
- `FrameBlackboard` struct (`OloEngine/src/OloEngine/Renderer/FrameBlackboard.h`) with typed handles for all canonical frame resources.
- `RenderGraph::ImportTexture/ImportFramebuffer/ImportBuffer/ImportHistory` — register physical resources and return typed generational handles.
- `RenderGraph::ResolveTexture/ResolveFramebuffer/ResolveBuffer` — validate + retrieve physical resource from handle.
- `RenderGraph::ExtractTexture/ExtractFramebuffer/FlushExtractions` — deferred callback queue for persistent resource readback.
- `RenderGraph::GetBlackboard/ClearBlackboard` — per-frame typed handle store.
- `Renderer3D::SetupFrameBlackboard()` — called at end of `BeginSceneCommon()`; imports all live physical resources (scene, G-Buffer, velocity, AO, shadows, post-process, OIT, TAA/fog histories, IBL) into the blackboard each frame.
- `PostProcessRenderPass::GetTAAHistoryTextureID/GetFogHistoryTextureID` — new accessors for temporal history texture IDs.

Exit criterion: frame resources are passed by handles through a blackboard;
direct `Ref<Framebuffer>` handoffs become legacy glue, not the source of
truth.

**Phase C — Setup/execute split and per-frame compilation** ✅ **COMPLETE** ([2026-04-28])

* Introduce `RGBuilder` with `Read`, `Write`, `Create`, `Import`, `Extract`,
  and `UseBlackboard` operations. Reads/writes include access usage and
  subresource range.
* Convert production passes to a setup/execute callback pair by default.
  Temporary wrappers around existing `RenderPass` objects are allowed only
  for blocked subsystems and must include a deprecation ticket and target
  removal phase.
* Move per-frame pass settings currently pushed through setters into pass
  parameter structs captured at setup time. Keep command buckets as pass
  payloads with explicit resource declarations.
* Introduce an `RGCommandContext` for execute callbacks. It should provide
  backend operations for clears, viewport/scissor, draw-buffer selection,
  fullscreen draws, compute dispatch, image copies, framebuffer blits,
  memory barriers, and `CommandBucket` replay using API-neutral parameters.
  Legacy pass forwarding to `RenderCommand` is a temporary fallback only and
  should be eliminated subsystem-by-subsystem; new RDG code must not call
  `RenderCommand` or raw GL directly.
* Add `Renderer3D::BuildFrameGraph(frameContext)` as the per-frame graph
  authoring point. `ConfigureRenderGraph` becomes temporary compatibility
  glue and eventually disappears.

**In Progress:**
- `RGBuilder` class (`OloEngine/src/OloEngine/Renderer/RGBuilder.h`) with declarative Read/Write/Create/Import/Extract/UseBlackboard API.
- `RGReadUsage` and `RGWriteUsage` enums for access-mode declarations.
- `RGSubresourceRange` struct for mip/layer/slice tracking.
- `RenderGraph::AllocateTransientTextureHandle/FramebufferHandle/BufferHandle` — public allocators for virtual resource creation.
- `RGBuilder` now captures per-pass resource declarations (read/write resource names) during setup.
- `RenderGraph::BuildFrameGraph()` now derives ordering dependencies from builder declarations (RAW/WAW ordering for registered graph-native passes that map to known graph nodes).
- `RenderGraph::BuildFrameGraph()` now records lightweight compilation stats (`passes visited`, `declared reads/writes`, `derived edges`) for frame-level debugging/inspection.
- `Renderer3D::ConfigureRenderGraph()` now registers initial graph-native setup bridges for `SSAOPass`, `GTAOPass`, and `PostProcessPass` (legacy execute path retained).
- `Renderer3D::ConfigureRenderGraph()` now also registers setup bridges for `UICompositePass` and `FinalPass`.
- `Renderer3D::ConfigureRenderGraph()` now also registers setup bridges for `OITResolvePass` and `SSSPass`.
- `Renderer3D::ConfigureRenderGraph()` now also registers setup bridges for `ParticlePass`, `WaterPass`, and `DecalPass` with OIT-aware declarations.
- `Renderer3D::ConfigureRenderGraph()` now also registers setup bridges for `ShadowPass`, `ScenePass`, `DeferredLightingPass`, `DeferredOpaqueDecalPass`, `ForwardOverlayPass`, and `FoliagePass`.
- `RenderGraph::BuildFrameGraph()` now derives declaration-based edges in `AddPass()` insertion order (instead of registration order), avoiding inverted WAW edges and cycle-prone ordering artifacts.
- `RenderGraph::BuildFrameGraph()` now rejects derived edges that would introduce a reverse-cycle against existing explicit dependencies (logs a warning and skips the edge).
- `ConfigureRenderGraph()` still keeps a small manual ordering baseline for `ValidateResourceHazards()` at configure time; runtime `BuildFrameGraph()` derivation layers additional RAW/WAW edges per frame.
- `Renderer3D::ConfigureRenderGraph()` now also registers setup bridges for `FoliagePass` with SceneColor write declaration (added [2026-04-28]).
- `Renderer3D::ConfigureRenderGraph()` now also registers setup bridge for `DeferredOpaqueDecalPass` with G-Buffer write declarations (added [2026-04-28]).
- `WaterPass → ParticlePass` explicit baseline has been removed ([2026-04-28]); both passes now declare SceneColor writes so BuildFrameGraph derives the WAW edge automatically.
- `Renderer3D::SetupFrameBlackboard()` now imports `UIComposite` framebuffer handles (FinalPass is modeled as present side-effect, not a synthetic framebuffer resource).
- `Renderer3D::SetupFrameBlackboard()` now imports both OIT MRT targets (`OITAccum`, `OITRevealage`) as graph handles.
- `Renderer3D::EndScene()` now logs `BuildFrameGraph` compile stats only when they change.
- `RGBuilder` create/import/extract paths remain lightweight stubs; full subresource-aware dependency and barrier synthesis remains deferred to later phases.
- **[FIXED 2026-04-27]** BuildFrameGraph iteration bug: was iterating `m_InsertionOrder` (which contains both legacy RenderPass and graph-native passes) and looking up all in `m_GraphPasses` (graph-native only), causing hundreds of missed-lookup warnings per frame → memory pressure → crash. Fixed by silently skipping legacy passes (no warning spam) while maintaining iteration order for correct dependency derivation. Added defensive checks in RGBuilder Read/Write operations for handle validity.
- Remaining explicit manual edges (not yet safely removable):
  - None currently remaining for baseline forward/deferred post-processing chains.
  - Reserved for future special cases (conditional pass availability, fallbacks).
  - `ShadowPass → ScenePass`: removed from the explicit baseline ([2026-04-28]).
    - ShadowPass declares writes to `ShadowMapCSM` and `ShadowMapSpot`.
    - ScenePass declares reads of both shadow maps.
    - ValidateResourceHazards derives RAW edge from declaration overlap; no explicit edge needed.
    - Focused topology/hazard suites validate correct ordering derivation.
  - `ScenePass → DeferredOpaqueDecalPass`: removed from the explicit baseline ([2026-04-28]).
    - ScenePass declares writes to full G-Buffer (Albedo, Normal, Metallic, Emissive).
    - DeferredOpaqueDecalPass declares writes to G-Buffer subset (Albedo, Normal, Emissive).
    - BuildFrameGraph derives WAW edge from overlapping G-Buffer declarations; no explicit edge needed.
    - Focused topology/hazard suites validate correct ordering derivation.
  - `ScenePass → FoliagePass`: removed from the explicit baseline ([2026-04-28]).
    - ScenePass declares writes to SceneColor (forward path) or G-Buffer (deferred path).
    - FoliagePass declares writes to SceneColor.
    - BuildFrameGraph derives WAW edge from overlapping SceneColor declarations; no explicit edge needed.
    - On deferred path, FoliagePass doesn't run, so edge is path-specific.
    - Focused topology/hazard suites validate correct ordering derivation.
  - `FoliagePass → DecalPass`: removed from the explicit baseline ([2026-04-28]).
    - Both passes now declare `SceneColor` writes (DecalPass also declares OIT buffer writes in deferred mode).
    - BuildFrameGraph derives WAW edge from declaration overlap; no explicit edge needed.
    - Focused topology/hazard suites validate correct ordering derivation.
  - `DecalPass → WaterPass`: removed from the explicit baseline ([2026-04-28]).
    - Both passes now declare `SceneColor` writes (WaterPass also declares OIT buffer writes in deferred mode).
    - BuildFrameGraph derives WAW edge from declaration overlap; no explicit edge needed.
    - Focused topology/hazard suites validate correct ordering derivation.
  - `WaterPass → ParticlePass`: removed from the explicit baseline ([2026-04-28]).
    - Both passes now declare `SceneColor` writes (ParticlePass also declares OIT buffer writes in deferred mode).
    - BuildFrameGraph derives WAW edge from declaration overlap; no explicit edge needed.
    - Focused topology/hazard suites validate correct ordering derivation.
    - Configure-time topology fixture now mirrors production: no direct Scene→AO edge is authored in `ConfigureRenderGraph`.
    - Focused topology/hazard suites remain green after removing stale fixture-only AO-edge enforcement.
  - `ScenePass → DeferredLightingPass`: removed from the explicit baseline.
    - Deferred ordering is now satisfied transitively via
      `ScenePass → DeferredOpaqueDecalPass → DeferredLightingPass`
      (validated by `RenderGraphConfigureTopology.SceneToDeferredLightingCanBeTransitiveViaDecal`).
    - Fallback behavior remains in production when the decal node is unavailable
      (`ScenePass → DeferredLightingPass` is still authored in that path).
  - `ScenePass → DeferredOpaqueDecalPass`: still required by configure-time hazard validation baseline.
    - Regression lock added in tests (`RenderGraphConfigureTopology.MissingSceneToDeferredOpaqueDecalEdgeIsFlagged`) after baseline hardening.
  - `DeferredOpaqueDecalPass → DeferredLightingPass`: removed from the explicit configure-time baseline.
    - Deferred insertion order now places `DeferredOpaqueDecalPass` before
      `DeferredLightingPass`, with runtime declaration-derived ordering from
      `BuildFrameGraph()` covering the deferred chain.
  - Fixture-only explicit `ScenePass → PostProcessPass` mirror edge has been removed; transitive ordering via `Scene → Foliage → Decal → Water → Particle → OITResolve → SSS → PostProcess` is sufficient (validated by focused topology/hazard suites).
  - `SSAO/GTAO → ParticlePass` has been removed from the explicit baseline (validated by focused topology/hazard suites).
  - `DecalPass → (SSAO|GTAO)Pass` has been removed from the explicit baseline (validated by focused topology/hazard suites).
  - `WaterPass → (SSAO|GTAO)Pass` has been removed from the explicit baseline (validated by focused topology/hazard suites).
  - These remain tracked manual edges; Phase D/E should continue moving texture-binding accesses into declarations to unlock further derivation.

Exit criterion: ✅ Phase C **COMPLETE** — the graph can be rebuilt per frame from declarative pass
setup without mutating global pass state after compile. Bridge wrappers are limited to explicitly tracked
exceptions (texture-binding WAR dependencies, OIT mode conflicts). All 17 production passes bridged;
zero runtime cycles; correct edge derivation in insertion order; per-frame compilation stable.

### Phase D — Transient pool and aliasing

**Immediately after Phase C:** First priority is extending resource declaration system to capture texture-binding accesses (SSAO/GTAO reading depth/normals via texture slots) so remaining WAR manual edges can be derived automatically. This validates the declaration/derivation pipeline before allocating transient resources. Once declarations are comprehensive, transient resource allocation becomes straightforward.

**In Progress ([2026-04-28]):**
- `RGBuilder::CreateTexture/CreateFramebuffer/CreateBuffer` now preserve `RGResourceDesc` metadata instead of dropping descriptors.
- `RenderGraph::BuildFrameGraph()` now emits a per-frame transient plan (`GetTransientPlan`) with:
  - first/last pass lifetime indices,
  - reachability gating (`willAllocate=false` for unreachable/disabled transients),
  - descriptor-compatibility alias groups + slot reuse decisions for non-overlapping lifetimes,
  - estimated byte footprint per transient candidate.
- `RenderGraph::MaterializeTransientResources()` now maps reachable, allocatable transient plan entries to `TransientPool` acquisitions and writes resolved physical IDs/refs into typed handle storage.
- Runtime opt-in toggle added (`RenderGraph::SetTransientMaterializationEnabled`): default remains off for headless/unit-test safety; `Renderer3D::SetupRenderGraph()` enables it for production rendering.
- `RenderGraph::IsTransientDescriptorAllocatable()` now rejects transient descriptors whose format cannot be lowered for the target kind (`ToImageFormat` / `ToFramebufferFormat`), preventing unsupported texture/framebuffer allocations from being planned.
- `RGResourceFormat::RG16Float` texture resources now lower to native `ImageFormat::RG16F` instead of an integer surrogate, so float RG transient textures (e.g. velocity-style resources) are planned/materialized with the correct storage format.
- Transient plan diagnostics now emit explicit descriptor skip reasons (e.g. `unsupported-framebuffer-format`, `unsupported-image-format`, `missing-dimensions`, `zero-size-buffer`) instead of a single generic descriptor failure code.
- `DumpToJson()` now exports transient alias-planning records under `"aliases"`.
- Focused regression coverage now locks the explicit transient skip-reason matrix for unsupported framebuffer/image formats, missing dimensions, and zero-size buffers, and verifies JSON debug dumps include alias records plus non-allocating transient diagnostics.

* Create a GL 4.6 transient pool keyed by descriptor compatibility. For GL,
  this can start as a pool of reusable textures/framebuffers rather than true
  memory aliasing. The API must still expose Vulkan/DX12-style alias intent.
* Compute resource lifetimes after topological ordering and allocate transient
  physical resources based on non-overlap.
* Convert the highest-value transient candidates first: post-process ping-pong
  buffers, bloom mips, SSAO raw/blur buffers, SSS blur output, OIT
  accumulation/revealage, HZB/GTAO scratch, water refraction copy, and
  selection-outline scratch buffers.
* Keep true persistent resources imported/extracted: swap-chain, shadow maps,
  IBL maps, TAA/fog histories, editor readbacks, and long-lived shader/asset
  resources.

Exit criterion: disabled or unreachable transient resources allocate nothing, 
and reachable transients report lifetimes plus alias decisions in a debug dump.

### Phase E — Compiler features: culling, barriers, and validation

* Implement backwards reachability from requested final outputs. A pass that
  produces a resource nobody reads is culled unless marked side-effecting
  (readback, external present, timestamp query, debug capture, etc.).
* Emit backend barrier records from access transitions. For GL 4.6, start with
  correct `glMemoryBarrier` placement for compute/image/storage/texture-fetch
  paths and explicit MSAA resolve / copy ordering. For future Vulkan/DX12,
  lower the same records to image layouts, buffer barriers, and queue-family
  transfers.
* Validate illegal read/write patterns, read/write of same subresource without
  an explicit feedback declaration, extraction of culled resources, imported
  resource lifetime misuse, and stale handles from prior compiles.
* Add DOT and JSON dumps of the compiled graph including pass order, resource
  descriptors, access modes, lifetimes, aliases, barriers, culled passes, and
  timings.
* Add debug validation that direct backend calls happen only through the
  graph-provided command context while a pass is executing. The goal is not to
  ban immediate-style work; it is to make it observable, labelled, and checked
  against the pass's declared resource and state contract.

**In Progress:**
- Backward reachability + culling landed (explicit-final-pass gated for compatibility):
  unreachable passes are skipped unless side-effecting.
- Side-effecting pass support is active (`RenderPass::SideEffect`, `IsSideEffecting()`).
- Initial GL barrier planning landed from declared RG access transitions:
  per-pass planned flags are computed and emitted via `RGCommandContext::MemoryBarrier()`.
- Barrier planning now records diagnostics for:
  - missing producer for read resources,
  - reads whose producers are all culled/unreachable,
  - unmapped access transitions.
- DOT dump now includes culling/barrier introspection comments and visual styling
  for culled nodes.
- Regression coverage added for:
  - storage writer→reader barrier planning,
  - render-target→shader-sample transition planning,
  - missing-producer diagnostics,
  - stale extraction handle rejection,
  - extraction-of-culled-resource rejection,
  - same-pass overlapping read/write feedback hazards,
  - imported resource lifetime misuse hazards,
  - declaration-derived deferred-core edge synthesis without manual edges,
  - declaration-derived Scene→SSAO / Scene→GTAO edge synthesis without manual edges.
- JSON dump (`DumpToJson`) now emits compiled graph state including pass order,
  resource descriptors, culling, barriers, diagnostics, computed resource
  lifetimes, per-resource access modes, transient alias plans, and per-pass
  CPU timing samples from the most recent `Execute()`.
- `DumpToJson` also emits `timingSummary` aggregates (`executedPasses`,
  `culledPasses`, `totalCpuMs`, `averageCpuMs`, `maxCpuMs`, `maxPass`) for
  quick frame-level inspection without post-processing the pass timing array.
- `DumpToJson` now includes lightweight metadata/summary blocks
  (`schemaVersion`, `timingVersion`, `hasTimings`, `frameSummary`,
  `buildStats`) so tooling can quickly validate payload compatibility and
  consume frame/build counters without scanning full arrays.
- `DumpToJson` now also emits an `executionTimeline` array (pass name +
  `culled`/`executed` flags + `cpuMs`) to make per-frame execution traces
  explicit for dashboards and offline debugging.
- `DumpToJson` timing payloads now include stable per-pass `orderIndex` in
  both `executionTimeline` and `timings`, and `timingVersion` is bumped to `2`
  so downstream tooling can diff/pass-map deterministically even when array
  order is ignored.
- `DumpToJson` now emits `timingStatsByPass` (name-keyed per-pass stats with
  `orderIndex`, `executed`, `culled`, `cpuMs`) and bumps `timingVersion` to
  `3`, enabling O(1) lookups for dashboards/analysis pipelines without array
  scans.
- `DumpToJson` now emits a deterministic `timingDigest` (`unit=cpuUs`,
  pass-order-preserving `concat`, `entryCount`) and bumps `timingVersion` to
  `4`, enabling lightweight CI snapshot comparisons without parsing full
  timing arrays.
- `DumpToJson` now emits a deterministic `resourceDigest` (`entryCount`,
  `resourceCount`, `lifetimeCount`, `accessCount`, `aliasCount`, compact
  `concat`) and bumps `schemaVersion` to `3`, enabling lightweight structural
  graph snapshot comparisons without scanning full resource/lifetime arrays.
- `DumpToJson` now emits deterministic `barrierDigest` and `graphDigest`
  payloads (compact counts + `concat`) and bumps `schemaVersion` to `4`,
  enabling low-cost CI snapshot checks for synchronization and whole-graph
  structure changes without parsing full barrier/resource arrays.

**Phase F progress (initial slice):**
- Selection-outline branch now has a dedicated graph resource
  (`SelectionOutlineColor`) and blackboard handle.
- `SelectionOutlinePass` setup declarations are graph-native (`PostProcessColor`
  read → `SelectionOutlineColor` write), and `UICompositePass` consumes the
  correct source handle based on the feature toggle.
- Final present path no longer depends on graph framebuffer piping for
  `UICompositePass -> FinalPass`; `FinalPass` is now treated as a
  side-effecting present sink with explicit ordering and direct input binding.
- Post-chain execution order now uses explicit graph dependencies
  (`Particle -> OITResolve -> SSS -> PostProcess -> [SelectionOutline] ->
  UIComposite -> Final`) while framebuffer input handoff is bound explicitly
  each frame in `Renderer3D::EndScene`, reducing production dependence on
  `ConnectPass` framebuffer piping.

**Phase F progress (slice 2 — configure-time wiring removal [2026-04-28]):**
- Removed configure-time `SetInputFramebuffer` stubs from `Renderer3D::ConfigureRenderGraph()`
  for `SSSPass`, `OITResolvePass`, and `PostProcessPass`.
- All three passes already guard `m_InputFramebuffer == nullptr` in `Execute()`, so removal
  does not introduce null-pointer risk.
- Per-frame input binding for all three passes is handled entirely by the Phase-F explicit
  handoff block in `Renderer3D::EndScene()` (added slice 1).
- `PostProcessPass::SetSceneDepthFramebuffer()` is retained at configure-time (different
  concern: depth for DOF/MotionBlur, not pipeline handoff).
- `GLStateGuardTest.RestorePolicy_RestoresCoreStateOnDtor`: removed the `polyMode[1]`
  assertion that was testing driver-specific query behavior (`GL_POLYGON_MODE` in core
  profile 4.6 is a unified front/back mode; some drivers only write one value to the
  output array). `polyMode[0]` is sufficient to verify the guard's `ApplyCore()` restore.

**Phase F progress (slice 3 — AO handoff + declaration alignment [2026-04-28]):**
- Removed configure-time AO input wiring from `Renderer3D::SetupRenderGraph()`:
  `SSAOPass::SetSceneFramebuffer(...)` and `GTAOPass::SetSceneFramebuffer(...)`.
- AO scene framebuffer handoff is now bound per-frame in `Renderer3D::EndScene()`
  (same migration pattern as the post chain).
- `SetupFrameBlackboard()` now imports `SceneNormals` from scene framebuffer
  attachment 2, making the typed blackboard field live instead of dormant.
- `SSAOPass` and `GTAOPass` setup declarations now read `SceneNormals`e
  (matching real pass sampling) instead of `GBufferNormal`, reducing
  declaration/runtime drift in hazard derivation.
- Validation run after this slice: `OloEngine-Tests` full suite remains green
  (`2206 passed, 3 skipped, 0 failed`) and `pre-commit run --all-files` passes.

**Phase F progress (slice 4 — explicit backbuffer output contract [2026-04-28]):**
- Added canonical `ResourceNames::Backbuffer` and typed blackboard handle
  `FrameBlackboard::Backbuffer`.
- `SetupFrameBlackboard()` now imports `Backbuffer` as an external framebuffer
  resource with null backing (intentional): the physical present path remains
  `RGCommandContext::BindDefaultFramebuffer()` inside `FinalRenderPass::Execute()`.
- `FinalRenderPass` now declares `Write(Backbuffer)` in addition to
  `Read(UIComposite)`, making the present sink explicit in resource contracts.
- `Renderer3D` graph-setup declaration for `FinalPass` now writes
  `board.Backbuffer`, so `BuildFrameGraph()` sees an explicit final external
  output edge in addition to the existing execution dependency.
- Validation after this slice: focused RenderGraph/hazard tests pass,
  full suite remains green (`2206 passed, 3 skipped, 0 failed`), and
  `pre-commit run --all-files` passes.

**Phase F progress (slice 5 — PostProcess typed input handles [2026-04-28]):**
- Added graph-resource resolution to `RGCommandContext` (`ResolveTexture`,
  `ResolveFramebuffer`) and bound the active `RenderGraph` during pass execution.
- Replaced raw `PostProcessRenderPass` per-frame side-channel setters for
  scene depth, AO, velocity, and CSM shadow texture IDs with typed
  `RGTextureHandle` inputs resolved during `Execute()`.
- `Renderer3D::EndScene()` now forwards `FrameBlackboard` handles
  (`SceneDepth`, `AOBuffer`, `Velocity`, `ShadowMapCSM`) to `PostProcessPass`
  instead of computing and pushing raw GL texture IDs.
- Removed the last active code references to
  `SetSceneDepthFramebuffer`, `SetSSAOTexture`,
  `SetShadowMapCSMTextureID`, and `SetVelocityTextureID`.
- `PostProcessRenderPass` resource declarations now explicitly include
  `AOBuffer` and `Velocity`, bringing declared dependencies closer to runtime
  sampling behavior.
- Validation after this slice: focused graph tests pass, full suite remains
  green (`2206 passed, 3 skipped, 0 failed`), and `pre-commit run --all-files`
  passes.

**Phase F progress (slice 6 — UI-tail typed framebuffer handles [2026-04-28]):**
- Added typed `RGFramebufferHandle` input setters to `SelectionOutlinePass`,
  `UICompositePass`, and `FinalPass`, while keeping the legacy raw framebuffer
  setter as temporary compatibility glue.
- These passes now resolve their input framebuffer through `RGCommandContext`
  during `Execute()`, so graph-visible handle wiring becomes the source of
  truth for the editor/UI tail.
- Removed the last active production-side `Renderer3D::EndScene()` raw
  `SetInputFramebuffer(...)` calls for `SelectionOutlinePass`,
  `UICompositePass`, and `FinalPass`.
- `Renderer3D::EndScene()` now wires the tail strictly from blackboard
  handles: `PostProcessColor -> [SelectionOutlineColor] -> UIComposite -> Backbuffer`.
- Validation after this slice: focused graph tests pass (`98 passed, 3 skipped,
  0 failed`), the full suite remains green (`2206 passed, 3 skipped, 0 failed`),
  and `pre-commit run --all-files` passes.

**Phase F progress (slice 7 — PostProcess typed source framebuffer handle [2026-04-28]):**
- Added typed `RGFramebufferHandle` input setter to `PostProcessPass` and
  execution-time framebuffer resolution via `RGCommandContext`.
- Introduced canonical `ResourceNames::SSSColor` + `FrameBlackboard::SSSColor`
  to model the SSS-stage output (or passthrough scene color) as a graph-visible
  framebuffer handle.
- `Renderer3D::SetupFrameBlackboard()` now imports `SSSColor` from
  `SSSPass->GetTarget()` each frame.
- Removed production-side `Renderer3D::EndScene()` raw
  `PostProcessPass->SetInputFramebuffer(...)` handoff; `PostProcessPass` is now
  wired through typed handles (`SSSColor` fallback to `SceneColor`).
- Updated graph-pass setup contracts:
  - `SSSPass` now writes `SSSColor` (when active)
  - `PostProcessPass` now reads `SSSColor` (or `SceneColor` fallback)
- Validation after this slice: focused graph tests pass (`98 passed, 3 skipped,
  0 failed`), full-suite + hooks validated in the same session.

**Phase F progress (slice 8 — AO typed scene-resource handles [2026-05-02]):**
- Added typed `SetSceneDepthHandle(RGTextureHandle)` and
  `SetSceneNormalsHandle(RGTextureHandle)` setters to both `SSAORenderPass`
  and `GTAORenderPass`.
- AO `Execute(RGCommandContext&)` now resolves the scene depth and normals via
  `context.ResolveTexture(...)` against `FrameBlackboard::SceneDepth` and
  `FrameBlackboard::SceneNormals` instead of attachment lookups on the raw
  scene framebuffer. Raw `m_SceneFramebuffer` is retained only as a fallback
  for headless / unit-test contexts where the graph is not active.
- `GTAORenderPass::DispatchGTAO` now takes the resolved normals texture ID as
  an argument instead of reaching back to the raw framebuffer member.
- `Renderer3D::EndScene()` now forwards `board.SceneDepth` and
  `board.SceneNormals` to both AO passes alongside the legacy raw framebuffer
  setter (kept as fallback during the migration).
- Validation after this slice: focused graph/AO tests pass (`102 passed,
  3 skipped, 0 failed`), full suite green (`2208 passed, 3 skipped, 0 failed`).

**Phase F progress (slice 9 — forward render-target typed handles [2026-05-02]):**
- Added typed `SetSceneColorHandle(RGFramebufferHandle)` setters and
  `m_SceneColorHandle` members to `FoliageRenderPass`, `ForwardOverlayRenderPass`,
  `WaterRenderPass`, `DecalRenderPass`, and `ParticleRenderPass`.
- Each pass's `Execute(RGCommandContext&)` now resolves `m_SceneFramebuffer`
  from the typed handle via `context.ResolveFramebuffer(m_SceneColorHandle)`
  at the top of execution; the legacy raw `m_SceneFramebuffer` member is kept
  populated by configure-time `SetSceneFramebuffer` calls and acts as a
  headless / unit-test fallback when the graph is not active. All downstream
  references to `m_SceneFramebuffer` inside Execute remain unchanged because
  the resolved Ref overwrites the member when the typed path is live.
- `Renderer3D::EndScene()` now forwards `board.SceneColor` to all five passes
  every frame, alongside the existing post-chain handoff for OIT/SSS/PostProcess.
- `SelectionOutlineRenderPass` and `DeferredLightingPass` are intentionally
  left out of this slice: the former samples entity IDs (no scene-color render
  target), the latter writes G-Buffer attachments that need their own typed
  handle slice.
- Validation after this slice: focused graph/forward-pass tests pass
  (`146 passed, 3 skipped, 0 failed`), full suite green (`2208 passed,
  3 skipped, 0 failed`).

**Phase F progress (slice 10 — Decal scene-depth typed handle [2026-05-02]):**
- Added typed `SetSceneDepthHandle(RGTextureHandle)` setter and
  `m_SceneDepthHandle` member to `DecalRenderPass`.
- Both the OIT and forward decal paths now resolve scene depth via
  `context.ResolveTexture(m_SceneDepthHandle)`, falling back to
  `m_SceneFramebuffer->GetDepthAttachmentRendererID()` for headless /
  unit-test contexts.
- `Renderer3D::EndScene()` forwards `board.SceneDepth` to `DecalPass`
  alongside the existing scene-color handoff.
- Validation: full suite green (`2208 passed, 3 skipped, 0 failed`).

**Phase F progress (slice 11 — DeferredLighting typed G-Buffer handles + schema fix [2026-05-02]):**
- **Schema fix.** `FrameBlackboard` and `ResourceNames` previously listed a
  `GBufferMetallic` slot at RT2 and labelled RT3 as `GBufferEmissive`. The
  real layout (per `GBuffer::AttachmentIndex` and the `*_GBuffer.glsl`
  fragment outputs) is `Albedo / Normal / Emissive / Velocity`; metallic is
  packed into `Albedo.a`, roughness/AO into `Normal.zw`. Renamed the slots
  to match: dropped `GBufferMetallic`, moved `GBufferEmissive` to RT2.
  Velocity (RT3) was already exposed via `FrameBlackboard::Velocity`.
- **G-Buffer import correctness.** Renderer3D's blackboard import in
  deferred mode previously sourced G-Buffer texture IDs from
  `s_Data.ScenePass->GetTarget()` — but that framebuffer is the *lit-output
  FB* in deferred mode (single colour attachment). Imports for slots 1–3
  silently returned 0. Now imports go through `ScenePass->GetGBuffer()`
  with `GBuffer::AttachmentIndex` enum slots, so the handles point at the
  actual MRT attachments.
- **Typed handle inputs.** Added `SetGBufferAlbedoHandle`,
  `SetGBufferNormalHandle`, `SetGBufferEmissiveHandle`, and
  `SetSceneDepthHandle` to `DeferredLightingPass`. The Execute path's
  resolved (single-sample) branch now reads each ID via
  `context.ResolveTexture(...)`, falling back to
  `m_GBuffer->GetColorAttachmentID(...)` when the resolve is invalid
  (headless / unit-test context). The MSAA per-sample branch still pulls
  multisample attachment IDs directly from `m_GBuffer` because those are
  not graph-imported (a follow-up could add `*_MS` companion handles).
- **Per-frame wiring.** `Renderer3D::EndScene()` forwards
  `board.SceneColor`, `board.GBufferAlbedo`, `board.GBufferNormal`,
  `board.GBufferEmissive`, and `board.SceneDepth` to the lighting pass.
  Pass setup callbacks for `SceneRenderPass`, `DeferredLightingPass`, and
  `DeferredOpaqueDecalPass` were updated to drop the obsolete
  `GBufferMetallic` read/write declarations.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`).

#### Phase F slice 12 — drop legacy `SetInputFramebuffer` safety nets

After several slices of stable graph-resolved framebuffer handles, the
per-frame raw `SetInputFramebuffer` setters in the post-chain are now
redundant. Slice 12 removes the safety nets and makes typed handles the
only load-bearing wiring for the post chain in `Renderer3D::EndScene()`.

- **Pass-side resolution priority flipped.** `PostProcessRenderPass`,
  `SelectionOutlineRenderPass`, `UICompositeRenderPass`, and
  `FinalRenderPass` now resolve their input framebuffer typed-handle-first:
  `if (m_InputFramebufferHandle.IsValid()) { auto resolved =
  context.ResolveFramebuffer(...); }` and only fall back to the raw
  `m_InputFramebuffer` member when the typed handle is invalid (headless /
  unit-test contexts that drive passes without a render graph).
- **OIT and SSS typed handles.** `OITResolveRenderPass` and `SSSRenderPass`
  gained `SetInputFramebufferHandle(RGFramebufferHandle)` setters and a
  matching member; their `Execute(RGCommandContext&)` resolves the typed
  handle at the top before the existing `m_InputFramebuffer` guard.
- **EndScene cleanup.** Renderer3D's per-frame "Phase F safety net" block
  that wired OIT/SSS/PostProcess via raw `SetInputFramebuffer` was
  replaced with a typed-handle handoff that reads `board.SceneColor` from
  the blackboard. The four paired raw `SetInputFramebuffer` calls in the
  `if (s_Data.PostProcessPass)` block (PostProcess, SelectionOutline,
  UIComposite, Final) were also deleted; only the typed
  `SetInputFramebufferHandle` setters remain.
- **Configure-time setters preserved.** Initial `SetSceneFramebuffer` /
  `SetInputFramebuffer` calls inside `ConfigureRenderGraph` and the unit
  tests still work because passes keep their raw setter as a headless
  fallback. Production runtime no longer touches them per frame.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`).

#### Phase F slice 13 — MSAA G-Buffer typed handles

Slice 11 left the MSAA per-sample shading branch in `DeferredLightingPass`
reading multisample attachment IDs directly from the raw `m_GBuffer` Ref
because the FrameBlackboard did not yet model multisample G-Buffer
attachments. Slice 13 closes that gap so the per-sample path goes through
typed handles like the resolved (single-sample) branch already does.

- **Blackboard slots.** Added `GBufferAlbedoMS`, `GBufferNormalMS`,
  `GBufferEmissiveMS`, `VelocityMS`, and `SceneDepthMS` to
  `FrameBlackboard`, plus matching `string_view` constants in
  `ResourceNames`. Multisample IDs share the same backing memory as the
  resolved attachments but bind as `sampler2DMS`, so they need their own
  graph identities.
- **Conditional import.** `Renderer3D::SetupFrameBlackboard` populates the
  `*MS` slots only when `GBuffer::GetSampleCount() > 1`. The depth
  multisample handle is also imported here (rather than alongside
  `SceneDepth`) because the multisample depth lives on the G-Buffer, not
  on the lit scene framebuffer.
- **Pass integration.** `DeferredLightingPass` gained `SetGBufferAlbedoMSHandle`,
  `SetGBufferNormalMSHandle`, `SetGBufferEmissiveMSHandle`,
  `SetVelocityMSHandle`, and `SetSceneDepthMSHandle` setters with
  matching members. Both branches of the attachment-ID lookup in
  `Execute()` now resolve typed handles first and fall back to
  `m_GBuffer->GetMSColorAttachmentID()` /
  `m_GBuffer->GetColorAttachmentID()` when the resolve returns 0
  (headless / unit-test contexts).
- **Per-frame wiring.** `Renderer3D::EndScene()` forwards the new
  handles alongside the slice-11 resolved-path handles. Invalid handles
  (when MSAA is off) trigger the raw fallback automatically.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`; PerfRegressionTest CPU-timing flake under full-suite
  contention re-confirmed pass in isolation).

#### Phase F slice 14 — OIT graph-edge gating

Originally scoped as full transient lifecycle for OIT buffers. The
underlying `OITBuffer` Ref is shared with three transparent contributors
(`ParticleRenderPass`, `WaterRenderPass`, `DecalRenderPass`) that cache it
at configure time, which makes truly transient creation a multi-pass
refactor. Slice 14 instead delivers the graph-level half of the feature:
OIT graph resources only enter the frame graph when OIT is actually
running.

- **Conditional import.** `Renderer3D::SetupFrameBlackboard` now only
  imports `board.OITAccum` / `board.OITRevealage` when
  `Settings.Path == Deferred && Settings.Deferred.OITEnabled` (and an
  `OITResolvePass` exists). When OIT is off, the handles stay invalid.
- **Edge ripple.** Transparent contributors already guard their
  `builder.Write(board.OITAccum, ...)` declarations behind
  `if (board.OITAccum.IsValid())`, so they automatically skip declaring
  OIT writes when OIT is off — no graph edges into a buffer nothing
  reads. `OITResolvePass` also self-skips via `m_Enabled`.
- **Allocator behaviour.** The underlying `OITBuffer` Ref is left allocated
  across toggles to avoid allocator churn when the user flips the OIT
  setting interactively. Truly transient `OITBuffer` ownership is left as
  follow-up work that requires reworking the contributor wiring to use a
  callback (or to look the buffer up via the blackboard) instead of
  caching a Ref at configure time.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`).

#### Phase F slice 15 — lazy `OITBuffer` allocation via provider callback

Slice 14 stopped importing OIT graph resources when OIT was disabled but
the underlying `OITBuffer` was still allocated unconditionally in
`OITResolveRenderPass::Init` (one screen-sized RGBA16F + one screen-sized
RG16F + matching depth view). Slice 15 makes the buffer truly transient
w.r.t. the OIT toggle: paths that never enable OIT pay zero GPU memory.

- **Lazy allocation.** `OITResolveRenderPass::Init` no longer creates an
  `OITBuffer`. A new `GetOrCreateOITBuffer()` accessor materialises the
  buffer from the cached `m_FramebufferSpec` on first call. The const
  `GetOITBuffer()` accessor still exists for diagnostics and returns null
  until the lazy create has fired. `ResizeFramebuffer` /
  `SetupFramebuffer` already updated `m_FramebufferSpec` and only resize
  the buffer when it exists, so deferred creation picks up the latest
  dimensions automatically.
- **Provider callbacks.** `ParticleRenderPass`, `WaterRenderPass`, and
  `DecalRenderPass` replaced the `SetOITBuffer(Ref<OITBuffer>)` setter
  (which cached a Ref at configure time) with
  `SetOITBufferProvider(std::function<Ref<OITBuffer>()>)`. Each pass
  refreshes its cached `m_OITBuffer` at the top of `Execute` only when
  `m_OITEnabled` is true; otherwise the cached Ref is reset. The
  refresh-on-execute pattern keeps the rest of the Execute logic
  unchanged while letting the provider source the lazy-allocated buffer.
- **Per-frame wiring.** `Renderer3D` installs a mutable lambda capturing a
  `Ref<OITResolveRenderPass>` and calling
  `oitResolvePassRef->GetOrCreateOITBuffer()`. `SetupFrameBlackboard`
  switched its OIT import branch from `GetOITBuffer()` to
  `GetOrCreateOITBuffer()`, so the lazy allocation happens at the same
  moment the graph imports the framebuffer when OIT is active for the
  current frame.
- **Memory-cost win.** When the user keeps OIT disabled (the default
  state), no `OITBuffer` is ever allocated. Previously the buffer was
  allocated at startup and persisted for the lifetime of the renderer.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`; PerfRegressionTest CPU-timing flake under full-suite
  contention re-confirmed pass in isolation).

#### Phase F slice 16 — extract FXAA into a standalone graph pass

The monolithic `PostProcessRenderPass` runs nine independent effects
through a shared ping-pong scaffold (bloom, DOF, motion blur, TAA,
chromatic aberration, colour grading, tone map, vignette, FXAA). To
unblock per-effect graph scheduling — and to stop the post-process node
from being one of the last black-box passes in the graph — slice 16
extracts the simplest effect in the chain (FXAA) into its own
`FXAARenderPass`. This establishes the extraction pattern (own output
framebuffer, own read/write declarations, gated blackboard import,
provider-style UBO sharing, fallback handle selection in downstream
consumers) that the remaining post-process effects will reuse.

- **New pass.** `FXAARenderPass` has its own RGBA8 output framebuffer
  (LDR, sized to the viewport), a single `PostProcess_FXAA.glsl` shader,
  and a typed `RGFramebufferHandle` input. Execute reads the input via
  `RGCommandContext::ResolveFramebuffer`, falls back to the legacy raw
  setter for headless tests, and self-skips when `Enabled` is false or
  the input is missing. The shared `PostProcessUBO` (binding 7) is
  re-bound at the top of Execute because IBL precompute and bloom-mip
  updates can transiently rebind slot 7.
- **Blackboard plumbing.** `FrameBlackboard` gains an `FXAAColor`
  `RGFramebufferHandle`; `ResourceNames::FXAAColor` mirrors it.
  `Renderer3D::SetupFrameBlackboard` only imports `FXAAColor` when
  `PostProcess.FXAAEnabled` is true, so `board.FXAAColor.IsValid()` is
  the canonical "anti-aliased post-process available" signal for
  downstream consumers.
- **PostProcess opt-out.** A `SetFXAAHandledExternally(bool)` setter
  (default false) on `PostProcessRenderPass` skips the inline FXAA
  applyEffect branch when the standalone pass is wired up. Tests that
  never construct an `FXAARenderPass` keep the original behaviour
  unchanged. Renderer3D flips the flag every frame in `EndScene`.
- **Graph topology.** `FXAAPass` is always added to the graph between
  `PostProcessPass` and `SelectionOutlinePass`/`UICompositePass` (the
  topology stays static — the pass self-skips when disabled). Execution
  edges fan in/out around it. The `RegisterGraphPass` callback only
  declares its read/write resource accesses when FXAA is enabled, so a
  disabled FXAA pass produces zero graph edges.
- **Consumer fallback.** `SelectionOutlineRenderPass` and
  `UICompositeRenderPass` (both their RegisterGraphPass callbacks and
  the `EndScene` typed-handle wiring) now prefer `board.FXAAColor` when
  it is valid and fall back to `board.PostProcessColor` otherwise. No
  changes to the passes themselves were required — the handle is the
  only thing that switches.
- **Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/FXAARenderPass.{h,cpp}` (new),
  `OloEngine/src/OloEngine/Renderer/FrameBlackboard.h`,
  `OloEngine/src/OloEngine/Renderer/ResourceHandle.h`,
  `OloEngine/src/OloEngine/Renderer/Passes/PostProcessRenderPass.{h,cpp}`,
  `OloEngine/src/OloEngine/Renderer/Renderer3D.{h,cpp}`,
  `OloEngine/src/CMakeLists.txt`.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`).
#### Phase F slice 17 — extract ChromAb / ColorGrading / ToneMap / Vignette into standalone graph passes

The same extraction pattern established in slice 16 is applied to the four
remaining inline effects that ran inside `PostProcessRenderPass`: chromatic
aberration, colour grading, tone mapping (HDR→LDR boundary), and vignette.
Each becomes a dedicated pass class that owns its own output framebuffer,
declares typed blackboard read/write edges, and self-skips when its effect is
disabled — the static graph topology stays constant.

- **Four new pass classes.** `ChromaticAberrationRenderPass` (RGBA16F),
  `ColorGradingRenderPass` (RGBA16F), `ToneMapRenderPass` (RGBA16F, always
  enabled), `VignetteRenderPass` (RGBA8, LDR after tone mapping). Each
  follows the same structure as `FXAARenderPass`: own output FB, re-bind
  `m_PostProcessUBO` at execute start, `GetTarget()` passthrough when
  disabled, `DeclareRead`/`DeclareWrite` for typed blackboard edges.
- **Sub-chain input handle resolution.** Each pass picks the most recent
  valid blackboard handle as its input (e.g. ToneMap prefers
  `ColorGradingColor`, then `ChromAbColor`, then `PostProcessColor`).
- **ToneMap exception.** Tone mapping always runs; `m_Enabled = true` by
  default and never toggled; `ToneMapColor` is imported unconditionally.
- **`PostProcessRenderPass` gated.** Four `m_*HandledExternally` flags
  (and matching setters) gate each inline effect so that when the standalone
  pass is active the inline version is skipped.
- **Static graph topology.** `ConfigureRenderGraph` inserts
  `ChromAberrationPass → ColorGradingPass → ToneMapPass → VignettePass`
  between `PostProcessPass` and `FXAAPass`; execution dependencies form a
  straight line. The downstream `SelectionOutlinePass` and `UICompositePass`
  builders prefer the most recent valid handle in the chain.
- **Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/
  {ChromaticAberration,ColorGrading,ToneMap,Vignette}RenderPass.{h,cpp}`
  (new × 8), `PostProcessRenderPass.{h,cpp}`, `Renderer3D.{h,cpp}`,
  `FrameBlackboard.h`, `ResourceHandle.h`, `OloEngine/src/CMakeLists.txt`.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`). (audit findings, [2026-05-02])

#### Phase F slice 18 — extract Fog into standalone graph pass

Volumetric fog is the outermost remaining effect inside `PostProcessRenderPass`
(executes last in the PP chain, right before the slice-17 effects), so it is
extracted first in tail-inward order.

- **`FogRenderPass`.** Two-pass implementation owning half-resolution and
  history framebuffers (`RGBA16F` half-res). Pass A ray-marches at half
  resolution, then `std::swap(m_FogHalfResFB, m_FogHistoryFB)` preserves
  the temporal history. Pass B bilaterally upsamples and composites onto a
  full-resolution `RGBA16F` output FB. Re-binds `m_PostProcessUBO` (binding 7)
  at execute start to guard against UBO stealing.
- **Scene depth and shadow CSM.** Resolved via blackboard handles
  (`SceneDepth`, `ShadowMapCSM`) with raw-ID fallback so the pass survives
  before and after those resources are imported.
- **`FogHistory` redirected.** `SetupFrameBlackboard` now imports
  `FogHistory` from `FogRenderPass::GetFogHistoryTextureID()` (falls back to
  `PostProcessPass` when `FogPass` is null).
- **`PostProcessRenderPass` gated.** `m_FogHandledExternally` flag (plus
  matching setter) skips the inline fog section when `FogRenderPass` is
  active.
- **`FogColor` blackboard handle.** New `ResourceNames::FogColor` edge
  sits between `PostProcessColor` and `ChromAbColor` in the static chain.
  Downstream `ChromAberrationPass` (and the full ternary chain through
  `UICompositePass`) prefer `FogColor` when available.
- **Execution topology.** `PostProcessPass → FogPass → ChromAberrationPass`
  replaces the former `PostProcessPass → ChromAberrationPass` edge.
- **Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/
  FogRenderPass.{h,cpp}` (new × 2), `PostProcessRenderPass.{h,cpp}`,
  `Renderer3D.{h,cpp}`, `FrameBlackboard.h`, `ResourceHandle.h`,
  `OloEngine/src/CMakeLists.txt`.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`).

#### Phase F slice 19 — extract TAA into standalone graph pass

Temporal anti-aliasing now becomes the new first extracted node after
`PostProcessRenderPass`, which means the remaining inline tail starts at
precipitation and then hands off to the extracted TAA/fog/effects chain.

- **`TAARenderPass`.** Owns a full-resolution `RGBA16F` output framebuffer,
  a persistent full-resolution `RGBA16F` history framebuffer imported next
  frame as `TAAHistory`, and its own TAA parameter UBO at binding 32.
- **Graph placement.** `PostProcessPass → TAAPass → FogPass` now replaces the
  former `PostProcessPass → FogPass` edge.
- **`TAAColor` blackboard handle.** New resource imported only when TAA is
  enabled; `FogPass` prefers it over `PostProcessColor`, and the entire
  downstream post-process chain (`ChromAberration` through `UIComposite`)
  falls back through `FogColor → TAAColor → PostProcessColor` as needed.
- **`PostProcessRenderPass` gated.** New `m_TAAHandledExternally` flag (plus
  setter) skips the legacy inline TAA section when `TAARenderPass` is active.
- **History ownership redirected.** `SetupFrameBlackboard` now imports
  `TAAHistory` from `TAARenderPass::GetTAAHistoryTextureID()` with a legacy
  fallback to `PostProcessRenderPass` for partial/headless configurations.
- **Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/
  TAARenderPass.{h,cpp}` (new × 2), `PostProcessRenderPass.{h,cpp}`,
  `Renderer3D.{h,cpp}`, `FrameBlackboard.h`, `ResourceHandle.h`,
  `OloEngine/src/CMakeLists.txt`.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`).

#### Phase F slice 20 — extract screen-space precipitation into standalone graph pass

The precipitation screen-space overlay (directional streaks + lens impacts) is
extracted from `PostProcessRenderPass` and inserted between `TAAPass` and
`FogPass`:

  `PostProcess(AO+Bloom+DOF+MB) → TAA → Precipitation → Fog → ChromAberration → …`

- **`PrecipitationRenderPass`.** Owns a full-resolution `RGBA16F` output
  framebuffer, loads `PostProcess_Precipitation.glsl`, and creates the
  `PrecipitationScreenUBO` at binding 19 (`UBO_PRECIPITATION_SCREEN`).  CPU
  state is pulled from the `ScreenSpacePrecipitation` static helpers
  (`GetStreakParams`, `GetLensImpactGPUData`) inside `Execute` — no
  additional setters needed for per-frame GPU data.
- **Graph placement.** `TAAPass → PrecipitationPass → FogPass` replaces the
  former `TAAPass → FogPass` edge.  The pass self-skips when disabled, so
  `GetTarget()` returns the input framebuffer and downstream stages fall
  back to `TAAColor` or `PostProcessColor` automatically.
- **`PrecipitationColor` blackboard handle.** New resource imported only when
  `Precipitation.Enabled && (ScreenStreaksEnabled || LensImpactsEnabled)`;
  `FogPass` prefers it over `TAAColor`, and the downstream chain
  (`ChromAberration` through `UIComposite`) extends its fallback priority as
  `FogColor → PrecipitationColor → TAAColor → PostProcessColor`.
- **`PostProcessRenderPass` gated.** New `m_PrecipitationHandledExternally`
  flag (plus setter) skips the legacy inline precipitation section at step 3.25
  when `PrecipitationRenderPass` is active.
- **Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/
  PrecipitationRenderPass.{h,cpp}` (new × 2), `PostProcessRenderPass.{h,cpp}`,
  `Renderer3D.{h,cpp}`, `FrameBlackboard.h`, `ResourceHandle.h`,
  `OloEngine/src/CMakeLists.txt`.
- Validation: build clean; full suite green (`2208 passed, 3 skipped,
  0 failed`).

#### Phase F slice 21 — extract motion blur into standalone graph pass

Motion blur is extracted from `PostProcessRenderPass` and inserted between
`PostProcessPass` and `TAAPass`:

  `PostProcess(AO+Bloom+DOF) → MotionBlur → TAA → Precipitation → Fog → ...`

- **`MotionBlurRenderPass`.** New standalone pass owning a full-resolution
  `RGBA16F` output framebuffer, loading `PostProcess_MotionBlur.glsl`, and
  resolving scene depth through a typed handle (`SceneDepth`) with raw-ID
  fallback for headless/unit-test paths.
- **`MotionBlurColor` blackboard handle.** Imported only when
  `PostProcess.MotionBlurEnabled` is true.
- **`PostProcessRenderPass` gated.** New
  `SetMotionBlurHandledExternally(bool)` +
  `m_MotionBlurHandledExternally` skip the legacy inline motion blur block
  when `MotionBlurRenderPass` is active.
- **Chain fallback updates.** `TAAPass` now prefers
  `MotionBlurColor → PostProcessColor`; downstream fallbacks (`Precipitation`
  through `UIComposite`) include `MotionBlurColor` before `PostProcessColor`.
- **Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/
  MotionBlurRenderPass.{h,cpp}` (new × 2), `PostProcessRenderPass.{h,cpp}`,
  `Renderer3D.{h,cpp}`, `FrameBlackboard.h`, `ResourceHandle.h`,
  `OloEngine/src/CMakeLists.txt`.

#### Phase F slice 22 — extract DOF into standalone graph pass

Depth of field is extracted from `PostProcessRenderPass` and inserted between
`PostProcessPass` and `MotionBlurPass`:

  `PostProcess(AO+Bloom) → DOF → MotionBlur → TAA → Precipitation → Fog → ...`

- **`DOFRenderPass`.** New standalone pass owning a full-resolution
  `RGBA16F` output framebuffer, loading `PostProcess_DOF.glsl`, and
  resolving scene depth through a typed handle (`SceneDepth`) with raw-ID
  fallback for headless/unit-test paths.
- **`DOFColor` blackboard handle.** Imported only when
  `PostProcess.DOFEnabled` is true.
- **`PostProcessRenderPass` gated.** New
  `SetDOFHandledExternally(bool)` + `m_DOFHandledExternally` skip the legacy
  inline DOF block when `DOFRenderPass` is active.
- **Chain fallback updates.** `MotionBlurPass` now prefers
  `DOFColor → PostProcessColor`; downstream fallbacks (`TAA` through
  `UIComposite`) include `DOFColor` before `PostProcessColor`.
- **Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/
  DOFRenderPass.{h,cpp}` (new × 2), `PostProcessRenderPass.{h,cpp}`,
  `Renderer3D.{h,cpp}`, `FrameBlackboard.h`, `ResourceHandle.h`,
  `OloEngine/src/CMakeLists.txt`.

#### Phase F slice 23 — extract Bloom into standalone graph pass

Bloom is extracted from `PostProcessRenderPass` and inserted between
`PostProcessPass` (which now only handles AO apply) and `DOFPass`:

  `PostProcess(AO) → Bloom → DOF → MotionBlur → TAA → Precipitation → Fog → ...`

- **`BloomRenderPass`.** New standalone pass owning a mip-chain of
  `RGBA16F` framebuffers (threshold → downsample → upsample) plus a full-
  resolution composite output. Runs the full four-shader Bloom pipeline
  (Threshold / Downsample / Upsample / Composite). TexelSize in the shared
  `PostProcessUBO` is mutated per-mip and restored on exit, identical to the
  previous inline logic.
- **`BloomColor` blackboard handle.** Imported only when
  `PostProcess.BloomEnabled` is true.
- **`PostProcessRenderPass` gated.** New
  `SetBloomHandledExternally(bool)` + `m_BloomHandledExternally` skip the
  legacy inline Bloom block when `BloomRenderPass` is active. The
  `anyEffectEnabled` guard is likewise updated.
- **Chain fallback updates.** `DOFPass` now prefers
  `BloomColor → PostProcessColor`; all downstream fallbacks (`MotionBlur`
  through `UIComposite`) include `BloomColor` between `DOFColor` and
  `PostProcessColor`.
- **Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/
  BloomRenderPass.{h,cpp}` (new × 2), `PostProcessRenderPass.{h,cpp}`,
  `Renderer3D.{h,cpp}`, `FrameBlackboard.h`, `ResourceHandle.h`,
  `OloEngine/src/CMakeLists.txt`.

#### Phase F slice 24 — extract AO apply into standalone graph pass

AO apply (SSAO or GTAO final multiplication) is extracted from `PostProcessRenderPass` 
and inserted immediately after `SSSPass`:

  `SSSColor → AOApply → AOApplyColor → PostProcess(copy) → PostProcessColor → Bloom → ...`

- **`AOApplyRenderPass`.** New standalone pass owning a full-resolution
  `RGBA16F` output framebuffer, loading `PostProcess_SSAOApply.glsl`, and
  declaring reads of scene colour, AO texture (SSAO or GTAO), and scene depth
  (for bilateral upsampling). Resolves AO texture IDs through typed handles
  (`AOBuffer`, `SceneDepth`) with raw-ID fallbacks for headless paths.
- **`AOApplyColor` blackboard handle.** Imported only when SSAO or GTAO is
  enabled. Gated by `(ActiveAOTechnique==SSAO && SSAOEnabled) || (ActiveAOTechnique==GTAO && GTAOEnabled)`.
- **`PostProcessRenderPass` gated.** New
  `SetAOApplyHandledExternally(bool)` + `m_AOApplyHandledExternally` skip the
  legacy inline AO apply block when `AOApplyRenderPass` is active. The
  `anyEffectEnabled` guard is likewise updated.
- **Chain fallback updates.** `PostProcessPass` now prefers input from
  `AOApplyColor` (when valid); all downstream chain fallbacks (`BloomPass`
  through `UIComposite`) include `AOApplyColor` between `SSSColor` and
  `PostProcessColor`.
- **Execution dependencies.** Chain changed from `SSSPass → PostProcessPass`
  to `SSSPass → AOApplyPass → PostProcessPass`, ensuring AO apply runs
  before any other post-process work.
- **Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/
  AOApplyRenderPass.{h,cpp}` (new × 2), `PostProcessRenderPass.{h,cpp}`,
  `Renderer3D.{h,cpp}`, `FrameBlackboard.h`, `ResourceHandle.h`,
  `OloEngine/src/CMakeLists.txt`.

#### Phase F slice 25 — collapse PostProcessRenderPass into a transparent passthrough

With all effects extracted, `PostProcessRenderPass` became a pure copy node
(GPU blit from AOApplyColor to its own ping-pong output) that was unconditionally
executed every frame because four effects (Vignette, ChromAb, FXAA, ColorGrading)
lacked `HandledExternally` gates in the `anyEffectEnabled` check.

- **`IsAllHandledExternally()`.**  New public method on `PostProcessRenderPass`;
  returns `true` when all twelve `m_*HandledExternally` flags are set. Called from
  `Execute` and from `Renderer3D::SetupFrameBlackboard`.
- **Execute short-circuit.** `Execute(RGCommandContext&)` returns immediately
  (no UBO bind, no shader, no blit) when `IsAllHandledExternally()`. `m_InputFramebuffer`
  is still updated so the pass remains a correct passthrough node.
- **`anyEffectEnabled` corrected.** The four effects that previously lacked
  `HandledExternally` gates (Vignette, ChromAb, FXAA, ColorGrading) are now
  properly guarded, preventing spurious no-op effect-chain traversal.
- **ToneMap early-exit fixed.** The `!anyEffectEnabled && !m_ToneMapShader`
  guard is updated to `!anyEffectEnabled && (!m_ToneMapShader || m_ToneMapHandledExternally)`
  so the pass also exits early when ToneMap is handled externally.
- **`SetupFrameBlackboard` aliasing.** `PostProcessColor` is now imported from
  the best available upstream source
  (`AOApplyPass->GetTarget() → SSSPass->GetTarget() → ScenePass->GetTarget()`)
  rather than `PostProcessPass->GetTarget()`. This ensures `BloomPass` (and all
  downstream passes) receive valid data on every frame without going through the
  PostProcessPass at all.  The fallback to `PostProcessPass->GetTarget()` is
  preserved for configurations where standalone passes are absent.
- **No GPU blit on production path.** End-to-end post-processing chain now runs
  entirely through dedicated standalone passes; the intermediate
  `PostProcessRenderPass` copy is eliminated, saving one `glBlitNamedFramebuffer`
  call per frame.
- **Files touched.** `PostProcessRenderPass.{h,cpp}`, `Renderer3D.cpp`.

The "Implementation checkpoint" subsection that previously stated
"Phases B, D–G — Not yet started" was significantly stale. Code-level audit
on `feature/rendergraph_rework` confirms:

- **Phase B — Complete.** `FrameBlackboard`, typed
  `Import{Texture,Framebuffer,Buffer,History}`, queued extraction,
  `Resolve{Texture,Framebuffer,Buffer}` are all implemented in
  `RenderGraph.{h,cpp}` and exercised by `Renderer3D::SetupFrameBlackboard`.
- **Phase C — Complete.** All 17 production passes are registered through
  `RegisterGraphPass()` with declarative setup callbacks; `BuildFrameGraph()`
  derives edges from declared reads/writes and computes `FrameBuildStats`.
- **Phase D — Implemented.** `RebuildTransientPlan()`,
  `MaterializeTransientResources()`, `TransientPool` acquire/release, and
  the `SetTransientMaterializationEnabled` runtime toggle are all live.
  Aliasing is keyed by descriptor compatibility; lifetime computation runs
  per `BuildFrameGraph()`. Imported resources are guarded against transient
  reuse.
- **Phase E — Implemented.** `ComputeReachability()` performs backward
  reachability from the explicit final pass and culls unreachable
  non-side-effecting passes. `ComputeBarrierPlan()` derives
  `MemoryBarrierFlags` from declared `RGReadUsage`/`RGWriteUsage`
  transitions, populates `m_PlannedBarriers`, and `Execute()` issues them
  via `RGCommandContext::MemoryBarrier` between passes when
  `m_RuntimeBarrierExecutionEnabled` is true. `DumpToDot` and `DumpToJson`
  (with timing/digest payloads) are implemented.
- **Phase F — In progress.** Slices 1–27 above completed. Remaining work
  covers multi-view shadow / probe materialisation (per-cascade
  and per-cube-face graph declarations), and further splitting of post-processing
  chains (DOF, motion blur, TAA, fog, etc.).

#### Phase F slice 26 — complete shadow graph declarations (point-light cubemaps)

Point-light shadow cubemaps were produced by `ShadowRenderPass` but never
declared as graph resources; the `FrameBlackboard` held a single unused
`ShadowMapPoint` handle and no per-light names existed in `ResourceNames`.

- **`ResourceNames::ShadowMapPoint0..3`.** Replaced the single `ShadowMapPoint`
  string with four per-light-slot constants (`ShadowMapPoint0`–`ShadowMapPoint3`)
  plus a convenience `std::array<std::string_view, 4> ShadowMapPoint`.
  `<array>` is now included in `ResourceHandle.h`.
- **`FrameBlackboard::ShadowMapPoint`.** Changed from `RGTextureHandle` to
  `std::array<RGTextureHandle, 4> ShadowMapPoint{}`. All four handles
  default-initialise to invalid. Size matches `UBOStructures::ShadowUBO::MAX_POINT_SHADOWS`.
- **`ShadowRenderPass::Init`.** Added `DeclareWrite(ShadowMapSpot, Texture2DArray)`
  (was missing) and a loop over `MAX_POINT_SHADOWS` adding
  `DeclareWrite(ShadowMapPoint[i], TextureCube)` for each light slot.
- **`SetupFrameBlackboard`.** Added a loop that calls `GetPointRendererID(i)` for
  each light slot and imports a `TextureCube` handle when the ID is non-zero.
- **`RegisterGraphPass "ShadowPass"`.** Added a range-for over
  `board.ShadowMapPoint` that calls `builder.Write(pointHandle, DepthStencil)`
  for each valid handle.
- **`RegisterGraphPass "ScenePass"` and `"DeferredLightingPass"`.** Added
  range-for loops that declare `builder.Read(pointHandle, ShaderSample)` for
  each active point shadow cubemap. The forward path (`ScenePass`) and deferred
  lighting path both receive correct hazard edges toward the `ShadowPass`.
- **imguizmo `src/` migration fix.** Upstream imguizmo tag 1.9 moved all source
  files to a `src/` subdirectory; `OloEngine/vendor/CMakeLists.txt` was updated
  with a `file(EXISTS ...)` probe that falls back to the root for older checkouts.
- **Files touched.** `ResourceHandle.h`, `FrameBlackboard.h`,
  `Passes/ShadowRenderPass.cpp`, `Renderer3D.cpp`, `vendor/CMakeLists.txt`.

#### Phase F slice 27 — declaration-derived edge synthesis in ValidateResourceHazards

`ValidateResourceHazards` previously required an explicit `AddExecutionDependency`
call for every producer→consumer pair, even when those passes already declared
the resource access via `DeclareRead`/`DeclareWrite`. Slice 27 makes the validator
self-sufficient: it synthesises RAW (read-after-write) ordering edges directly
from the declaration pairs, with a fixed-point transitivity pass to propagate
chains.

- **`RenderGraph::ValidateResourceHazards`.** After the explicit-edge closure
  computation, a new block iterates all (producer, consumer) pass pairs. When
  `producer.GetWrites()` and `consumer.GetReads()` share a resource name, the
  derived RAW edge `consumer depends-on producer` is inserted into the closure.
  A fixed-point while loop then propagates transitivity across all newly added
  edges, so multi-hop chains (A writes X → B reads X, B writes Y → C reads Y)
  fully close without manual intervention.
- **`Renderer3D::ConfigureRenderGraph`.** Removed two explicit ordering edges
  that were derivable from declarations:
  - `AddExecutionDependency("ShadowPass", "ScenePass")` — removed.
    `ShadowRenderPass::Init` DeclareWrites `ShadowMapCSM`; `SceneRenderPass::Init`
    DeclareReads it. The RAW edge is now fully derived.
  - `AddExecutionDependency("UICompositePass", "FinalPass")` — removed.
    `UICompositeRenderPass::Init` DeclareWrites `UIComposite`; `FinalRenderPass::Init`
    DeclareReads it. The RAW edge is now fully derived.
  All other explicit edges (ScenePass→FoliagePass, FoliagePass→DecalPass,
  WaterPass→ParticlePass, post-chain ordering) are kept because those passes
  lack sufficient pass-level `DeclareRead`/`DeclareWrite` coverage for the validator
  to derive the necessary ordering edges (WAW serialisation or cross-chain dependencies
  that go beyond simple RAW pairs).
- **Test updates.** Six tests in `ResourceHazardValidationTests.cpp` and three in
  `RenderGraphPathSwitchTests.cpp` that asserted old "RAW-without-explicit-edge =
  hazard" semantics were updated to reflect the new derived-edge behaviour.
  New tests added:
  - `DeclaredRAWPair_DerivedEdgePreventsHazardWithoutExplicitDependency` — two-pass
    RAW pair with no explicit edge.
  - `Slice27_DeclarationChainTransitivityIsHazardFree` — three-pass two-hop chain
    fully derived.
  - `ProductionShapedGraph_DerivedEdgePreventsHazardForShadow` — production-shaped
    shadow/post scenario without explicit edge.
  - `IblDeclarationsAloneSufficient`, `MissingShadowToSceneExplicitEdge_DerivedEdgeSufficient`,
    `MissingSceneToDeferredOpaqueDecalExplicitEdge_DerivedEdgeSufficient` (updated).
  - `StartupBaselineEdges_DerivedEdgesMakeGraphHazardFree` — regression test for
    the original startup hazard set.
- **Files touched.** `RenderGraph.cpp`, `Renderer3D.cpp`,
  `OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`,
  `OloEngine/tests/Rendering/RenderGraphPathSwitchTests.cpp`.

#### Phase F slice 28 — fix post-chain DeclareRead declarations; remove 17 explicit post-chain edges

Several post-process passes carried `DeclareRead(PostProcessColor)` as a legacy
placeholder even though their runtime input arrives from the *previous* pass's
output via the framebuffer ping-pong chain. The mismatch prevented the slice 27
hazard validator from deriving any ordering edges beyond the few pairs that already
matched. Slice 28 corrects the declarations and removes every explicit
`AddExecutionDependency` in the post-chain.

**Root cause.** During the per-pass Phase F migration (slices 16–24) each pass was
given its own output resource name (`BloomColor`, `DOFColor`, …), but the downstream
consumer declarations were left as `DeclareRead(PostProcessColor)` — a catch-all
that no longer matched the real producer. The slice 27 derived-edge synthesiser
therefore had no matching (write, read) pairs for those hops.

**Declaration changes (9 files):**
| Pass | Old DeclareRead | New DeclareRead |
|------|----------------|----------------|
| `PostProcessRenderPass` | `SceneColor` | `AOApplyColor` |
| `DOFRenderPass` | `PostProcessColor` | `BloomColor` |
| `MotionBlurRenderPass` | `PostProcessColor` | `DOFColor` |
| `TAARenderPass` | `PostProcessColor` | `MotionBlurColor` |
| `FogRenderPass` | `PostProcessColor` | `PrecipitationColor` |
| `ChromaticAberrationRenderPass` | `PostProcessColor` | `FogColor` |
| `FXAARenderPass` | `PostProcessColor` | `VignetteColor` |
| `SelectionOutlineRenderPass` | `PostProcessColor` | `VignetteColor` + `FXAAColor` |
| `UICompositeRenderPass` | `PostProcessColor` | `VignetteColor` + `FXAAColor` + `SelectionOutlineColor` |

`SelectionOutlineRenderPass` and `UICompositeRenderPass` declare all *possible*
inputs so the validator derives the correct ordering edge for whichever producers
are present in a given configuration. When a producer is absent its DeclareRead is
a no-op for edge derivation.

**Explicit edges removed from `Renderer3D::ConfigureRenderGraph` (17 edges):**
`AOApplyPass→PostProcessPass`, `PostProcessPass→BloomPass`, `BloomPass→DOFPass`,
`DOFPass→MotionBlurPass`, `MotionBlurPass→TAAPass`, `TAAPass→PrecipitationPass`,
`PrecipitationPass→FogPass`, `FogPass→ChromAberrationPass`,
`ChromAberrationPass→ColorGradingPass`, `ColorGradingPass→ToneMapPass`,
`ToneMapPass→VignettePass`, `VignettePass→FXAAPass`, `FXAAPass→SelectionOutlinePass`,
`SelectionOutlinePass→UICompositePass`, `FXAAPass→UICompositePass`,
`VignettePass→SelectionOutlinePass`, `VignettePass→UICompositePass`.

The remaining explicit edges — the geometry chain (Foliage, Decal, Water, AO, Particle,
OITResolve, SSS) and the `SSSPass→AOApplyPass` handoff — are kept because those passes
still lack DeclareRead/DeclareWrite coverage for WAW serialisation.

**Test updates.** `ConfiguredGraphFixture` extended with fields for AOApply and the
full post-chain (Bloom, DOF, MotionBlur, TAA, Precipitation, Fog, ChromAb,
ColorGrading, ToneMap, Vignette, FXAA). `BuildPathTopology` updated to use correct
declarations and remove post-chain explicit edges. `StartupBaselineEdges` test
updated to use `AOApplyColor` handoff. New tests added:
- `Slice28_AOApplyPassToPostProcessPassDerivedEdge` — minimal AOApplyColor pair.
- `Slice28_FullPostChainNoExplicitEdgesIsHazardFree` — 13-pass post-chain, zero
  explicit edges.
- `Slice28_FXAAAndSelectionOutlineVariantIsHazardFree` — branching end-of-chain.
- `Slice28_SelectionOutlineOnlyVariantIsHazardFree` — absent-producer benign read.

**Files touched.** `Renderer3D.cpp`,
`OloEngine/src/OloEngine/Renderer/Passes/PostProcessRenderPass.cpp`,
`DOFRenderPass.cpp`, `MotionBlurRenderPass.cpp`, `TAARenderPass.cpp`,
`FogRenderPass.cpp`, `ChromaticAberrationRenderPass.cpp`, `FXAARenderPass.cpp`,
`SelectionOutlineRenderPass.cpp`, `UICompositeRenderPass.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.


#### Phase F slice 29 — add declarations to Particle/OITResolve/SSS/AOApply; 3 explicit edges removed

Four passes in the `ParticlePass → OITResolvePass → SSSPass → AOApplyPass`
geometry tail had no `DeclareRead`/`DeclareWrite` contracts, forcing three
explicit `AddExecutionDependency` calls that the slice 27 validator could
not derive automatically. Slice 29 adds the missing declarations and removes
those edges.

**Root cause.** `OITResolveRenderPass`, `SSSRenderPass`, and `ParticleRenderPass`
were labelled "legacy path" and left declaration-free. `AOApplyRenderPass` only
declared `DeclareRead(SceneColor)` even though its actual input when SSS is
enabled is the SSS output framebuffer.

**Declaration additions (4 files):**
| Pass | New DeclareRead | New DeclareWrite |
|------|----------------|-----------------|
| `ParticleRenderPass` | — | `OITAccum`, `OITRevealage`, `SceneColor` |
| `OITResolveRenderPass` | `OITAccum`, `OITRevealage`, `SceneColor` | `SceneColor` |
| `SSSRenderPass` | `SceneColor` | `SSSColor` |
| `AOApplyRenderPass` | +`SSSColor` (alongside existing `SceneColor`) | — |

`OITResolveRenderPass` declares both `DeclareRead(SceneColor)` and
`DeclareWrite(SceneColor)` — a read-modify-write composite: it reads the OIT
accumulation/revealage buffers and composites the result into the scene
framebuffer in-place. `SSSRenderPass` always declares `DeclareWrite(SSSColor)`
(its own RGBA16F target); when disabled it is a passthrough at runtime but the
static declaration conservatively derives the SSS → AOApply ordering edge.

**Explicit edges removed from `Renderer3D::ConfigureRenderGraph` (3 edges):**
- `AddExecutionDependency("ParticlePass", "OITResolvePass")` — derived from
  `OITAccum`/`OITRevealage` write/read pair (RAW).
- `AddExecutionDependency("OITResolvePass", "SSSPass")` — derived from
  `SceneColor` write/read pair (RAW).
- `AddExecutionDependency("SSSPass", "AOApplyPass")` — derived from `SSSColor`
  write/read pair (RAW).

**Remaining explicit edges in the geometry chain** (WAW ordering, not derivable):
`ScenePass→FoliagePass`, `FoliagePass→DecalPass`, `DecalPass→WaterPass`,
`WaterPass→SSAOPass`, `SSAOPass→ParticlePass`, `WaterPass→GTAOPass`,
`GTAOPass→ParticlePass`, `WaterPass→ParticlePass` — all write `SceneColor`
(WAW) without unique per-pass output resource names. These require either
per-pass unique output resources (future slice) or continued explicit edges.

**Test updates.** `BuildPathTopology` fixture updated: `SSSPass` stub gains
`DeclareWrite(SSSColor)`; `AOApplyPass` stub gains `DeclareRead(SSSColor)`;
the three explicit `ConnectPass`/`AddExecutionDependency` calls removed.
New tests added:
- `Slice29_ParticleToOITResolveDerivedEdge` — OITAccum/OITRevealage RAW pair.
- `Slice29_OITResolveToSSSPassDerivedEdge` — SceneColor+SSSColor two-hop chain.
- `Slice29_FullGeometryTailNoExplicitEdgesIsHazardFree` — full tail from Particle
  through AOApply with only WAW edges explicit.

**Files touched.** `Renderer3D.cpp`,
`OloEngine/src/OloEngine/Renderer/Passes/ParticleRenderPass.cpp`,
`OITResolveRenderPass.cpp`, `SSSRenderPass.cpp`, `AOApplyRenderPass.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.


#### Phase F slice 30 — AOBuffer contracts for SSAO/GTAO; remove 2 AO→Particle edges

The AO branch still used explicit ordering through `ParticlePass` because
`SSAOPass` and `GTAOPass` did not declare `AOBuffer` writes in their legacy
pass contracts. Slice 30 models AO production explicitly and removes the two
ordering edges that were only present to force AO completion indirectly.

**Declaration additions (2 files):**
| Pass | New DeclareRead | New DeclareWrite |
|------|----------------|-----------------|
| `SSAORenderPass` | `SceneDepth`, `SceneNormals` | `AOBuffer` |
| `GTAORenderPass` | `SceneDepth`, `SceneNormals` | `AOBuffer` |

`AOApplyRenderPass` already declares `DeclareRead(AOBuffer)`, so
`ValidateResourceHazards` now derives AO producer→consumer edges directly:
- `SSAOPass → AOApplyPass` (RAW on `AOBuffer`)
- `GTAOPass → AOApplyPass` (RAW on `AOBuffer`)

**Explicit edge changes in `Renderer3D::ConfigureRenderGraph`:**
- Removed `AddExecutionDependency("SSAOPass", "ParticlePass")`
- Removed `AddExecutionDependency("GTAOPass", "ParticlePass")`
- Added `AddExecutionDependency("SSAOPass", "GTAOPass")` to serialize dual
  writers when both AO passes are present in the static topology and both
  declare `AOBuffer` writes (WAW ordering requirement).

`WaterPass → SSAOPass`, `WaterPass → GTAOPass`, and `WaterPass → ParticlePass`
remain explicit in this slice.

**Test updates.** `BuildPathTopology` AO stubs now declare AOBuffer writes for
the selected AO mode, and no longer model explicit AO→Particle ordering.
Added tests:
- `Slice30_SSAOToAOApplyDerivedEdge`
- `Slice30_GTAOToAOApplyDerivedEdge`
- `Slice30_DualAOWritersNeedExplicitOrdering`

**Files touched.** `Renderer3D.cpp`,
`OloEngine/src/OloEngine/Renderer/Passes/SSAORenderPass.cpp`,
`GTAORenderPass.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.


#### Phase F slice 31 — remove Water→SSAO/GTAO explicit edges; derive AO ordering from SceneDepth

After slice 30, SSAO/GTAO to AOApply ordering was derived via `AOBuffer`, but
the baseline still carried explicit `WaterPass → SSAOPass` and
`WaterPass → GTAOPass` edges. Those edges were only acting as conservative
ordering guards and are unnecessary for resource-hazard correctness.

**Explicit edge changes in `Renderer3D::ConfigureRenderGraph`:**
- Removed `AddExecutionDependency("WaterPass", "SSAOPass")`
- Removed `AddExecutionDependency("WaterPass", "GTAOPass")`

**Why this is safe.** `ScenePass` declares `DeclareWrite(SceneDepth)` and
`SSAOPass` / `GTAOPass` declare `DeclareRead(SceneDepth)`, so
`ValidateResourceHazards` now derives:
- `ScenePass → SSAOPass` (RAW on `SceneDepth`)
- `ScenePass → GTAOPass` (RAW on `SceneDepth`)

Slice 30's explicit `SSAOPass → GTAOPass` remains in place to serialize dual
`AOBuffer` writers (WAW) in the static topology where both AO passes are
registered.

**Test updates.** Topology fixture removed the explicit Water→AO edge and added:
- `Slice31_SceneToSSAODerivedEdgeWithoutWaterEdge`
- `Slice31_SceneToGTAODerivedEdgeWithoutWaterEdge`

These lock in that SceneDepth declaration pairs are sufficient without
Water→AO baseline edges.

**Files touched.** `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

---

#### Phase F slice 32 — remove geometry-chain explicit edges; derive ordering from SceneColor RMW

After slice 31 the remaining forward-path explicit edges were:
- `ScenePass → FoliagePass`
- `FoliagePass → DecalPass`
- `DecalPass → WaterPass`
- `WaterPass → ParticlePass`

All four are WAW on `SceneColor` — each pass renders in-place into the scene
framebuffer. By adding read-modify-write declarations (`DeclareRead(SceneColor)` +
`DeclareWrite(SceneColor)`) to each pass, every consecutive pair exposes a RAW
edge that `ValidateResourceHazards` can derive automatically.

**Explicit edge changes in `Renderer3D::ConfigureRenderGraph`:**
- Removed `AddExecutionDependency("ScenePass", "FoliagePass")`
- Removed `AddExecutionDependency("FoliagePass", "DecalPass")`
- Removed `AddExecutionDependency("DecalPass", "WaterPass")`
- Removed `AddExecutionDependency("WaterPass", "ParticlePass")`

**Pass declaration changes:**
- `FoliageRenderPass::Init` — `DeclareRead(SceneColor)`, `DeclareWrite(SceneColor)`
- `DecalRenderPass::Init` — `DeclareRead(SceneColor)`, `DeclareWrite(SceneColor)`
- `WaterRenderPass::Init` — `DeclareRead(SceneColor)`, `DeclareWrite(SceneColor)`
- `ParticleRenderPass::Init` — `DeclareRead(SceneColor)` added (Write already present)

**Why this is safe.** The RMW pattern gives the validator the following chain:
> ScenePass.Write → Foliage.Read (RAW) → Foliage.Write → Decal.Read (RAW)
> → Decal.Write → Water.Read (RAW) → Water.Write → Particle.Read (RAW)

All four ordering relationships are now declaration-derived. The remaining
explicit `SSAOPass → GTAOPass` edge (WAW on `AOBuffer`) is unchanged — no read
sits between the two AO writers so no RAW can be derived.

**Test updates.** `BuildPathTopology` fixture updated with SceneColor RMW
declarations for all four passes; four explicit edges removed from the fixture.
Four guardrail test comments updated. Four new positive tests added:
- `Slice32_SceneToFoliageDerivedFromSceneColor`
- `Slice32_FoliageToDecalDerivedFromSceneColor`
- `Slice32_DecalToWaterDerivedFromSceneColor`
- `Slice32_WaterToParticleDerivedFromSceneColor`

**Files touched.** `FoliageRenderPass.cpp`, `DecalRenderPass.cpp`,
`WaterRenderPass.cpp`, `ParticleRenderPass.cpp`, `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 33 — Deferred-path explicit edges derived (2025)

Remove the 5 `AddExecutionDependency` calls in `ConfigureRenderGraph`'s
`if (deferred)` block by adding `SceneColor` RMW declarations to the two
passes that were missing them:

- **`DeferredLightingPass`**: added `DeclareRead(SceneColor)` +
  `DeclareWrite(SceneColor)`. This creates a RAW edge
  `DeferredOpaqueDecalPass → DeferredLightingPass` (OpaqueDecal already wrote
  SceneColor). The former explicit `"DeferredOpaqueDecalPass"→"DeferredLightingPass"`
  and `"ScenePass"→"DeferredLightingPass"` (transitive through OpaqueDecal)
  edges are now derived.
- **`ForwardOverlayRenderPass`**: added `DeclareRead(SceneColor)` (already had
  `DeclareWrite`). Creates RAW edge `DeferredLightingPass → ForwardOverlayPass`.
  Former explicit `"DeferredLightingPass"→"ForwardOverlayPass"` edge derived.
- **`DeferredOpaqueDecalPass`** already declared `DeclareRead(SceneDepth)` +
  `DeclareWrite(SceneColor)` from prior slices — `ScenePass → DeferredOpaqueDecalPass`
  derives from the SceneDepth RAW, requiring no change here.
- The 5 explicit edges removed from `Renderer3D::ConfigureRenderGraph`.
  Only `AddExecutionDependency("SSAOPass", "GTAOPass")` remains — a true WAW
  on `AOBuffer` with no read between the two AO writers that cannot be derived
  via RAW.

Four new tests verify the derived edges:
- `Slice33_SceneToDeferredOpaqueDecalDerivedFromSceneDepth`
- `Slice33_DeferredOpaqueDecalToDeferredLightingDerivedFromSceneColor`
- `Slice33_DeferredLightingToForwardOverlayDerivedFromSceneColor`
- `Slice33_ForwardOverlayToFoliageDerivedFromSceneColor`

**Files touched.** `DeferredLightingPass.cpp`, `ForwardOverlayRenderPass.cpp`,
`Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 34 — Last explicit forward-path edge eliminated (2025)

Remove `AddExecutionDependency("SSAOPass", "GTAOPass")` — the last explicit
execution-dependency edge in the forward/deferred render path — by eliminating
the WAW at the source:

- **`Renderer3D.cpp` `ConfigureRenderGraph`**: Replace the unconditional
  `AddPass(SSAOPass)` + `AddPass(GTAOPass)` pair with a `switch` on
  `s_Data.PostProcess.ActiveAOTechnique` that adds exactly one AO pass
  (`SSAOPass` for `SSAO`, `GTAOPass` for `GTAO`, neither for `None`). With
  at most one `AOBuffer` writer in the graph there is no WAW and no explicit
  ordering edge is needed.
- **`Renderer3D.h` / `Renderer3D.cpp` `ConfigureRenderGraph`**: Add
  `ActiveGraphAOTechnique` sentinel field (mirrors the existing
  `ActiveGraphPath`). Recorded at the end of `ConfigureRenderGraph` so
  `ApplyRendererSettings` can detect technique changes.
- **`Renderer3D.cpp` `ApplyRendererSettings`**: Extend the path-change
  detection to also test `ActiveAOTechnique != ActiveGraphAOTechnique`.
  A technique switch now triggers `ConfigureRenderGraph` to rebuild the
  topology with the new single-pass set.
- **`OloEditor/PostProcessSettingsPanel.cpp`**: Call
  `Renderer3D::ApplyRendererSettings()` after the AO technique combo changes
  so the graph is reconfigured immediately in the editor.

Four new tests document the behavioral contract:
- `Slice34_DualAOWritersWithoutExplicitEdgeIsWAWHazard` (negative — proves the
  old dual-writer topology was only safe with the now-removed explicit edge)
- `Slice34_SSAOOnlyInGraphHasNoAOBufferWAW`
- `Slice34_GTAOOnlyInGraphHasNoAOBufferWAW`
- `Slice34_NoneAOTechniqueNoAOPassInGraphIsHazardFree`

**Files touched.** `Renderer3D.h`, `Renderer3D.cpp`,
`OloEditor/src/Panels/PostProcessSettingsPanel.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

**No `AddExecutionDependency` calls remain in the forward/deferred render
path.** The only surviving call is in `ResetTopologyAndRebuildAcrossPathsNoLeaks`
(test-only, for the simplified rebuild fixture).

#### Slice 35 — Self-resolving input handles: OITResolve & SSS (2025)

Remove two per-frame `SetInputFramebufferHandle` side-channel calls
(`OITResolvePass` and `SSSRenderPass`) from `EndScene()` by teaching those
passes to look up their `SceneColor` input directly from the frame blackboard
during `Execute()`.

- **`RGCommandContext.h/.cpp`**: Add `GetBlackboard() const noexcept` — returns
  `&m_RenderGraph->GetBlackboard()` when a graph is attached, `nullptr`
  otherwise (headless / test fallback guard). Declaration is pure (no inline
  graph dereference in the header) to avoid a circular-include between
  `RGCommandContext.h` and `RenderGraph.h`; implementation lives in
  `RGCommandContext.cpp` where `RenderGraph.h` is already included.
  `FrameBlackboard.h` is now included in `RGCommandContext.h` to expose the
  return type.
- **`OITResolveRenderPass.cpp` `Execute(RGCommandContext&)`**: Replace the
  `context.ResolveFramebuffer(m_InputFramebufferHandle)` call with a
  `context.GetBlackboard()` lookup on `board->SceneColor`.
- **`SSSRenderPass.cpp` `Execute(RGCommandContext&)`**: Same treatment as
  `OITResolveRenderPass`.
- **`OITResolveRenderPass.h` / `SSSRenderPass.h`**: Remove
  `SetInputFramebufferHandle()` public setter and `m_InputFramebufferHandle`
  member — both passes now self-resolve; the raw `SetInputFramebuffer()`
  remains as a headless / unit-test fallback.
- **`Renderer3D.cpp` `EndScene()`**: Remove the Phase F slice 12 block that
  called `OITResolvePass->SetInputFramebufferHandle(board.SceneColor)` and
  `SSSPass->SetInputFramebufferHandle(board.SceneColor)`. Replaced with a
  comment referencing slice 35.

Four new tests document the behavioral contracts:
- `Slice35_GetBlackboardReturnsNullptrWithoutGraph` (API unit test)
- `Slice35_GetBlackboardReturnsGraphBlackboardWhenAttached` (API unit test)
- `Slice35_OITResolveAndSSSOrderingDerivesFromDeclarations`
- `Slice35_SSSColorRAWEdgeToAOApplyDerivesFromDeclarations`

**Files touched.** `RGCommandContext.h`, `RGCommandContext.cpp`,
`OITResolveRenderPass.h`, `OITResolveRenderPass.cpp`,
`SSSRenderPass.h`, `SSSRenderPass.cpp`, `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 36 — Self-resolving SceneColor/SceneDepth for 5 forward geometry passes (2025)

Remove six per-frame side-channel setter calls from `EndScene()` by teaching
`ForwardOverlayRenderPass`, `FoliageRenderPass`, `WaterRenderPass`,
`DecalRenderPass`, and `ParticleRenderPass` to look up `SceneColor` (and
`SceneDepth` for `DecalPass`) directly from the render-graph blackboard
during `Execute()`.

- **5 pass `.cpp` files**: Replace `context.ResolveFramebuffer(m_SceneColorHandle)`
  with a `context.GetBlackboard()` lookup on `board->SceneColor`. For
  `DecalRenderPass`, also replace `context.ResolveTexture(m_SceneDepthHandle)`
  with a `board->SceneDepth` lookup. The raw `SetSceneFramebuffer()` setter is
  retained as a headless / unit-test fallback.
- **5 pass `.h` files**: Remove `SetSceneColorHandle(RGFramebufferHandle)`
  setter and `m_SceneColorHandle` member from all five headers. Remove
  `SetSceneDepthHandle(RGTextureHandle)` setter and `m_SceneDepthHandle`
  member from `DecalRenderPass.h`.
- **`Renderer3D.cpp` `EndScene()`**: Remove the 6 side-channel setter calls
  (ForwardOverlay/Foliage/Water/Decal/Particle `SetSceneColorHandle` + Decal
  `SetSceneDepthHandle`). The `fwdBoard` block is retained for the
  `DeferredLightPass` setters which are not yet self-resolving (deferred to a
  later slice due to the 10-setter MSAA companion complexity).

Four new tests document the behavioral contracts:
- `Slice36_ForwardGeometryPassesSceneColorRAWEdgeFromDeclarations`
- `Slice36_DecalPassSceneDepthRAWEdgeFromDeclarations`
- `Slice36_ParticleAndWaterAfterSceneColorWriter`
- `Slice36_FoliageAndOverlayAfterSceneColorWriter`

**Files touched.** `ForwardOverlayRenderPass.h/.cpp`, `FoliageRenderPass.h/.cpp`,
`WaterRenderPass.h/.cpp`, `DecalRenderPass.h/.cpp`, `ParticleRenderPass.h/.cpp`,
`Renderer3D.cpp`, `OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 37 — Self-resolving SceneDepth/SceneNormals for SSAO and GTAO passes (2025)

Remove four per-frame side-channel setter calls from `EndScene()` by teaching
`SSAORenderPass` and `GTAORenderPass` to look up `SceneDepth` and `SceneNormals`
directly from the render-graph blackboard during `Execute()`.

- **`SSAORenderPass.cpp` `Execute(RGCommandContext&)`**: Replace
  `context.ResolveTexture(m_SceneDepthHandle)` and
  `context.ResolveTexture(m_SceneNormalsHandle)` with a single
  `context.GetBlackboard()` lookup on `board->SceneDepth` and
  `board->SceneNormals`. Raw scene framebuffer attachment fallback retained
  for headless / unit-test contexts.
- **`GTAORenderPass.cpp` `Execute(RGCommandContext&)`**: Same treatment.
- **`SSAORenderPass.h`**: Remove `SetSceneDepthHandle(RGTextureHandle)` setter,
  `SetSceneNormalsHandle(RGTextureHandle)` setter, and both `m_Scene*Handle`
  members.
- **`GTAORenderPass.h`**: Same removals.
- **`Renderer3D.cpp` `EndScene()`**: Remove `aoBoard.SceneDepth` and
  `aoBoard.SceneNormals` setters from both the SSAO and GTAO blocks (4 calls
  total). The `aoBoard` local variables are now unused and removed.

Three new tests document the behavioral contracts:
- `Slice37_SSAOPassSelfResolvesSceneDepthAndNormals`
- `Slice37_GTAOPassSelfResolvesSceneDepthAndNormals`
- `Slice37_AOPassesAfterSceneDepthWriterNoHazards`

**Files touched.** `SSAORenderPass.h`, `SSAORenderPass.cpp`,
`GTAORenderPass.h`, `GTAORenderPass.cpp`, `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 38 — AOApplyRenderPass self-resolves SceneColor/SSSColor, AOBuffer, SceneDepth

`AOApplyRenderPass::Execute()` previously depended on three per-frame
side-channel calls in `EndScene()`:
`SetInputFramebufferHandle(SSSColor|SceneColor)`, `SetAOTextureHandle(AOBuffer)`,
and `SetSceneDepthHandle(SceneDepth)`.

All three were replaced with blackboard lookups inside `Execute()`:
- Input framebuffer: `board->SSSColor.IsValid() ? board->SSSColor : board->SceneColor` — mirrors the EndScene conditional exactly.
- AO texture: raw `m_AOTextureID` (still set via `SetAOTextureID` from SSAO/GTAO pass output) takes priority; blackboard `AOBuffer` resolves when it is 0.
- Scene depth: raw `m_SceneDepthTextureID` fallback → blackboard `SceneDepth`.

Three handle setter/member pairs removed from the class:
`SetInputFramebufferHandle` / `m_InputFramebufferHandle`,
`SetAOTextureHandle` / `m_AOTextureHandle`,
`SetSceneDepthHandle` / `m_SceneDepthHandle`.
The AOApplyPass block in `EndScene()` no longer needs a `board` reference.

Three new tests document the behavioral contracts:
- `Slice38_AOApplyPassSelfResolvesSceneColor`
- `Slice38_AOApplyPassSelfResolvesAOBufferAndSceneDepth`
- `Slice38_AOApplyPassPrefersSSSColorOverSceneColor`

**Files touched.** `AOApplyRenderPass.h`, `AOApplyRenderPass.cpp`, `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 39 — PostProcessRenderPass self-resolves five blackboard inputs

`PostProcessRenderPass::Execute()` previously depended on five per-frame
side-channel setters in `EndScene()`:
`SetInputFramebufferHandle` (conditional AOApplyColor/SSSColor/SceneColor),
`SetSceneDepthTextureHandle(SceneDepth)`,
`SetAOTextureHandle(AOBuffer)`,
`SetShadowMapCSMHandle(ShadowMapCSM)`, and
`SetVelocityTextureHandle(Velocity)`.

All five were replaced with blackboard lookups inside `Execute()`:
- Input framebuffer: `AOApplyColor` if valid, else `SSSColor`, else `SceneColor`.
- SceneDepth, AOBuffer (secondary to raw `m_AOTextureID`), ShadowMapCSM, Velocity: all via `context.ResolveTexture(board->…)`.

Five handle setter/member pairs removed from the class:
`SetInputFramebufferHandle`/`m_InputFramebufferHandle`,
`SetSceneDepthTextureHandle`/`m_SceneDepthHandle`,
`SetAOTextureHandle`/`m_AOTextureHandle`,
`SetShadowMapCSMHandle`/`m_ShadowMapCSMHandle`,
`SetVelocityTextureHandle`/`m_VelocityTextureHandle`.
The `board` reference in the PostProcessPass block of `EndScene()` is retained
(still needed for BloomPass, DOFPass, MotionBlurPass, TAAPass, etc.).

Three new tests document the behavioral contracts:
- `Slice39_PostProcessPassSelfResolvesInputChain`
- `Slice39_PostProcessPassSelfResolvesSceneDepthAndAOBuffer`
- `Slice39_PostProcessPassSelfResolvesShadowMapAndVelocity`

**Files touched.** `PostProcessRenderPass.h`, `PostProcessRenderPass.cpp`, `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 40 — DOFRenderPass, MotionBlurRenderPass, TAARenderPass self-resolve their inputs

Three passes sharing the same two-handle pattern (input framebuffer + SceneDepth) were
converted; TAARenderPass also adds a Velocity handle.

**DOFRenderPass** — removed `SetInputFramebufferHandle`/`m_InputFramebufferHandle` and
`SetSceneDepthTextureHandle`/`m_SceneDepthHandle`. `Execute()` now self-resolves:
input = `BloomColor` if valid, else `PostProcessColor`; depth from `board->SceneDepth`.

**MotionBlurRenderPass** — same pair removed. Input priority chain:
`DOFColor` → `BloomColor` → `PostProcessColor`; depth from `board->SceneDepth`.

**TAARenderPass** — same two pairs plus `SetVelocityTextureHandle`/`m_VelocityTextureHandle`
removed. Input priority chain: `MotionBlurColor` → `DOFColor` → `BloomColor` → `PostProcessColor`;
depth and velocity from blackboard (raw fallback IDs `m_SceneDepthTextureID`/`m_VelocityTextureID`
retained for headless path).

Seven handle setter calls removed from the EndScene() PostProcess block.

Three new tests document the behavioral contracts:
- `Slice40_DOFPassSelfResolvesInputAndSceneDepth`
- `Slice40_MotionBlurPassSelfResolvesInputChain`
- `Slice40_TAAPassSelfResolvesInputDepthAndVelocity`

**Files touched.** `DOFRenderPass.h`, `DOFRenderPass.cpp`,
`MotionBlurRenderPass.h`, `MotionBlurRenderPass.cpp`,
`TAARenderPass.h`, `TAARenderPass.cpp`, `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 41 — DeferredLightingPass self-resolves G-Buffer and SceneDepth handles

The most complex single-pass slice: DeferredLightingPass removes 10 handle setters
and 10 member variables. The pass runs only in Deferred rendering mode and reads
G-Buffer attachments (albedo, normal, emissive, velocity) and SceneDepth.

**DeferredLightingPass** — removed `SetSceneColorHandle`, `SetGBufferAlbedoHandle`,
`SetGBufferNormalHandle`, `SetGBufferEmissiveHandle`, `SetSceneDepthHandle`,
`SetGBufferAlbedoMSHandle`, `SetGBufferNormalMSHandle`, `SetGBufferEmissiveMSHandle`,
`SetVelocityMSHandle`, `SetSceneDepthMSHandle` (all 10 setters) and their corresponding
member variables. `Execute()` already contained self-resolving lambdas calling
`context.ResolveTexture()` with fallbacks to raw m_GBuffer accessors and m_SceneDepthTextureID.
Updated all lambdas to resolve directly from `context.GetBlackboard()` instead of
relying on stored handles.

Ten side-channel handle-setter calls removed from the EndScene() DeferredLightPass block
in Renderer3D.cpp.

Three new tests document the behavioral contracts:
- `Slice41_DeferredLightingPassSelfResolvesSceneColorAndGBuffer`
- `Slice41_DeferredLightingPassSelfResolvesSceneDepth`
- `Slice41_DeferredLightingPassSelfResolvesMSAAVariants`

**Files touched.** `DeferredLightingPass.h`, `DeferredLightingPass.cpp`, `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 42 — FogRenderPass self-resolves input framebuffer and scene depth

The fog post-process effect (volumetric fog with temporal reprojection) removes 3 handle setters
and 3 member variables. FogRenderPass reads an upstream post-process framebuffer (preference chain:
Precipitation > TAA > MotionBlur > DOF > Bloom > PostProcess) and scene depth; optionally reads
shadow map CSM for lighting volumetrics.

**FogRenderPass** — removed `SetInputFramebufferHandle`, `SetSceneDepthTextureHandle`,
`SetShadowMapCSMHandle` (all 3 setters) and their corresponding member variables. The setters
`SetEnabled()` and `SetPostProcessUBO()` are retained as they control frame-local settings.
`Execute()` now self-resolves the input framebuffer using a 6-step preference chain that checks
the blackboard for each upstream post-process target in priority order, falling back to raw
`m_InputFramebuffer` if none resolve. Scene depth and shadow map CSM are resolved with fallbacks
to raw texture IDs.

Three side-channel handle-setter calls (`SetInputFramebufferHandle`, `SetSceneDepthTextureHandle`,
`SetShadowMapCSMHandle`) removed from the EndScene() FogPass block in Renderer3D.cpp. Calls to
`SetEnabled()` and `SetPostProcessUBO()` retained.

Three new tests document the behavioral contracts:
- `Slice42_FogPassSelfResolvesInputAndSceneDepth`
- `Slice42_FogPassSelfResolvesShadowMapCSM`
- `Slice42_FogPassSelfResolvesUpstreamChain`

**Files touched.** `FogRenderPass.h`, `FogRenderPass.cpp`, `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 43 — ColorGradingRenderPass, BloomRenderPass, ChromaticAberrationRenderPass, VignetteRenderPass, ToneMapRenderPass self-resolve input framebuffer

This batch removes `SetInputFramebufferHandle` setters (1 per pass, 5 total) and corresponding
member variables from five single-handle post-process passes. All five use the same 7-step
preference chain for input framebuffer resolution: Fog > Precipitation > TAA > MotionBlur > DOF > Bloom > PostProcess.

**Affected passes:**

* **BloomRenderPass** — reads PostProcessColor exclusively (no preference chain needed).
* **ChromaticAberrationRenderPass, ColorGradingRenderPass, VignetteRenderPass, ToneMapRenderPass**
  — each uses the full 7-step preference chain; chromatic aberration is the first in the
  post-process tail, so it reads the most recent upstream result (or PostProcess if nothing
  is enabled upstream).

Five side-channel handle-setter calls (`SetInputFramebufferHandle`) removed from the EndScene()
block in Renderer3D.cpp. Setters like `SetEnabled()` and `SetPostProcessUBO()` are retained as
they control frame-local settings.

Four new tests document the behavioral contracts:
- `Slice43_BloomPassSelfResolvesPostProcessColor`
- `Slice43_ChromaticAberrationPassSelfResolvesUpstreamChain`
- `Slice43_ColorGradingPassSelfResolvesUpstreamChain`
- `Slice43_ToneMapAndVignettePassSelfResolveUpstream`

**Files touched.** `ColorGradingRenderPass.h`, `ColorGradingRenderPass.cpp`, `BloomRenderPass.h`,
`BloomRenderPass.cpp`, `ChromaticAberrationRenderPass.h`, `ChromaticAberrationRenderPass.cpp`,
`VignetteRenderPass.h`, `VignetteRenderPass.cpp`, `ToneMapRenderPass.h`, `ToneMapRenderPass.cpp`,
`Renderer3D.cpp`, `OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

#### Slice 44 — FXAARenderPass, PrecipitationRenderPass, SelectionOutlineRenderPass, UICompositeRenderPass, FinalRenderPass self-resolve input framebuffer

This final slice completes Phase F by removing `SetInputFramebufferHandle` setters (1 per pass, 5 total)
and corresponding member variables from the remaining five single-handle passes. Each pass now
self-resolves its input framebuffer from the render-graph blackboard using typed resolution with
fallbacks to raw framebuffer member variables for headless/test contexts.

**Affected passes:**

* **FXAARenderPass** — 11-step preference chain: Vignette > ToneMap > ColorGrading > ChromAb > Fog > Precip > TAA > MotionBlur > DOF > Bloom > PostProcess.
* **PrecipitationRenderPass** — 5-step preference chain: TAA > MotionBlur > DOF > Bloom > PostProcess.
* **SelectionOutlineRenderPass** — 12-step preference chain: FXAA > Vignette > ToneMap > ColorGrading > ChromAb > Fog > Precip > TAA > MotionBlur > DOF > Bloom > PostProcess.
* **UICompositeRenderPass** — 13-step preference chain: SelectionOutline > FXAA > Vignette > ToneMap > ColorGrading > ChromAb > Fog > Precip > TAA > MotionBlur > DOF > Bloom > PostProcess.
* **FinalRenderPass** — 1-step: UIComposite exclusively.

Five side-channel handle-setter calls (`SetInputFramebufferHandle`) removed from the EndScene()
block in Renderer3D.cpp. Frame-local state setters (e.g., `SetEnabled()` for SelectionOutline
and FinalPass) are retained.

Four new tests document the behavioral contracts:
- `Slice44_FXAAPassSelfResolvesUpstreamChain`
- `Slice44_PrecipitationPassSelfResolvesUpstreamChain`
- `Slice44_SelectionOutlinePassSelfResolvesUpstreamChain`
- `Slice44_UICompositeAndFinalPassSelfResolveUpstream`

**Files touched.** `FXAARenderPass.h`, `FXAARenderPass.cpp`, `PrecipitationRenderPass.h`,
`PrecipitationRenderPass.cpp`, `SelectionOutlineRenderPass.h`, `SelectionOutlineRenderPass.cpp`,
`UICompositeRenderPass.h`, `UICompositeRenderPass.cpp`, `FinalRenderPass.h`,
`FinalRenderPass.cpp`, `Renderer3D.cpp`,
`OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp`.

**Phase F complete:** All 44 slices (DeferredLighting → SelectionOutline → FinalPass) now
self-resolve handles from the render-graph blackboard with typed resolution and fallbacks.
Per-frame side-channel SetInputFramebufferHandle calls have been eliminated from EndScene()
entirely. The main renderer no longer depends on ConnectPass / SetInputFramebuffer for
production data flow.

- **Phase G — In progress (slice 12 complete).** Pass work-type classification,
  compute-hoist scheduling, graph dump metadata, async-compute batch query,
  submission-plan IR, plan-driven Execute(), explicit temporal-history contracts,
  cross-batch resource dependency surfacing, explicit resource transition records,
  unified resource lifetime records, and subresource range propagation done.
  Multi-queue scheduling remains open.

Exit criterion: resource edges derive execution order; manually-authored
ordering edges are only needed for real side effects that do not touch graph
resources. **Complete, slices 1–44.**

### Phase F — Renderer adoption

Convert the renderer from "legacy passes inside a graph" to "rendering built
with the graph" in feature-sized slices:

* **Deferred path.** Express G-Buffer MRT attachments, depth resolve, opaque
  decals, deferred lighting, forward overlay, and velocity as graph resources.
  `SceneRenderPass::EnsureGBuffer()` must move out of `Execute()` and become
  a descriptor created during setup.
* **AO and HZB.** Express SSAO, GTAO, HZB generation, denoise, and AO apply as
  graph passes. The post-process chain reads the selected AO handle from the
  blackboard rather than a per-frame texture ID setter.
* **Transparency / OIT.** Create OIT accumulation/revealage/depth resources
  only when OIT is enabled and transparent contributors exist. Water, Decal,
  and Particle passes declare writes to those handles; resolve is culled when
  no accumulation is produced.
* **Post-processing.** Split the monolithic `PostProcessRenderPass` into graph
  nodes: AO apply, bloom threshold/down/up/composite, DOF, motion blur, TAA,
  precipitation, fog raymarch/upsample, chromatic aberration, colour grading,
  tone map, vignette, and FXAA. Disabled effects simply do not add nodes.
  These nodes are expected to use the graph command context directly rather
  than the sortable draw bucket; fullscreen work does not benefit from the
  bucket's material/shader/depth sort.
* **Temporal histories.** Model TAA and fog histories as imported previous
  resources plus extracted current resources. Resize/settings changes
  invalidate history through the extraction contract.
* **Shadows and multi-view.** Materialise CSM cascades, point-light cube faces,
  probe captures, planar reflections, and future VR eyes as view/slice
  instances over the same logical pass where possible.
* **UI/final/present.** Treat the backbuffer as an imported external output;
  `FinalPass` becomes a graph present/blit pass instead of a synthetic final
  framebuffer owner.

Exit criterion: the main renderer no longer depends on `ConnectPass` /
`SetInputFramebuffer` for production data flow. Any remaining bridge wrappers
are confined to explicitly scoped editor/debug compatibility and scheduled
for removal.

### Phase G — Async compute, multi-frame graphs, and backend readiness

* Add pass flags for graphics, compute, copy, async-compute candidate,
  side-effecting, never-cull, and readback.
* Introduce queue scheduling once barriers and resource lifetimes are stable.
  Good first candidates are HZB, GTAO, denoise, and selected bloom stages.
* Add multi-frame graph support for temporal resources, GPU-driven indirect
  buffers, persistent exposure/auto-luminance, and future frame pacing.
* Keep GL 4.6 as the first backend, but ensure the compiler's intermediate
  representation can lower cleanly to Vulkan/DX12.

Exit criterion: compute-only work can overlap when dependencies allow, and
the compiled graph carries enough information to implement explicit-barrier
backends without redesigning the public API.

Each phase should add its own tier of tests to the existing 11-layer pyramid
(see [renderer-testing.md](renderer-testing.md)). Compiler-level behaviour
(culling, aliasing, barrier placement, extraction, stale-handle rejection) is
especially property-test-friendly.

---

## 4. Rendering-system adoption map

The roadmap is only complete if the final graph is actually used by the
renderer, not merely available as a framework. The current architecture should
be migrated according to this map:

| Current system | Current coupling | RDG target |
|---|---|---|
| `ShadowRenderPass` / `ShadowMap` | Pass writes externally-owned shadow textures; consumers bind IDs through globals | Import or produce shadow atlases as graph textures; cascades/faces are subresource views; scene/fog/deferred lighting read handles |
| `SceneRenderPass` | Owns scene FB and lazy G-Buffer; writes differ by rendering path | Create scene colour/depth/velocity and G-Buffer MRT handles during setup; path-specific passes consume blackboard resources |
| `DeferredLightingPass` | Reads `SceneRenderPass::GetGBuffer()` and writes into scene FB through a setter | Reads G-Buffer/depth/light-grid/shadow/IBL handles; writes scene colour handle |
| `DeferredOpaqueDecalPass` / `DecalRenderPass` | Drains decal bucket into G-Buffer via direct pointer handoff | Declares G-Buffer subresource reads/writes; transparent decal path writes OIT or scene-colour handles |
| `FoliageRenderPass`, `WaterRenderPass`, `ParticleRenderPass` | Render into scene FB or OITBuffer through setters and toggles | Declare scene-colour/depth/normals/OIT accesses; refraction copy becomes a graph copy/transient texture |
| `SSAORenderPass` / `GTAORenderPass` | Read scene FB through setters; expose texture IDs to post-process | Produce AO handle; HZB and denoise scratch are graph resources; selected AO handle is stored in blackboard |
| `OITResolveRenderPass` | Owns OITBuffer used by other passes; passthrough when disabled | OIT resources are transient graph outputs; resolve reads accum/revealage and writes scene colour; cull when unused |
| `SSSRenderPass` | Passthrough target from input when disabled | Optional graph node; disabled state forwards the input handle explicitly |
| `PostProcessRenderPass` | Monolithic internal chain with pass-owned ping-pong/mips/history | Split into graph-authored effect nodes; scratch is transient; histories are imported/extracted |
| `SelectionOutlineRenderPass` | Optional editor pass inserted by manual topology | Optional graph branch from post-process to UI; scratch buffers transient; selected IDs imported as a buffer |
| `UICompositeRenderPass` / `FinalRenderPass` | Framebuffer piping to a final blit | UI composite writes an LDR handle; final/present imports the backbuffer and performs explicit blit/present |
| Renderer UBOs / SSBOs | Updated globally before graph execution | Imported constant/storage resources or upload passes with explicit read dependencies |
| Direct `RenderCommand` / raw GL calls | Passes perform clears, fullscreen draws, compute dispatch, blits, barriers, and state restores inline | Route through `RGCommandContext`; graph scopes state, labels work, inserts barriers, validates declared resource access, and replays command buckets when needed |

---

## 5. Explicit non-goals

* **Not a wholesale rewrite in one commit.** Each migration phase lands
  behind the existing validator without breaking scenes.
* **Not a replacement for the command bucket.** Command buckets sit
  *inside* a pass and schedule individual draw calls. The RenderGraph
  schedules *passes*. The two concepts compose; neither replaces the
  other.
* **Not a requirement that every GPU call becomes a bucketed command.** The
  bucket is valuable for sortable scene geometry. Fullscreen passes, compute,
  clears, resolves, copies, readbacks, and compiler-inserted barriers should
  use the graph command context directly.
* **Not a generic DAG framework.** Keep the API rendering-specific.
  Avoid the temptation to make this a general-purpose task scheduler —
  that's `BS::thread_pool` / job-system territory.
* **Not a dependency on a specific backend.** The design must work for
  GL 4.6 today and Vulkan/DX12 tomorrow; no GL-specific types leak into
  the graph's public API. Backend-native handles and enums belong in import /
  extraction adapters and backend lowering code, not in graph setup or pass
  declarations.

### Adapter governance (enforced migration debt)

When a temporary compatibility shim or wrapper is unavoidable, it must satisfy
all of the following:

* Has a single owner (person or subsystem) and a linked tracking issue.
* Has a target removal phase/release noted in code comments and roadmap tasks.
* Emits a debug marker/label so runtime captures show where the bridge is used.
* Preserves the RDG contract (declared resources + access modes); it may not
  silently bypass hazard or barrier validation.
* Is reviewed for deletion whenever the touched subsystem receives feature work.

If any one of the above is missing, the shim is treated as non-compliant and
should be refactored before adding further RDG migration work on top.

---

## 6. Start criteria and current readiness

Any one of these is sufficient:

* Adding a new post-process chain causes the "which pass owns which
  framebuffer and who feeds whom" section of `ConfigureRenderGraph` to
  exceed another 50 lines.
* VRAM pressure becomes visible — hitting near-limit on 4 GB mobile-class
  GPUs or on dual-display high-DPI workflows.
* We ship a second backend (Vulkan). At that point automatic barrier
  insertion stops being "nice to have" and becomes structural.
* A new feature (planar reflections, multi-view VR, cascaded probes)
  demands running the same pass with different inputs and we'd otherwise
  duplicate pass instances.
* A contributor files a bug that's ultimately "pass X wrote to buffer Y
  before pass Z read it, but the validator didn't catch it because Y
  wasn't declared" — i.e. human wiring error the compiler would have
  prevented.

The current renderer already shows several of these signals: post-processing
contains an internal pass graph, AO/OIT/TAA/fog use manual per-frame texture
ID routing, deferred mode depends on lazy G-Buffer ownership inside
`SceneRenderPass`, and `ConfigureRenderGraph` carries substantial manual
edge wiring. That is enough to justify Phase 0 and Phase A work now. Later
phases should still land incrementally behind the existing validator.

---

## 7. Implementation checkpoint (this branch)

> **Branch:** `feature/rendergraph_rework` — full-plan branch; all phases
> (0 through G) land here in one pull request.

### Phase 0 — ✅ Complete

* Typed resource descriptors and handle classes (`RGResourceDesc`,
  `RGTextureHandle`, `RGBufferHandle`, `RGFramebufferHandle`) with generation
  tracking and stale-handle rejection.
* Resource registry population from imports + pass declarations, including
  producer/consumer tracking and `ResourceKindMismatch` hazard reporting.
* Imported resource registration and typed handle lookup APIs on `RenderGraph`.
* Pass submission inventory (`GetPassSubmissionInfo`) and explicit submission
  model tagging for all migrated passes.
* `RGCommandContext` integrated into graph execution, providing backend-agnostic
  pass-scope operations: viewport, clear, depth/blend/cull state, draw-buffer
  selection, texture bind, and indexed draw.
* All canonical resource names present in `ResourceNames` (scene colour, depth,
  velocity, G-Buffer attachments, AO, OIT, SSS, shadow, IBL, HZB/GTAO, TAA
  history, fog history, swap-chain/backbuffer).
* `RenderGraph::DumpToDot` for topology inspection.
* Runtime log stable: zero shader link errors, zero draw-drop spam, zero
  particle-pass GLStateGuard noise. Focused RenderGraph hazard tests: 64 passed.

### Phase A — ✅ Complete

Typed handles, resource registry, and import/stale-handle APIs are fully
implemented in `RenderGraph.h / .cpp`. All 17 production passes declare
`DeclareRead` / `DeclareWrite` with typed handles where applicable. Registry
validates descriptor compatibility, duplicate writers, and kind mismatches.

### Phase C (partial) — context migrations landed so far

### Phase C — context migrations complete

All 17 production passes are now migrated to `RGCommandContext`-driven execute
paths. Passes with private helper methods (`PostProcessRenderPass`,
`SceneRenderPass`) have their helpers using `RenderCommand` directly, while
the `Execute(RGCommandContext& context)` override routes all top-level state
through the context. Compute-only calls (`DispatchCompute`, `BindImageTexture`,
`MemoryBarrier`) remain as `RenderCommand` pending Phase C.2 expansion of
`RGCommandContext` to cover compute dispatch.

| Pass | Status |
|---|---|
| `FinalRenderPass` | ✅ Migrated |
| `UICompositeRenderPass` | ✅ Migrated |
| `SSSRenderPass` | ✅ Migrated |
| `DecalRenderPass` | ✅ Migrated |
| `DeferredLightingPass` | ✅ Migrated |
| `OITResolveRenderPass` | ✅ Migrated |
| `SelectionOutlineRenderPass` | ✅ Migrated |
| `DeferredOpaqueDecalPass` | ✅ Migrated |
| `FoliageRenderPass` | ✅ Migrated |
| `ForwardOverlayRenderPass` | ✅ Migrated |
| `GTAORenderPass` | ✅ Migrated (compute calls remain as `RenderCommand`) |
| `SSAORenderPass` | ✅ Migrated |
| `ShadowRenderPass` | ✅ Migrated |
| `PostProcessRenderPass` | ✅ Migrated (helpers use `RenderCommand`) |
| `SceneRenderPass` | ✅ Migrated (helpers use `RenderCommand`) |
| `WaterRenderPass` | ✅ Migrated |
| `ParticleRenderPass` | ✅ Migrated (compute calls remain as `RenderCommand`) |

### Phases B, D–E — ✅ Implemented (audit confirmed [2026-05-02])

* **Phase B** — `FrameBlackboard`, typed `Import{Texture,Framebuffer,Buffer,History}`,
  queued extraction, and `Resolve*` API live in `RenderGraph.{h,cpp}` and are
  populated each frame by `Renderer3D::SetupFrameBlackboard`.
* **Phase D** — `TransientPool` + `RebuildTransientPlan()` + `MaterializeTransientResources()`
  are live, with descriptor-compatibility aliasing and a runtime opt-in
  toggle (`SetTransientMaterializationEnabled`). Imported resources are
  guarded against transient reuse.
* **Phase E** — `ComputeReachability()` culls unreachable non-side-effecting
  passes from the explicit final pass; `ComputeBarrierPlan()` derives
  `MemoryBarrierFlags` from declared `RGReadUsage`/`RGWriteUsage` transitions
  and `Execute()` issues `glMemoryBarrier`s between passes when
  `m_RuntimeBarrierExecutionEnabled` is true. `DumpToDot`/`DumpToJson` (with
  schema/timing/resource/barrier/graph digests) are implemented.

### Phase F — Complete (slices 1–44)

See "Phase F progress" entries above. Slices 1–44 complete:
All 44 Phase F passes have been migrated from per-frame SetInputFramebufferHandle side-channel
calls to self-resolving blackboard lookups. PostProcessRenderPass is a transparent zero-cost node.
DeferredLightingPass, FogRenderPass, and post-process tail passes (ColorGrading, ToneMap, Vignette,
ChromaticAberration, Bloom, FXAA) all self-resolve input framebuffers using typed resolution with
fallbacks to raw member variables. Precipitation, SelectionOutline, UIComposite, and FinalPass
complete the final batch. Per-frame ConnectPass / SetInputFramebuffer calls have been eliminated
entirely. The main renderer no longer depends on side-channel handle setters for production data flow.

### Phase G — In progress (slice 12 complete)

#### Phase G Slice 1 — Pass work-type classification (PassWorkType + NeverCull + AsyncComputeCandidate)

Introduces the foundational infrastructure for Phase G queue scheduling. Three new concepts are
added to `RenderPass`:

- **`PassWorkType` enum** (`Graphics`, `Compute`, `Copy`): Classifies what hardware pipeline a
  pass uses. Default is `Graphics`. The scheduler uses this to determine which queue family the
  pass may run on.
- **`SideEffect::NeverCull = 1 << 4`**: A new SideEffect bit that marks a pass as unconditionally
  kept regardless of reachability. Uses the existing `IsSideEffecting()` predicate — no changes to
  `ComputeReachability()` required.
- **`AsyncComputeCandidate` flag**: Marks a `PassWorkType::Compute` pass as eligible for
  asynchronous compute queue overlap once multi-queue scheduling is added (Phase G.2). Informational
  in GL 4.6; will be actionable in Vulkan/DX12 backends.

**GTAORenderPass** is the only production pass currently annotated as `PassWorkType::Compute` with
`AsyncComputeCandidate = true` (HZB generation, GTAO main dispatch, and bilateral denoise are all
pure compute). All other 28 production passes default to `PassWorkType::Graphics`.

`RenderGraph::PassSubmissionInfo` now surfaces `WorkType` and `AsyncComputeCandidate` for tooling
and graph dump inspection.

Six new tests document the behavioural contracts:
- `RenderGraphPassFlags.PassWorkTypeDefaultsToGraphics`
- `RenderGraphPassFlags.ComputePassTypeRoundTrips`
- `RenderGraphPassFlags.CopyPassTypeRoundTrips`
- `RenderGraphPassFlags.AsyncComputeCandidateFlagRoundTrips`
- `RenderGraphPassFlags.NeverCullPreventsCulling`
- `RenderGraphPassFlags.PassSubmissionInfoReportsWorkTypeAndAsyncFlag`

**Files touched.** `OloEngine/src/OloEngine/Renderer/Passes/RenderPass.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/src/OloEngine/Renderer/Passes/GTAORenderPass.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

#### Phase G Slice 2 — Compute-pass scheduling hoist (HoistComputePasses)

Applies a dependency-preserving reorder to the topological execution order so
that `AsyncComputeCandidate` passes run before graphics passes in the same
dependency wavefront. Implemented as a modified Kahn’s algorithm
(`RenderGraph::HoistComputePasses()`) called at the end of
`UpdateDependencyGraph()` after the DFS-based topological sort succeeds.

**Algorithm**: Build in-degree and successor sets over `m_PassOrder`. Maintain
two queues — *computeReady* and *graphicsReady* — seeded with the zero-in-degree
passes. Each iteration drains all ready compute passes before advancing one
graphics pass. Result is committed to `m_PassOrder` only when the reordered
sequence is complete (defensive guard against any unforeseen cycle edge case).

**Effect in GL 4.6**: The GPU sees compute dispatch commands earlier in the
stream, allowing the fixed-function compute units to start while the rasterizer
pipeline is not yet saturated. No backend changes required. The flag is a
no-op for the scheduler if no `AsyncComputeCandidate` pass is present
(fast-path scan avoids Kahn’s overhead).

**Effect in future Vulkan/DX12 backends**: The ordering in `m_PassOrder` will
directly influence which passes are submitted to the async-compute queue once
multi-queue submission is added in a later Phase G slice.

Four new tests document the behavioural contracts:
- `RenderGraphComputeHoist.NoCandidatesLeavesOrderUnchanged`
- `RenderGraphComputeHoist.IndependentComputePassIsHoistedToFront`
- `RenderGraphComputeHoist.DependentComputePassRemainsAfterDependency`
- `RenderGraphComputeHoist.MultipleComputePassesAllHoisted`

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

#### Proposed next execution slices (post–G.12 punch list)

To close the remaining "legacy system" gap, execute the next slices in this
order:

1. **G.13 — Queue-lane assignment in submission plan (Graphics/Compute/Copy).**
  - Extend `GetSubmissionPlan()` to include explicit queue lane per batch.
  - Keep GL execution single-queue, but make lane assignment deterministic in IR.
  - **Done when:** JSON/DOT export shows stable lane assignment for every batch.

2. **G.14 — Cross-lane sync records (release/acquire intent).**
  - Add queue-ownership/sync metadata in `ResourceTransition` for producer→consumer
    transitions that cross lanes.
  - Keep runtime as metadata-only on GL; prepare Vulkan/DX12 lowering contract.
  - **Done when:** `DumpToJson` exports cross-lane sync records and schema tests pass.

3. **G.15 — Queue-aware scheduler validation tests.**
  - Add focused tests for legal overlap, forbidden overlap, and ordering preservation.
  - Include GTAO-style compute candidates and graphics consumers to ensure no WAR/RAW regressions.
  - **Done when:** schedule tests pass and hazard validator remains green.

4. **H.1 — Isolate remaining execute-path bridges.**
  - Inventory raw fallback usage (`m_InputFramebuffer`, cached raw texture IDs, legacy setters)
    and keep them only for headless/test compatibility, not production frame flow.
  - Add debug assertions/telemetry when production path falls back unexpectedly.
  - **Done when:** production frame runs show zero unexpected fallback activations.

5. **H.2 — Multi-view materialisation for shadows/probes.**
  - Model CSM cascades and point-shadow cube faces as view/slice instances rather than
    ad-hoc pass-internal loops.
  - Reuse `RGSubresourceRange` end-to-end for per-view transitions and lifetimes.
  - **Done when:** per-view resources/transitions appear as first-class graph records.

6. **H.3 — Retire `ConfigureRenderGraph` compatibility scaffolding.**
  - Move remaining topology glue from imperative pass wiring to pure graph setup + blackboard contracts.
  - Keep only clearly-scoped editor/debug bridges.
  - **Done when:** production renderer graph build is callback-driven without legacy wiring blocks.

This sequence keeps risk low: finish queue/synchronization IR first (G.13–G.15),
then remove bridge debt and complete renderer adoption (H.1–H.3).

#### Phase G Slice 3 — Surface PassWorkType and AsyncComputeCandidate in graph dumps

Extends `DumpToDot` and `DumpToJson` so that the compiled graph carries enough
metadata for tooling and future explicit-barrier backends to inspect work-type
classification without redesigning the public API.

**DumpToDot** additions:
- Compute passes: `fillcolor="#fff3cd"` (amber) — visually distinct from the default graphics blue
- Copy passes: `fillcolor="#cff4fc"` (teal)
- `AsyncComputeCandidate` passes: node label prefixed with `[async] `
- Header comment updated with colour legend

**DumpToJson** additions (schema version bumped 4 → 5):
- `passFlags` top-level array: per-pass `{ "pass", "workType", "asyncComputeCandidate" }`
- `frameSummary` extended: `"computePassCount"` and `"asyncComputeCandidateCount"`
- `executionTimeline` entries extended: `"workType"` and `"asyncComputeCandidate"` per entry
- `timingStatsByPass` entries extended: `"workType"` and `"asyncComputeCandidate"` per entry
- `graphDigest` concat extended: `;compute=N;asyncCandidates=M`

Three new test suites and one updated test document the behavioural contracts:
- `RenderGraphDumpJson.PassFlagsAreSurfacedInDump`
- `RenderGraphDumpJson.GraphDigestContainsComputeCountsForAllGraphicsPasses`
- `RenderGraphDumpDot.ComputePassColoredDifferentlyToGraphics`
- Updated: `RenderGraph.DumpToJsonWritesCompiledGraphDetails` (schema v5 assertions)

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

Multi-queue scheduling, multi-frame graph contracts, and explicit-barrier
backend lowering remain open.

#### Phase G Slice 4 — Async-compute batch query (`GetAsyncComputeBatches()`)

Introduces a first-class query API that partitions the hoisted execution order
into `AsyncComputeBatch` records, each carrying the fence-sync metadata needed
by explicit-barrier backends:

- **`ComputePasses`** — the batch members, in execution order.
- **`WaitPasses`** — non-batch passes this batch must wait for (predecessor
  graphics work that the compute queue cannot start ahead of).
- **`SignalPasses`** — non-batch passes that must wait for this batch (successor
  graphics work blocked on compute output).

**Backend mapping:**
| Backend | WaitPasses | SignalPasses |
|---|---|---|
| GL 4.6 | `glClientWaitSync` / `glWaitSync` before first dispatch | `glFenceSync` + `glClientWaitSync` before dependent draw |
| Vulkan | semaphore `waitSemaphores` on compute `VkSubmitInfo` | semaphore `signalSemaphores`; graphics `VkSubmitInfo` waits |
| DX12 | `ID3D12CommandQueue::Wait(fence)` on compute queue | compute queue `Signal(fence)`; graphics queue `Wait` |

**Implementation.** `RenderGraph::GetAsyncComputeBatches()` const method — reads
the already-hoisted `m_PassOrder`, builds a successor map from `m_Dependencies`,
and walks the order to collect contiguous `AsyncComputeCandidate` runs.  Returns
an empty vector (fast-path) when no candidate exists.

Five tests document the behavioural contracts:
- `RenderGraphAsyncBatch.NoCandidatesReturnsEmptyBatches`
- `RenderGraphAsyncBatch.SingleComputePassFormsBatchWithCorrectSignalPass`
- `RenderGraphAsyncBatch.IndependentComputePassHasEmptyWaitAndSignalLists`
- `RenderGraphAsyncBatch.ConsecutiveComputePassesGroupedInOneBatch`
- `RenderGraphAsyncBatch.ComputeBatchWaitsForGraphicsPrerequisite`

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

Multi-queue scheduling, multi-frame graph contracts, and explicit-barrier
backend lowering remain open.

#### Phase G Slice 5 — Submission-plan IR (`GetSubmissionPlan()`)

Introduces a backend-portable, linearised command stream that a Vulkan/DX12/GL
renderer can execute without re-reading graph topology or barrier tables.

**`SubmissionCommand` kind enum:**
| Kind | Meaning |
|---|---|
| `Pass` | Execute the named pass on the queue indicated by `WorkType` |
| `MemoryBarrier` | Insert a pipeline/memory barrier with the given `Barriers` flags |
| `BatchBegin` | Open async-compute submission slot `BatchIndex`; backend inserts queue-wait |
| `BatchEnd` | Close slot `BatchIndex`; backend inserts queue-signal / fence |

**Backend lowering:**
| Backend | BatchEnd | BatchBegin | MemoryBarrier |
|---|---|---|---|
| GL 4.6 | `glFenceSync` | `glClientWaitSync` | `glMemoryBarrier(flags)` |
| Vulkan | compute `VkSubmitInfo` signal semaphore | graphics `VkSubmitInfo` wait semaphore | `vkCmdPipelineBarrier` |
| DX12 | compute queue `Signal(fence)` | compute queue `Wait(fence)` | `ResourceBarrier` |

**Implementation.** `RenderGraph::GetSubmissionPlan()` const method — calls
`GetAsyncComputeBatches()` for batch boundaries, merges with
`m_PlannedBarriers` (Phase E), and emits a single `std::vector<SubmissionCommand>`
in hoisted execution order.

Five tests document the behavioural contracts:
- `RenderGraphSubmissionPlan.PureGraphicsGraphHasOnlyPassCommands`
- `RenderGraphSubmissionPlan.ComputePassWrappedInBatchBeginEnd`
- `RenderGraphSubmissionPlan.PassCommandsCarryCorrectWorkType`
- `RenderGraphSubmissionPlan.MultipleComputePassesSameIndexGetOneBatchPair`
- `RenderGraphSubmissionPlan.PlanPreservesHoistedExecutionOrder`

#### Phase G Slice 6 — Plan-driven `Execute()` + GL debug-group batch boundaries

Wires `Execute()` to consume `m_CachedSubmissionPlan` (the `SubmissionCommand` IR
from Slice 5) as its execution backbone, replacing the former ad-hoc pass-loop
and inline barrier-flag lookups.  The plan is cached once per frame after
`ComputeBarrierPlan()` runs (both in the dirty-rebuild path of `Execute()` and in
`BuildFrameGraph()`), so the hot path is a pure sequential scan over a
pre-computed vector — no per-frame hash lookups.

**Command dispatch:**
| Kind | Action in Execute() |
|---|---|
| `Pass` | Look up pass by name, call `pass->Execute(context)`, record timing + fire post-pass hook |
| `MemoryBarrier` | `commandContext.MemoryBarrier(flags)` gated on `m_RuntimeBarrierExecutionEnabled` |
| `BatchBegin` | `commandContext.BeginAsyncBatch(index)` + fire `m_BatchEventHook(index, true)` |
| `BatchEnd` | `commandContext.EndAsyncBatch(index)` + fire `m_BatchEventHook(index, false)` |

**GL 4.6 implementation of `BeginAsyncBatch` / `EndAsyncBatch`.**  The GL command
stream is inherently serialised — no true async queue overlap is possible in a
single context.  The methods therefore insert `glPushDebugGroup` / `glPopDebugGroup`
(KHR_debug extension, guarded by `GLAD_GL_KHR_debug`) so the batch region is
visible as a labelled group in RenderDoc and Nsight captures.  The guard also
makes the calls safe in headless / unit-test contexts where glad has not been
initialised.

**`BatchEventCallback` hook.** Added to `RenderGraph` alongside the existing
`PostPassHook`.  Tests and profiling tooling can install a callback that fires on
every `BatchBegin` / `BatchEnd` boundary to verify batch placement without needing
a real GL context.

Five tests document the behavioural contracts:
- `RenderGraphExecutePlanDriven.PureGraphicsGraphPassesExecuteInOrder`
- `RenderGraphExecutePlanDriven.CulledPassIsSkippedInPlanDrivenExecution`
- `RenderGraphExecutePlanDriven.BatchEventHookFiresBeginAndEndForAsyncComputePass`
- `RenderGraphExecutePlanDriven.BatchEventHookBatchIndexIsZeroForFirstBatch`
- `RenderGraphExecutePlanDriven.PostPassHookStillFiresForEachPass`

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/src/OloEngine/Renderer/RGCommandContext.h`,
`OloEngine/src/OloEngine/Renderer/RGCommandContext.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

#### Phase G Slice 7 — Explicit temporal-history contracts (`ExtractHistoryTexture()`)

Promotes temporal-resource write-back from an ad-hoc extraction callback into a
first-class graph contract. The frame graph already supported importing prior-frame
history via `ImportHistory(...)`; this slice adds the matching *next-frame output*
side so tooling and future backends can see which current-frame resource is intended
to persist as which history input.

**New API surface:**
- `RenderGraph::ExtractHistoryTexture(historyResource, sourceHandle, callback)` — queues
  a write-back of `sourceHandle` into the named imported history resource.
- `RenderGraph::TemporalHistoryContract` — records `{ HistoryResource, SourceResource,
  HistoryImported, SourceReachable }` for the current frame.
- `GetTemporalHistoryContracts()` — exposes the per-frame contract list for tests,
  tooling, and future backend planning.

**Validation rules:**
- Source-handle staleness reuses the existing stale-extraction diagnostic path.
- Source resources produced only by culled passes reuse
  `ExtractionOfCulledResource` and skip the callback.
- A contract whose `historyResource` was not imported via `ImportHistory(...)`
  now emits `InvalidHistoryContract` and skips the callback.

**DumpToJson additions (schema 5 → 6):**
- `frameSummary.historyResourceCount`
- `frameSummary.temporalHistoryContractCount`
- `resources[].isHistory`
- top-level `temporalHistoryContracts` array
- `graphDigest` extended with `;histories=N;historyContracts=M`

This keeps the implementation intentionally lightweight: GL 4.6 still executes
serially, but the compiler now carries enough metadata to describe *which* resource
crosses the frame boundary and whether the contract is valid.

Three tests document the behavioural contracts:
- `RenderGraphTemporalHistoryContracts.ExtractHistoryTextureRecordsContractAndInvokesCallback`
- `RenderGraphTemporalHistoryContracts.InvalidHistoryContractReportsDiagnosticAndSkipsCallback`
- `RenderGraphTemporalHistoryContracts.DumpToJsonIncludesHistoryResourcesAndContracts`

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

Multi-queue scheduling and explicit-barrier backend lowering remain open.

#### Phase G Slice 9 — Self-contained submission-plan batch metadata + JSON export

Makes `GetSubmissionPlan()` fully self-contained for backend submission logic by
embedding batch-boundary sync/resource payloads directly in `SubmissionCommand`
instead of requiring a side query to `GetAsyncComputeBatches()`.

**`SubmissionCommand` additions:**
- `WaitPasses` (BatchBegin)
- `SignalPasses` (BatchEnd)
- `InputResources` (BatchBegin)
- `OutputResources` (BatchEnd)

**Semantics:**
- `BatchBegin` now carries the queue-wait contract (`WaitPasses`) and the
  incoming resource hazards (`InputResources`) that must be satisfied before
  async compute starts.
- `BatchEnd` now carries the queue-signal contract (`SignalPasses`) and the
  outgoing resource hazards (`OutputResources`) that downstream graphics work
  must wait for.

**Implementation detail (`GetSubmissionPlan()`):**
1. Build `batches = GetAsyncComputeBatches()`.
2. Map `batchIndex -> AsyncComputeBatch`.
3. On `BatchBegin`, copy `WaitPasses` + `InputResources` into the command.
4. On `BatchEnd`, copy `SignalPasses` + `OutputResources` into the command.
5. Preserve existing pass/barrier emission and hoisted pass order.

**DumpToJson additions (schema 7 → 8):**
- `frameSummary.submissionCommandCount`
- top-level `submissionPlan` array with command records:
  - `kind=Pass`: `{ pass, workType }`
  - `kind=MemoryBarrier`: `{ flags }`
  - `kind=BatchBegin|BatchEnd`: `{ batchIndex, waitPasses, signalPasses, inputResources, outputResources }`
- `graphDigest` extended with `;submissionCommands=N`

Three tests document the behavioural contracts:
- `RenderGraphSubmissionPlan.BatchBeginCarriesWaitAndInputResources`
- `RenderGraphSubmissionPlan.BatchEndCarriesSignalAndOutputResources`
- `RenderGraphSubmissionPlan.DumpToJsonIncludesSubmissionPlan`

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

Multi-queue scheduling and explicit-barrier backend lowering remain open.

#### Phase G Slice 8 — Cross-batch resource dependency surfacing (`InputResources` / `OutputResources`)

Extends async-compute batch metadata so explicit-barrier backends can see not only
*which passes* are synchronisation boundaries (`WaitPasses` / `SignalPasses`), but
also *which resources* cross those boundaries.

**New API surface:**
- `RenderGraph::BatchResourceDependency { ResourceName, ExternalPass }`
- `RenderGraph::AsyncComputeBatch::InputResources`
- `RenderGraph::AsyncComputeBatch::OutputResources`

**Semantics:**
- `InputResources`: resources read by passes in the compute batch whose last writer
  before the batch is an external (non-batch) pass. These are the data hazards the
  batch must wait on before dispatch begins.
- `OutputResources`: resources written by passes in the compute batch whose first
  reader after the batch is an external pass. These are the hazards that downstream
  graphics work must wait on after compute finishes.

**Implementation detail (`GetAsyncComputeBatches()`):**
1. Build contiguous compute batches (existing Slice 4 logic).
2. Build per-batch pass-index range and gather read/write resource sets from
   `m_PassAccessDeclarations`.
3. Scan pre-batch passes to find last external writers for batch-read resources
   (`InputResources`).
4. Scan post-batch passes to find first external readers for batch-written resources
   (`OutputResources`).
5. Sort dependency vectors by resource name for deterministic output.

**DumpToJson additions (schema 6 → 7):**
- `frameSummary.asyncBatchCount`
- `frameSummary.batchInputResourceCount`
- `frameSummary.batchOutputResourceCount`
- top-level `asyncBatches` array with:
  - `computePasses`, `waitPasses`, `signalPasses`
  - `inputResources[]` entries `{ resource, externalPass }`
  - `outputResources[]` entries `{ resource, externalPass }`
- `graphDigest` extended with:
  - `;batches=N`
  - `;batchInputResources=M`
  - `;batchOutputResources=K`

Five tests document the behavioural contracts:
- `RenderGraphAsyncBatchResources.NoBatchResourceDepsWhenNoAccessDeclarations`
- `RenderGraphAsyncBatchResources.SingleResourceFlowsIntoBatch`
- `RenderGraphAsyncBatchResources.BatchOutputFlowsToGraphicsPass`
- `RenderGraphAsyncBatchResources.IndependentBatchHasNoCrossBoundaryResources`
- `RenderGraphAsyncBatchResources.DumpToJsonIncludesBatchResourceDeps`

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

Multi-queue scheduling and explicit-barrier backend lowering remain open.

#### Phase G Slice 10 — Explicit resource transition records (`GetResourceTransitions()`)

Exposes per-resource producer→consumer transition records computed from the
existing `m_PlannedBarriers` and `m_PassAccessDeclarations`, giving backend
drivers and debugging tools a precise, named view of every GPU resource
state change without having to re-derive it from raw barrier flags.

**New API surface:**
- `RenderGraph::ResourceTransition`
  - `ResourceName` — logical resource name
  - `ProducerPass` — last writing pass before the barrier (or `"external"` for
    imported resources that have no in-graph writer)
  - `ConsumerPass` — the pass before which the barrier was inserted
  - `FromUsage` (`RGWriteUsage`) — the write usage in the producer pass
  - `ToUsage` (`RGReadUsage`) — the read usage in the consumer pass
  - `Flags` (`MemoryBarrierFlags`) — the already-computed barrier flags
- `[[nodiscard]] std::vector<ResourceTransition> GetResourceTransitions() const`

**Semantics:**
- Returns one record per entry in `m_PlannedBarriers`.
- `ConsumerPass` is the value of `PlannedBarrier::BeforePass`.
- `ToUsage` is resolved from `m_PassAccessDeclarations[BeforePass]` — the
  first matching read declaration for the barrier's resource.
- `ProducerPass` / `FromUsage` are found by walking the topological order in
  reverse from the consumer to find the last pass that declared a write on
  the same resource. If no such pass exists, `ProducerPass = "external"` and
  `FromUsage` defaults to `RGWriteUsage::RenderTarget`.

**DumpToJson additions (schema 8 → 9):**
- `frameSummary.resourceTransitionCount`
- top-level `resourceTransitions` array (before `timings`):
  - entries `{ resource, producerPass, consumerPass, fromUsage, toUsage, flags }`
  - `fromUsage` / `toUsage` are human-readable strings (e.g. `"RenderTarget"`,
    `"ShaderSample"`)
- `graphDigest` extended with `;transitions=N`

Five tests document the behavioural contracts:
- `RenderGraphResourceTransitions.NoTransitionsWhenNoBarriersPlanned`
- `RenderGraphResourceTransitions.SingleTransitionCapturesProducerAndConsumer`
- `RenderGraphResourceTransitions.ProducerIsLastWriterBeforeConsumer`
- `RenderGraphResourceTransitions.ExternalImportHasNoProducerPass`
- `RenderGraphResourceTransitions.DumpToJsonIncludesResourceTransitions`

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

#### Phase G Slice 11 — Unified resource lifetime records (`GetResourceLifetimes()`)

Exposes a first-write / last-read lifetime record for **every registered resource**
(transient, imported, history, and extracted) so that an explicit-barrier backend
(Vulkan / DX12) can schedule image-layout transitions and memory acquire/release
without additional bookkeeping beyond what the graph already tracks.

**New API surface:**
- `RenderGraph::ResourceLifetime`
  - `ResourceName` — logical resource name
  - `IsImported` — entered via `ImportTexture` / `ImportFramebuffer` / `ImportBuffer`
  - `IsExtracted` — has a pending `TextureExtract` or `FramebufferExtract` callback
  - `IsHistory` — temporal-history resource (`ImportHistory` / `ExtractHistoryTexture`)
  - `IsTransient` — allocated by the transient pool (`WillAllocate == true`)
  - `FirstWritePassIndex` — index into `GetPassOrder()`; `UINT32_MAX` when import-only
  - `LastReadPassIndex` — index into `GetPassOrder()`; `UINT32_MAX` when write-only
  - `FirstWritePass` — name of the first writing pass; `"external"` when import-only
  - `LastReadPass` — name of the last reading pass; `""` when no reads declared
  - `FirstWriteUsage` (`RGWriteUsage`) — usage at first write
  - `LastReadUsage` (`RGReadUsage`) — usage at last read
- `[[nodiscard]] std::vector<ResourceLifetime> GetResourceLifetimes() const`

**Semantics:**
- Calls `EnsureResourceRegistryBuilt()` (via `GetRegisteredResources()`) to
  guarantee the resource list is lazily materialised before scanning.
- Flag derivation: `IsImported` from `m_ImportedResources`; `IsExtracted` by
  resolving `m_TextureExtracts` / `m_FramebufferExtracts` handles; `IsHistory`
  from `m_TemporalHistoryContracts`; `IsTransient` from `m_TransientPlan`
  where `WillAllocate == true`.
- Walk `m_PassOrder` once per resource, checking `m_PassAccessDeclarations`;
  record the FIRST write and the LAST read. Order is deterministic and
  matches the topological execution order.

**DumpToJson additions (schema 9 → 10):**
- `frameSummary.resourceLifetimeCount`
- top-level `resourceLifetimes` array (before `timings`):
  - entries `{ resource, isImported, isExtracted, isHistory, isTransient,
    firstWritePassIndex, lastReadPassIndex, firstWritePass, lastReadPass,
    firstWriteUsage, lastReadUsage }`
  - pass indices are `-1` when `UINT32_MAX` (import-only / write-only)
- `graphDigest` extended with `;lifetimes=N`

Five tests document the behavioural contracts:
- `RenderGraphResourceLifetimes.NoLifetimesWhenNoResourcesDeclared`
- `RenderGraphResourceLifetimes.TransientResourceHasCorrectFirstAndLastPass`
- `RenderGraphResourceLifetimes.ImportedResourceHasExternalFirstWrite`
- `RenderGraphResourceLifetimes.WriteOnlyResourceHasEmptyLastRead`
- `RenderGraphResourceLifetimes.DumpToJsonIncludesResourceLifetimes`

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.

#### Phase G Slice 12 — Subresource range propagation into barriers and transitions

Closes the last structural gap required for explicit-barrier backends: every
`PlannedBarrier` and `ResourceTransition` now carries an `RGSubresourceRange`
describing exactly which mips, array layers, and depth slices the access covers.

**New struct fields:**
- `PlannedBarrier::Range` (`RGSubresourceRange`) — the subresource range from
  the consuming access declaration (`builder.Read()`). Defaults to
  `RGSubresourceRange::Full()` (all mips/layers) when no range is specified.
- `ResourceTransition::Range` (`RGSubresourceRange`) — propagated from the
  corresponding `PlannedBarrier::Range`.

**Propagation path:**
1. `builder.Read(handle, readUsage, range)` stores `range` in the
   `RGAccessDeclaration` for the consumer pass.
2. `ComputeBarrierPlan()` copies `access.Range` into both `PlannedBarrier`
   construction sites (read-after-write and write-after-write).
3. `GetResourceTransitions()` copies `barrier.Range` into each
   `ResourceTransition` record.

**Backend mapping:** A Vulkan backend can now emit a complete
`VkImageMemoryBarrier::subresourceRange` with `baseMipLevel = Range.BaseMip`,
`levelCount = Range.MipCount`, `baseArrayLayer = Range.BaseLayer`,
`layerCount = Range.LayerCount` from a single `ResourceTransition` record,
without any redesign of the public API.

**DumpToJson additions (schema 10 → 11):**
- `plannedBarriers[]` entries extended: `"range": { baseMip, mipCount, baseLayer, layerCount, baseSlice, sliceCount }`
- `resourceTransitions[]` entries extended: same `"range"` object
- `graphDigest` concat extended with `;subresourceRanges=present`
- `~0u` (full range sentinel) serialised as `-1` so consumers can distinguish
  "all mips" from "exactly one mip"

**Phase G exit criterion reached.** With subresource range now present in
every `ResourceTransition`, the compiled graph carries sufficient information
for a Vulkan/DX12 backend to emit `VkImageMemoryBarrier` / `D3D12_RESOURCE_BARRIER`
with correct subresource fields — without redesigning the public API.

Five tests document the behavioural contracts:
- `RenderGraphSubresourceRange.FullRangeByDefaultWhenNoRangeSpecified`
- `RenderGraphSubresourceRange.MipRangePreservedInTransition`
- `RenderGraphSubresourceRange.LayerRangePreservedInTransition`
- `RenderGraphSubresourceRange.PlannedBarrierCarriesRange`
- `RenderGraphSubresourceRange.DumpToJsonIncludesRange`

**Files touched.** `OloEngine/src/OloEngine/Renderer/RenderGraph.h`,
`OloEngine/src/OloEngine/Renderer/RenderGraph.cpp`,
`OloEngine/tests/Rendering/RenderGraphTest.cpp`.
