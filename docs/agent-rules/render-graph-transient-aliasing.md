# Render-graph transient aliasing: version renames, stale pool reads, and how to hunt them

Distilled from the one-frame black-square artifact (VehiclesTest water scene,
issue #438 follow-up): squares of constant screen size flashing for one frame
when the camera pose changed, hit rate growing from ~3% to ~14% of
camera-move frames as the session aged, immune to disabling any single render
pass. The hunt found **two independent mechanisms that produced
near-identical symptoms** — a `WriteNewVersion` orphan-allocation bug (below)
and a single-NaN-pixel bloom amplification (§ NaN amplification at the end).
When a repro survives a fix that provably closed one mechanism, re-measure
from scratch instead of assuming the fix missed: matching symptoms are weak
evidence of a shared cause.

## The contract: `WriteNewVersion` is a rename, never an allocation

`RGBuilder::WriteNewVersion(handle, usage, tag)` exists for RMW dependency
bookkeeping — "the state of `SceneColor` after this pass wrote it" — so the
hazard validator doesn't see a same-pass feedback loop and downstream
name-based readers trace to the right producer. The versioned name
(`SceneColor@GPUDrivenOcclusionPass`) refers to the **same physical
resource** as its source:

- `RenderGraph::m_VersionAliasTargets` records `versioned → source`;
  `ResolveTexture` / `ResolveFramebuffer` / `ResolveBuffer` follow the chain
  to the source's physical (via the direct by-name maps, **not**
  `Get*Handle`, whose latest-version redirect would loop).
- The transient planner (`RenderGraphTransientPlanner::ComputePlan`) never
  allocates a version entry (`SkipReason = "version-alias"`) and **folds a
  version's accesses into the source's lifetime** — a pass reading only the
  version must still keep the base's backing alive, or the alias-slot
  assigner can legally hand it to another same-descriptor transient first.
- `MaterializeTransientResources` treats version names like imported /
  externally-backed entries: no pool acquire, no physical wiring.

Pinned by `RenderGraphVersionAlias.*` in
[RenderGraphTest.cpp](../../OloEngine/tests/Rendering/RenderGraphTest.cpp).

## The bug archetype this guards against

Before that contract existed, every `WriteNewVersion` materialized an
**orphan pool object** for the versioned name. Consumers that resolved
through the version — the unqualified latest-version redirect
(`GetFramebufferHandle("SceneColor")`), or a versioned attachment view whose
parent is the versioned FB (`SceneColorTexture@GPUDrivenOcclusionPass`, the
exact path `ReadFirstValidVersionedInputForPass` walks) — sampled the orphan,
while the producing pass rendered into the base handle's physical. Three
properties made this near-invisible:

1. **LIFO pool stability**: with a stable plan, release order and acquire
   order are deterministic, so the orphan tends to receive *last frame's*
   real texture for the same logical resource. One-frame-stale scene color is
   indistinguishable to the eye. Every "is this target clean?" capture is
   equally fooled — it shows a plausible scene.
2. **Plan rebuilds reshuffle the mapping**: a camera-pose change that flips
   pass reachability (planar reflection, occlusion buckets, …) dirties the
   graph, `RebuildTransientPlan()` reassigns alias slots, and the orphan
   lands on arbitrary same-descriptor pool content for exactly one frame —
   the visible garbage/black-square frame. Steady state then re-hides it.
3. **Pool aging raises the hit rate**: high-watermark frames leave extra
   objects per bucket (`Trim` keeps `maxPerBucket`), so the longer the
   session, the more shuffle candidates and the more often a rebuild frame
   lands on foreign content. A bug whose repro rate *grows over a session*
   is a strong pool-aging signal.

Corollary: **per-pass toggle bisection cannot find this class.** Disabling
the pass that *created* the version doesn't stop consumers from resolving a
stale name (publication/latest-maps outlive the pass — same trap as
[render-pass-published-state.md](render-pass-published-state.md)), and the
corruption follows whichever passes remain. Ten single-pass disable sweeps,
200 frames each, all came back negative on this bug.

## The instruments (permanent, env-gated)

Both live in `RenderGraph.cpp` (`MaterializeTransientResources`) and cost
nothing when the env vars are unset:

- **`OLO_RG_POISON_TRANSIENTS=1`** — clears every pool-acquired transient
  texture / FBO color attachment at materialize time to a **per-resource
  hue** (12-color palette keyed by resource-name hash; the mapping is logged
  once per resource as `RG poison map: <resource> -> <color>`). Any texel
  that reaches a consumer without being written *this frame* is unmistakable,
  and its color names the resource it leaked from. This converts a ~3%
  stochastic camera-move artifact into a **deterministic every-frame
  signal** — the difference between 200-frame statistical sweeps and a
  single screenshot.
- **`OLO_RG_DISABLE_ALIASING=1`** — every transient resource gets its own
  physical backing (alias-slot sharing off, `WillAllocate=false` entries
  acquire too). If an artifact disappears under this switch, the planner's
  lifetime analysis let two live resources share one object.

Launch via the driver so the env reaches the editor:
`$env:OLO_RG_POISON_TRANSIENTS='1'; driver.ps1 -Action attach`.

## Verify every instrument before trusting an exoneration

The most expensive detour of this hunt came from a *fake* A/B lever:
`olo_render_toggle_pass` takes lowercase FEATURE names (`gtao`, `taa`,
`fog`, `bloom`, `fxaa`, `godrays`, ...) — not render-pass node names. Every
`'SomethingPass'` toggle returned an "Unknown pass" error text that the
sweep scripts swallowed, so ten different passes were "exonerated" by
toggles that never toggled anything. Rules:

- Read and assert on EVERY tool response in a scripted battery — an error
  string in a result the script ignores becomes a fabricated data point.
- An A/B lever must PROVE it acted: diff the frame against the baseline and
  require a change consistent with the lever (disabling AO must visibly
  brighten creases; disabling water must remove the water). A toggle whose
  "off" frame differs from baseline only by frame noise did nothing.
- Restore state by writing back the PREVIOUS value the tool reported — an
  unconditional off-then-on pattern silently ENABLES features that started
  disabled (this battery turned the whole fog family on mid-run).

## Hunt protocol (what actually worked, in order)

1. **Statistical stage attribution** — repeated forced-frame captures of
   each stage output at a fixed pose cadence (`olo_render_capture_target`,
   ~100 frames per target), classifying artifacts offline. This narrows
   "where does corruption first appear" — but beware: a capture that reads a
   resource by *name* can itself resolve a stale physical, so a "clean"
   stage may be a one-frame-old copy that merely looks clean. Attribution
   told us AOApply's output was corrupt while its inputs read clean — true,
   but the *reason* was the name-resolution seam, not the pass.
2. **Poison mode** — whole-screen tint in steady state proved "something
   samples pool content not written this frame, every frame", killing all
   pass-local theories at once.
3. **Hue-coded poison** — the on-screen color + the logged `RG poison map`
   named the leaking resources directly (`SceneColor@GPUDrivenOcclusionPass`).
4. **Read the declaration site** — the resource name's `@tag` pointed
   straight at the `WriteNewVersion` call and the orphan-allocation path.

Tools that already exist and shortcut step 1 (from the #607 batch —
check them before building anything): `olo_render_graph_topology_export`
carries **resolved physical GL ids** per pass access ("do these two passes
touch the same physical texture this frame" is one lookup), and
`olo_render_validate` reports **versioned-name physical-id groups** — a
version whose physical differs from its base's is exactly this bug.

## Review checklist for new graph features

- Any new `Get*Handle`-by-name consumer: remember unqualified names redirect
  to the **latest version**; if you want the canonical base physical, that's
  fine post-fix (same object), but the *dependency* you record traces to the
  version's producer.
- Any new mechanism that mints a derived resource name (views, versions,
  MSAA resolves, mip views): decide explicitly whether it's a **rename of an
  existing physical** (register a resolution alias; never allocate) or a
  **real allocation** (planner entry). The failure mode of getting this
  wrong is invisible-until-reshuffle, not a crash.
- Anything holding a `Ref` to a pool object across `ReleaseAll()` does NOT
  own it — the pool will hand the same object to the next acquirer
  regardless of refcount. Cross-frame state must live in renderer-owned
  storage (the `RegisterHistoryTextureSink` / `ImportHistory` copy pattern
  TAA uses), never in a retained pool object.

## NaN amplification: one bad pixel becomes a 300 px black square

The second mechanism behind the same symptom. The causal chain, caught live
by the hunter below (each step is one log line):

1. `Water.glsl` emitted a **single NaN pixel** (3 NaN channels) on ~10% of
   camera-jump frames — an interpolated vector crossing zero length at one
   fragment, then `normalize()` → NaN. Guards now: `safeNormalize(v,
   fallback)` at every fragment-stage normalize of an interpolated or summed
   vector (the `len2 > 1e-12` predicate is false for NaN inputs too), plus
   zero-length guards on the VS/TES FFT-derivative normalizes.
2. Bloom's 13-tap downsample spread it (any tap touching NaN → NaN), the
   additive upsample spread it back up — 3 NaN channels in, **367,074 out**
   (~303×303 px). One BloomMip4 texel is 1411/44 ≈ 32 full-res px, which is
   exactly the observed staircase step of the block's edge. Guard now: a
   **non-finite kill** at the end of `PostProcess_BloomDownsample.glsl` —
   ternary select, NOT `mix()` (`mix(NaN, 0, 1)` is still NaN because it
   multiplies the NaN operand). This single sanitize point protects the
   pyramid from every upstream NaN producer, present and future.
3. `scene + NaN = NaN` in the additive composite → tonemap(NaN) → black
   block on screen, AND the auto-exposure histogram metered the NaN region →
   the same frames showed a global exposure wash-out. Two "different"
   symptoms, one root.

Diagnosis levers (both permanent, env-gated, in `RenderGraph.cpp`):

- **`OLO_RG_BLACKSQUARE_HUNT=1`** — after every executed pass, read back the
  pass target (plus a small watchlist of chain resources) and log any
  ≥64 px solid-black block AND any NaN channel (`BLACKSQUARE HUNT NAN: after
  pass 'X' … N NaN value(s), first at texel (x, y)`). The first pass logged
  is the origin. Heavy (full-res readback per pass) — hunts only.
- Reading a "clean" capture of a resource is only trustworthy at a pinned
  time (`afterPass:`): after its last read, a pooled backing is LEGALLY
  reused by later transients, so end-of-frame captures of mid-chain
  resources show the reuser's content, not corruption.
- The NaN scan matters because a `< threshold` block scan classifies NaN as
  black but can't see isolated NaN PIXELS (a 1-pixel origin is invisible
  until bloom amplifies it) — census NaNs per channel, not blocks.
