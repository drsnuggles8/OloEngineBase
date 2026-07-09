# Render-pipeline caches must invalidate on every reconfigure, not just on a fingerprint change

Short rule for anyone adding a **process-wide cache** to `Renderer3D::s_Data`
(the `RenderPipeline`, the `RenderGraph`, or anything with their lifetime).

## The trap

`Renderer3D::s_Data` and everything it owns — `s_Data.Pipeline`
(`RenderPipeline`) and `s_Data.RGraph` (`RenderGraph`) — live for the whole
process: created once in `Renderer3D::Init`, torn down once in
`Renderer3D::Shutdown`. In the test binary that means **one instance shared by
every test**. A cache stored there therefore survives across path switches,
scene loads, and (in tests) across `TEST_F`s.

A `ConfigureRenderGraph` / path switch / AO-technique switch rebuilds the graph
topology via `RenderGraph::ResetTopology()`, which **wipes** the blackboard
(`m_Blackboard`) and the imported-resource maps. Any cache that assumes those
survived between calls is now stale — but a cache keyed on a hash of
*scene/settings inputs* can't see the wipe, because the inputs are identical.

That is exactly issue **#530**: the second consecutive `RenderingPath::Deferred`
entry in one process recomputed the *same* blackboard-populate fingerprint as
the first, so `PopulateBlackboard` short-circuited past the freshly-wiped
blackboard, every pass's `Setup()` then read empty handles, `RGBuilder`
silently dropped every declaration, and the whole 37-pass graph culled
(`reads=0/writes=0`) — a blank frame with no GL error.

## The rule

**A cache whose validity depends on the blackboard / imported-resource maps
surviving must be invalidated by the same event that wipes them — a topology
reset — not merely by a change in the inputs it happens to hash.**

Prefer coupling the cache key to the structural event over enumerating call
sites:

- `RenderGraph::GetTopologyGeneration()` is a monotonic counter bumped by every
  `ResetTopology()` / `Shutdown()` (the two places that wipe the blackboard).
  Hash it into any per-frame fingerprint (`ComputeBlackboardFingerprint` does)
  and the cache self-invalidates on **any** reconfigure — including a *future*
  reconfigure path that forgets to call an explicit invalidation hook.
- An explicit `InvalidateBlackboardCache()` at each settings-change site is the
  fragile alternative: it only invalidates the paths you remembered to wire.

If you add a new process-wide render cache, ask: *what wipes the state I'm
caching, and does my cache key move when that happens?* If the answer is "a
topology reset", fold `GetTopologyGeneration()` into the key.

## Guard

`RenderGraph.ResetTopologyAdvancesTopologyGenerationForCacheInvalidation`
(CPU, `RenderGraphTest.cpp`) pins the generation-bump contract.
`DeferredOccludedInstanceFieldScene.ReenteringDeferredPathDoesNotCullEntireGraph_Issue530`
(GPU, `OcclusionCullDeferredVisualEvidenceTest.cpp`, SKIPs headless)
reproduces the full reconfigure→reconfigure→rerender sequence and fails on a
blank frame if the cull returns.

---

# The transient pool is a size-keyed cache: evict it on any node resize, not just a display resize

The `TransientPool` (owned by `RenderGraph`) recycles GPU framebuffers/textures
across frames, bucketed by **full spec including width/height**. It is the same
family of trap as the blackboard cache above: a *process-wide cache* that must
be invalidated by the **structural event that makes its entries stale**, not by
a proxy for that event.

## The trap (issue #563 — the render-graph half of #549)

An FSR1 upscale (`Upscale != Off`) renders the scene at a **reduced band**:
`RenderPipeline::PopulateBlackboard` shrinks the ScenePass / SSAO / GTAO
framebuffers below display res, which cascades to the `SceneColor` / `SceneDepth`
/ `SceneNormals` / `Velocity` transient descriptors. Toggling back to native
restores those nodes to full res. Crucially, **this restore can happen without
the display (physical) dimensions ever changing** — a runtime `Upscale`→`Off`
toggle, or re-entering a full-size scene in one process after a prior test left
the band reduced.

`RenderGraph::Resize` used to evict the pool **only when the display dimensions
changed**. So a same-display-size resize that restored a reduced node to full
res left the pool holding stale reduced-size (and paired stale full-size)
framebuffers. The alias-group resolver in `MaterializeTransientResources` then
handed one of those stale transients to the scene chain for the first ~2 frames
after the transition, so `SceneColor` resolved to a never-written (zeroed)
texture and the whole frame rendered **black for ~2 frames**, then recovered.
Order-dependent: it only bit when an earlier test (or runtime action) had left
reduced-band transients in the pool.

## The rule

**Evict the transient pool whenever a transient descriptor's size can have
changed — i.e. whenever any graph node's framebuffer actually resized — not only
when the display dimensions changed.** Two sites drive a scene-band resize and
both must evict:

- `RenderGraph::Resize` — clears the pool when `dimensionsChanged || anyNode
  resized` (it now compares each node's `GetFramebufferSpecification()` before/
  after `ResizeFramebuffer`). This catches the window/viewport-resize entry path
  where a node is restored from a reduced band while the display size is
  unchanged. A genuinely idle resize (same display size, no node actually
  resized) still skips the clear, so steady-state frames don't churn the pool.
- `RenderPipeline::PopulateBlackboard`'s FSR1 scene-band resize block — clears
  the pool right after `ScenePass->ResizeFramebuffer`, because a **runtime**
  `Upscale`-mode toggle changes the band with no window resize and never reaches
  `RenderGraph::Resize`.

Generalise: if you add a transient whose size is derived from something other
than the display resolution (a render-scale, a half-res AO band, a shadow
cascade size), ask *what changes that size, and does the pool get evicted when
it does?* A size-keyed cache that outlives a size change silently serves stale
(or wrong-alias) entries.

## Guard

`RenderGraphTransientPool.ResizeEvictsPoolWhenNodeResizedButDisplayUnchanged`
(CPU/GL-gated, `RenderGraphTest.cpp`, SKIPs headless) shrinks a registered node
to a reduced band then resizes at the **same display size** and asserts the pool
is emptied. `RenderGraphTransientPool.ResizeEvictsStalePoolEntries` still pins
the paired "same-dimension idle resize must **not** churn the pool" contract.
`EASUVisualEvidenceTest.ForwardUpscaleOffTransitionKeepsSceneVisible`
(GPU, SKIPs headless) reproduces the runtime `Performance`→`Off` transition with
a 2-frame warm-up and fails on a black frame if the pool keeps stale transients.

---

# Pass objects survive a topology reset — but `RenderGraphNode::OnReset()` is a dead hook (#595)

Two different "resets" exist and they have **different** lifetimes for the pass
objects — don't conflate them:

- **`RenderGraph::ResetTopology()`** — the per-build topology wipe, called at the
  top of `BuildRenderPipelineGraph` (every path build). It clears `m_NodeLookup`,
  invalidates every RG resource/framebuffer handle slot, and bumps
  `GetTopologyGeneration()`. The **pass objects persist** across it: the pipeline
  re-registers the *same* long-lived `PostProcessPasses.*` / `FrameCorePasses.*`
  instances into the freshly-reset graph.
- **`RenderPipeline::Setup()`** — the heavier reconfigure. It calls
  `RenderPipeline::Reset()` (drops every pass `Ref<>`, destroying the objects)
  then `CreateFramePasses` / `CreatePostProcessPasses` (constructs fresh objects
  via `Ref<T>::Create()` and calls `Init()` on each). Here the pass objects
  **do not** survive.

Because a pass persists across `ResetTopology()`, any RG handle it cached is now
dangling. The engine handles this **not** via a reset hook but by re-resolving
every RG handle from the current graph inside `Execute()` each frame
(`GetPrimaryInputTextureHandle()` → `context.ResolveTexture(...)`,
`GetPrimaryOutputFramebufferHandle()` → `context.ResolveFramebuffer(...)`; a
pass's `m_Target` is reassigned from that fresh resolve every Execute). So no
cross-frame RG handle is trusted to survive a reset in the first place.

Consequently **`RenderGraphNode::OnReset()` has zero call sites anywhere in the
engine** (grep it — every occurrence is a definition/override or an unrelated
SoundGraph `m_OnReset`). It is a vestigial virtual from an earlier
persistent-and-reset design; the ~38 overrides that null `m_Target` / cached
handles are dead-but-harmless (the fields are also re-resolved every Execute).
Do **not** assume `OnReset()` runs on a topology reset — it does not. If you
genuinely need per-reset work on a persistent pass, either wire a real call site
into `BuildRenderPipelineGraph` / `ResetTopology` first, or (the existing idiom)
re-resolve the state in `Execute()` from the live graph. `FinalRenderPass::OnReset`
is documented as an intentional no-op on this basis: its `m_BlitShader` is a plain
shader asset rebuilt only by `Init()` (i.e. only on the destroy-and-recreate
`Setup()` path), and its fullscreen triangle is a self-healing static owned by
`MeshPrimitives`, not the pass.
