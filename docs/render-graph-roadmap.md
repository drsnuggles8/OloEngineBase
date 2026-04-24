# RenderGraph Roadmap

> **Status:** Living design doc. Reflects the `feature/rendering_improvements`
> branch at the time of writing. Owned by the rendering subsystem — keep
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

## 1. Where we are today (Option 3)

The current `OloEngine::RenderGraph`
(`OloEngine/src/OloEngine/Renderer/RenderGraph.h`) is best described as a
**topologically-ordered, statically-wired pass list with a hazard
validator**:

* Passes are `Ref<RenderPass>` instances **owned** by `Renderer3D::s_Data`,
  not by the graph. The graph only holds weak references via a name→pass
  lookup (`m_PassLookup`).
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
   per-mode rebuild (Option 3) is coarse: we switch between fully-preset
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
7. **Graph visualisation is out-of-band.** We have `ValidateResourceHazards`
   reporting but no runtime dump of "what did the graph look like this
   frame" — useful when tracking down correctness bugs.

Limitations 3–5 are what tipped us toward Option 4 as the long-term
target: they aren't structural to rendering, they're artefacts of the
"statically-wired validator" choice.

---

## 2. Where we want to go (Option 4 — true adaptive RDG)

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

### 2.2 Concrete benefits

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

### 2.3 Prior art worth studying

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

We don't need to rewrite the graph in one go. The Option 4 design can be
approached incrementally, and each step leaves the codebase shippable.

### Phase A — Promote reads/writes to first-class handles

* Introduce `RGTextureHandle` / `RGBufferHandle` as typed wrappers around
  the identifiers we already use in `DeclareRead` / `DeclareWrite`.
* Attach a lightweight `RGResourceRegistry` to the graph that maps
  handles to the *existing* physical resources owned by passes.
* Pass wiring still happens in `ConfigureRenderGraph`, but consumer passes
  look up their input framebuffer via the registry instead of via an
  explicit `ConnectPass` call.
* No behavioural change yet; this is a refactor to separate identity from
  ownership.

### Phase B — Add a transient framebuffer pool

* Create an `RGTransientPool` keyed on `{ width, height, format, samples }`.
* Audit passes that create intermediate render targets with predictable
  lifetimes (Bloom chain, SSAO scratch, DOF bokeh, SSS blur, tone-map
  scratch). Convert them to request from the pool with a declared
  lifetime (`first-use`..`last-use`).
* Keep permanent passes (ScenePass main colour, shadow atlas) owning
  their own resources — those are used every frame and never aliased.
* Measure VRAM before/after. Target: ≥20% reduction in peak render-target
  memory at 1080p.

### Phase C — Setup/execute split

* Introduce `RGBuilder` with `.Read(handle, usage)` / `.Write(handle,
  usage)` / `.Create(desc)`. Passes get a `Setup(RGBuilder&)` callback.
* Existing passes gain a thin `Setup` that re-emits their current
  `DeclareRead` / `DeclareWrite`. No behavioural change; this is the
  foundation for the compiler.
* The graph's `Compile()` step now owns what `ConfigureRenderGraph` does
  today, driven by the declarations instead of explicit `ConnectPass`.

### Phase D — Reachability-based culling

* `SetFinalPass` becomes the root of a backwards-reachability walk.
* Passes that don't contribute to the final output are pruned from the
  execution list and their transient resources never allocated.
* Toggles like "SSAO off" become declarative — `SSAOPass::Setup` no
  longer emits a write when disabled, and the whole subgraph falls out.

### Phase E — Automatic barriers (prep for non-GL backends)

* Each `RGReadUsage` / `RGWriteUsage` value (ShaderSample, ColourAttach,
  DepthAttach, Storage, TransferSrc, TransferDst, ...) maps to a
  state/barrier descriptor.
* For GL 4.6 this is mostly a no-op or
  `glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | ...)` calls at the
  right points.
* For a future Vulkan/DX12 backend this is where `vkCmdPipelineBarrier`
  gets emitted.

### Phase F — Async compute, view decoupling, multi-frame graphs

* Only worth tackling once A–E are shipped and stable. These are the
  "true RDG" payoff features but also the ones that can introduce the
  most regressions if attempted first.

Each phase should add its own tier of tests to the existing 11-layer
pyramid (see [renderer-testing.md](renderer-testing.md)). Compiler-level
behaviour (culling, aliasing, barrier placement) is especially
property-test-friendly.

---

## 4. Explicit non-goals

* **Not a wholesale rewrite in one commit.** Each migration phase lands
  behind the existing validator without breaking scenes.
* **Not a replacement for the command bucket.** Command buckets sit
  *inside* a pass and schedule individual draw calls. The RenderGraph
  schedules *passes*. The two concepts compose; neither replaces the
  other.
* **Not a generic DAG framework.** Keep the API rendering-specific.
  Avoid the temptation to make this a general-purpose task scheduler —
  that's `BS::thread_pool` / job-system territory.
* **Not a dependency on a specific backend.** The design must work for
  GL 4.6 today and Vulkan/DX12 tomorrow; no GL-specific types leak into
  the graph's public API.

---

## 5. Signals that we should start Phase A

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

Until one of those lands we stay on Option 3; the per-mode rebuild is
enough, and the cost/benefit of pulling the compiler in ahead of real
demand isn't favourable.

---

## 6. Change log

* **Initial entry** — Option 3 landed (per-RenderingPath topology rebuild
  via `RenderGraph::ResetTopology` + `Renderer3D::ConfigureRenderGraph`).
  This document records the rationale and the Option 4 target.
* **Graph introspection.** `RenderGraph::DumpToDot(path)` writes a
  Graphviz snapshot of the current topology (final pass double-ringed;
  solid blue edges = framebuffer-producer, dashed grey = execution
  order); editor panel lists `GetPassOrder()` live and exposes the
  dump-to-DOT button.
* **L1 coverage.** `RenderGraphResetTopology` GTest suite (3 tests:
  rebuild, reference ownership, repeated reset) locks in the Option 3
  per-path rebuild behaviour.
