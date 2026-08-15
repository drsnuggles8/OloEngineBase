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

# A consumer must key off what the GRAPH is wired for, not what the settings ask for (#771)

`ActiveAOTechnique` decides **which AO pass is registered** — `RegisterSceneAndLightingNodes`
switches on it and calls `graph.AddNode("SSAOPass" | "GTAOPass")`. That switch runs at
**topology-build time**, so the field has two readers with different clocks: the topology (updated
only by `ConfigureRenderGraph`, recorded in `s_Data.ActiveGraphAOTechnique`) and the per-frame
pipeline hook (reading `s_Data.PostProcess.ActiveAOTechnique` live). Setting the field without
`Renderer3D::ApplyRendererSettings()` makes them disagree indefinitely.

## The trap (issue #771)

While they disagreed, `PopulateBlackboard` declared `AOBuffer` and enabled `AOApplyPass` because the
*requested* technique was GTAO — but no `GTAOPass` was in the graph to write it. `AOApplyPass` does
not know its producer is missing; it samples `AO.AOBuffer` unconditionally and computes
`sceneColor * mix(1.0, ao, intensity)`. So the whole frame was multiplied by a transient nobody had
written.

**`ao = 0` is not "no data" — it is maximum occlusion.** On a freshly allocated (zeroed) transient
that is `sceneColor * 0`, an *exactly* black frame; on a recycled, still-dirty one it is merely very
dark. That single difference is the entire "flake":

- **It tracked allocation history, not code.** The `Off → Performance` FSR1 switch resizes the scene
  band and evicts the transient pool on the same edge (#563), so the AO buffer landed on fresh
  storage exactly there. Whether that storage read as zeroes depended on which tests had run before.
  It passed in isolation, in its own suite, and against most subsets; it failed only in full-suite
  order — and it was dismissed as order-dependent for weeks, including in PR #794's evidence.
- **The instrument that "fixed" it is the tell.** `OLO_RG_POISON_TRANSIENTS=1` made it pass while
  `OLO_RG_DISABLE_ALIASING=1` did not. Poison writes non-zero into every freshly acquired transient,
  so "poison fixes it, aliasing does not" reads as *some consumer is sampling storage nobody wrote
  this frame*. Treat that pair as a diagnosis, not a workaround.
- **The graph will tell you outright.** The `RenderGraph AO/Post order:` trace printed `GTAO=n/a`
  and the submission plan contained no `GTAOPass` at all. When a pass's output looks wrong, check it
  is in the plan before debugging its contents.

This had already bitten twice — issue #533, and `olo_render_toggle_pass` in `McpTools.cpp` — and
both were fixed by remembering to call `ApplyRendererSettings()` at that one call site. Enumerating
call sites is the fragile half of the rule at the top of this document; the third victim was a test
that simply set the field.

## The rule

**Where a setting decides graph TOPOLOGY, every per-frame consumer of that decision must read the
value the graph was built with, not the pending one.** `RenderPipeline`'s AO gates — the `AOBuffer`
declaration, the SSAO/GTAO scratch declarations, and `AOApplyPass::SetEnabled` — now key off
`data.ActiveGraphAOTechnique`. A technique change that has not reached the topology yet degrades to
**"no AO"**, which is correct-looking, instead of a black frame. Applying the setting still requires
`ApplyRendererSettings()`; what changed is the cost of forgetting.

Corollaries worth carrying:

- **Ask what a consumer's "unwritten" value MEANS to it.** For a multiplicative modulation (AO,
  shadow mask, fog transmittance) the neutral element is 1.0 and zero is the maximum-effect end of
  the range — exactly what fresh storage supplies. Publish the neutral element on every path where
  the producer does not produce. Both AO producers now do, via
  `Passes/AOTargetIdentity.h::PublishAOTargetAsFullyVisible` — deliberately shared, because the two
  write the same graph resource under the same contract and a reimplementation in each is how this
  invariant drifts. `SSAORenderPass` *looks* like it always had this (it clears to white every
  frame), but that clear is on its own SSAORaw/SSAOBlur scratch and sits **after** its early
  returns, so it never protected `AOBuffer` either.
- **The gate must cover BOTH ends of the seam.** Keying only the consumer off the graph's technique
  fixes one direction and leaves the mirror: the producers bail out of `Setup`/`Execute` on their
  own `m_Settings.ActiveAOTechnique`, so feeding them the *requested* technique while the consumer
  reads the *graph's* reproduces the identical black frame with the roles swapped. `RenderPipeline`
  hands both ends the graph's technique from one place.
- **A value the producer cannot emit is a free assertion.** `GTAO.comp` clamps visibility to
  `[0.03, 1.0]`, so an AO texel of exactly 0 reaching the apply pass can *only* be unwritten
  storage. Where a producer has such a floor, say so in the test suite — it converts "the frame
  looked dark" into "this texel was never written".
- **Silent skips cost weeks.** All four of `GTAORenderPass::Execute`'s early returns were unlogged,
  so the differential evidence had to be gathered by bisecting suite order. They now emit a
  rate-limited warning naming which guard fired.

## Guard

`GTAOMath.AoConsumersKeyOffTheGraphsTechniqueNotTheRequestedOne` (CPU, `GTAOMathTest.cpp`) pins the
gate: the AOApply enable decision and the `AOBuffer` declaration must read `ActiveGraphAOTechnique`.
`GTAOMath.GtaoAoTargetIsNeverLeftUnwrittenForTheApplyPass` (CPU) pins the second layer — the
shader's positive visibility floor, and that **every** early return in `GTAORenderPass::Execute`
after the AO target resolves publishes the no-occlusion identity first, so a fifth guard added later
cannot reintroduce the hole. `EASUVisualEvidenceTest.GTAOSurvivesRuntimeUpscaleSwitch` (GPU, SKIPs
headless) is the end-to-end repro — and note it never actually exercised GTAO until it was taught to
call `ApplyRendererSettings()`, which is why a green run of it had proven nothing.

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
