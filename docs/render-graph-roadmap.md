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
- `SSAOPass` and `GTAOPass` setup declarations now read `SceneNormals`
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

Exit criterion: resource edges derive execution order; manually-authored
ordering edges are only needed for real side effects that do not touch graph
resources.

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

### Phases B, D–G — Not yet started

The following milestones remain:

* **Phase B** — typed frame blackboard; `ImportTexture` / `ImportBuffer` /
  `ImportHistory`; queued extraction for TAA/fog histories; retire direct
  pass-to-pass `Ref<Framebuffer>` setters in favour of handle handoffs.
* **Phase D** — transient pool and lifetime-based aliasing for GL 4.6.
* **Phase E** — backward-reachability culling; automatic `glMemoryBarrier`
  insertion from declared access modes; DOT/JSON dump with timings.
* **Phase F** — full renderer adoption (deferred, AO/HZB, OIT, post-process
  split, temporal histories, multi-view shadow/probe).
* **Phase G** — async compute scheduling, multi-frame graph, backend-readiness
  audit.
