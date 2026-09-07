# The reflection tier-selection contract — one quantity, ordered estimators, weights that sum to one

Issue #1057 (#979 Phase 2). The engine has four sources of reflected light — planar reflections,
SSR, a ray-query tier (new here) and probe/IBL — and had no policy saying which one answers a given
pixel. This ADR is that policy. It is written as an algebra rather than a flowchart because #979's
non-goal — *"do not silently double-count DDGI/SSGI/RT/PT contributions"* — is a property that has
to hold by construction; a flowchart with four branches cannot be checked, and a double-count is
close to invisible in a still frame.

The contract is the deliverable. The shaders are an implementation of it.

---

## 1. The rule

Every tier estimates **the same physical quantity**: the radiance arriving at the shading point
along the specular lobe. They are not different lights to be added; they are competing answers to
one question, and they differ only in how much of the lobe each can actually see.

A tier therefore emits two things and nothing else:

- `L_t` — its estimate of that radiance.
- `c_t ∈ [0,1]` — its **confidence**: the fraction of the lobe this tier is entitled to answer for.

The tiers are ordered, best-informed first:

    planar  >  SSR  >  ray query  >  probe/IBL

and the composite is an ordered "over", evaluated **from the bottom up**:

    R := L_ibl                          // the bottom tier is pinned at c = 1
    R := mix(R, L_probe, c_probe)
    R := mix(R, L_ray,   c_ray)
    R := mix(R, L_ssr,   c_ssr)
    R := mix(R, L_planar,c_planar)

Expanding gives each tier's effective weight:

    w_t = c_t · Π_{u above t} (1 - c_u),      w_ibl = Π_{u ≠ ibl} (1 - c_u)

**Σ w_t = 1 exactly, for every possible vector of confidences.** It telescopes, because the bottom
tier is pinned at `c = 1`. That identity is the whole no-double-count guarantee: there is no
confidence assignment, valid or absurd, that makes the tiers sum to more or less than one. It is
also cheap to test on the CPU with no GPU at all, which is what `ReflectionTierContractTest` does —
energy conservation as a ratchet, not as a comment.

## 2. Why bottom-up is the load-bearing decision

The obvious reading of "planar beats SSR beats rays beats probes" is top-down: let the best tier
claim its share and hand the *residual* down. That is the same algebra, and it is much worse to
build, because it forces every tier to know the confidence of every tier above it. Concretely: SSR's
confidence is a per-pixel value computed inside its trace, currently baked into the delta it writes.
A ray-query tier that had to claim `1 - c_ssr` would need that value transported through SSR's
five-stage denoiser chain (#708) — whose only spare lane, the signal's alpha, already carries view
depth on every path including the early-outs, deliberately.

Evaluated bottom-up, **no tier needs to know anything about the tiers above it.** Each one lerps
over whatever is already there. The ordering is expressed purely by *where a tier sits in the
frame*, not by data it has to be handed. That is why the ray tier can be inserted below SSR without
touching a single line of the denoiser chain.

This is not a trick; it is the same reason back-to-front alpha compositing needs no per-layer
bookkeeping.

## 3. This is already how two of the boundaries work

The contract is largely a *statement* of existing behaviour, which is the main reason to trust it.

**probe over IBL** — `include/DeferredLightingShared.glsl` already does exactly
`mix(globalPrefilter, probeSpecular.rgb, probeSpecular.a)`. `oloSampleReflectionProbes` returns its
confidence in alpha. That is `c_probe`, already named and already used.

**SSR over what is below it** — `PostProcess_SSR.glsl` writes the signed delta
`(reflTarget - baseColor) * blend`, and `PostProcess_SSRComposite.glsl` adds it, because
`mix(base, refl, blend) == base + (refl - base) * blend`. So `blend` **is** `c_ssr` and SSR is
already a correct "over" against whatever produced `baseColor`. Its own comment gives the reason in
the contract's terms: adding instead of lerping *"double-counts and washes out (sky + object)"*.

So the hierarchy's bottom three boundaries were already contract-shaped. What was missing was the
statement, a tier between SSR and the probes, and a way to see which tier answered.

## 4. Where the ray-query tier goes, and what it may shade

**Position: below SSR, above the probes.** It fills exactly the gap SSR cannot cover — off-screen
and occluded hits — and it composites over the probe/IBL result that `DeferredLighting` produced.
Because SSR then lerps over *its* output by `c_ssr`, a pixel where SSR is confident is unchanged,
and a pixel where SSR fades out at the screen edge lands on a ray-traced answer instead of dropping
to a low-frequency probe. That edge drop is the seam this issue exists to remove.

    c_ray = hitValid · fresnel · roughnessGate · rayTracingAvailable

`roughnessGate` falls to zero above a roughness threshold: rays are spent where the lobe is narrow
enough for one sample to mean something, and rough surfaces stay on probes, where a ray budget buys
nothing. On a miss `c_ray = 0` and the tier contributes exactly nothing — the same "a miss costs
nothing" property SSR's early-outs already have.

**What it may shade — the #805 boundary, stated plainly.** A reflection tier must *shade* its hit,
where the shadow tier (#1063) needed only a visibility bit. Arbitrary material texture sampling at a
hit needs a shader-visible sampler heap, which is **#805, open and not in this slice** — this is why
`include/RayTracingAlphaTest.glsl` takes texture fetch as a caller-supplied macro.

What *is* reachable without the heap is the whole untextured material record: the hit's instance
slot (`rayQueryGetIntersectionInstanceCustomIndexEXT`) resolves through `GPUSceneInstance` to a
`GPUSceneMaterial` carrying `BaseColorFactor`, `MetallicFactor`, `RoughnessFactor` and
`EmissiveFactor`. So the first slice shades **opaque and masked hits with flat material constants**
lit by the sun plus sky/probe irradiance.

The consequence, said out loud rather than discovered later: **a textured surface reflects its base
colour factor, not its texture.** A brick wall reflects flat brick-red. This is a real quality
ceiling and it is deferred behind #805, not worked around.

It is nonetheless worth shipping, and the reason is specific rather than optimistic: the artifact
this issue targets is a *discontinuity* — reflections changing character at a screen border — and
what removes it is having the right silhouette, at the right depth, in roughly the right colour
where there was previously a low-frequency probe smear. Flat-shaded hits deliver that. They do not
deliver a correct mirror, and the contract above is what lets the textured version replace them
later by raising `L_ray`'s quality without touching a single weight.

## 5. Fallback is loud and countable

There is no silent degradation (`docs/agent-rules/no-silent-fallbacks.md`). When ray tracing is
unavailable — a no-RT device, `OLO_VULKAN_NO_RAY_TRACING=1`, an empty TLAS, or the GL backend —
`c_ray` is pinned to 0 for every pixel. The algebra then collapses to exactly today's
planar → SSR → probe/IBL hierarchy, because `mix(R, L_ray, 0) == R`.

That is the *reason* the raster-only output is byte-identical when the tier is off: it is not a
tested coincidence, it is `mix(x, y, 0) == x`. The pass reports the reason it stood down through a
stats struct with a de-duplicated warning, following `RayTracedShadowPass`'s precedent exactly.

**The trap this cost real time to learn:** an empty TLAS and a correctly-falling-through tier look
identical on screen. `olo_rt_scene_stats` must be checked before any ray-traced evidence is
believed.

## 6. Planar is a forward-path tier, and today that is a real gap

Stating the hierarchy exposed something the issue assumed was already true. **Planar reflections and
SSR never coexist.** `PlanarReflectionRenderPass` disables itself on the deferred path (a single
replayed opaque bucket would capture the G-Buffer, not lit colour), and its result is consumed only
by `Water.glsl` through the planar-reflection UBO. SSR is deferred-only.

So `c_planar` is structurally zero everywhere the other three tiers run, and the top boundary of the
hierarchy is presently untested by any real frame. The contract is written for four tiers because
the algebra is the same for four as for three and the deferred planar resolve is a known future
slice — but the honest statement of today's state is that this ADR orders **three** live tiers and
reserves the fourth's seat.

This is a finding, not a deferral of work in scope: nothing in #1057 asks for a deferred planar
resolve, and adding one would be a substantially larger change than the tier it would feed.

## 7. Consequences

- The composite order is now load-bearing and belongs in the render graph, not in a shader's head:
  the ray tier must run after `RayTracingScenePass` (by-name dependency) and before `SSRRenderPass`.
- A new tier is added by inserting one `mix` at the right depth and defining its confidence. It
  cannot double-count, whatever it does, as long as the bottom tier stays pinned at 1.
- **The bottom tier must never report a confidence below 1.** That is the single invariant the whole
  guarantee rests on; a probe/IBL tier that "admits it doesn't know" would leave energy unclaimed
  and darken the frame. Uncertainty at the bottom is expressed by widening the lobe, never by
  lowering the weight.
- Confidence is *arbitration*, not an estimator weight. SSR's `blend` currently folds `vndfWeight`
  (the Monte-Carlo weight of its one VNDF sample) into the same scalar. That is correct for the
  radiance and wrong in principle for arbitration — a noisy per-frame sample weight should not
  decide which tier owns a pixel. It is left as-is here because SSR's weight is consumed only by
  SSR's own composite, where it means the right thing; a tier that ever needs to read *another*
  tier's confidence must split the two first.
- The tier debug view is part of the contract, not a nicety: a hierarchy whose selection cannot be
  seen per pixel is a hierarchy nobody can review.

## 8. Where the contract lives, so a reviewer can check it

| Piece | File |
|---|---|
| The weight algebra and the `Σ w = 1` identity | `Renderer/ReflectionTier.h` (`ComputeReflectionTierWeights`) |
| The ratchet that proves it, over a swept grid and the degenerate inputs | `tests/Rendering/ReflectionTierContractTest.cpp` |
| probe over IBL (`c_probe` in an alpha lane, already shipping) | `shaders/include/DeferredLightingShared.glsl` |
| The ray-query tier and its `c_ray` | `shaders/RayTracedReflection.glsl`, `Renderer/Passes/RayTracedReflectionPass.{h,cpp}` |
| SSR over the ray tier (its existing delta composite, unchanged) | `shaders/PostProcess_SSRComposite.glsl` |
| The frame order that *is* the tier order | `Renderer/RenderPipelineBuilderPost.cpp` |
| The tier debug view | `shaders/PostProcess_SSRComposite.glsl` (`OloReflectionTierDebugColor`) |

Three implementation notes worth stating because each one is a decision rather than a detail:

- **The ray tier costs no new binding.** The TLAS is a device address inside `UBO_RAY_TRACING` (65),
  shared with the shadow tier's and the probe's blocks the way #978 established. The three GPU Scene
  tables come from their canonical SSBO bindings (15/16/17) rather than by device address —
  deliberately, because those tables are refilled by a CPU `SetData` each frame and a draw resolving
  the *persistent* address would read the pre-snapshot records.
- **`c_ssr` reaches the debug view in the SSR guide plane's alpha**, a lane that was written on every
  path and never once sampled (the two spatial denoiser stages read only normal and roughness). No
  new attachment, and nothing in the production path reads it — evaluating bottom-up is precisely
  what means no tier *has* to.
- **`c_ray` reaches it in the alpha of the colour the tier hands downstream**, and only while the
  debug view is on, so no production frame carries a non-1.0 alpha down the chain.
