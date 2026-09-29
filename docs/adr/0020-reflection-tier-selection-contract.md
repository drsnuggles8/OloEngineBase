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
tier is pinned at `c = 1`. There is no confidence assignment, valid or absurd, that makes the
tiers' weights sum to more or less than one, and it is cheap to test on the CPU with no GPU at all,
which is what `ReflectionTierContractTest` does. The identity makes the tiers' estimates of `R` a
convex combination. It is a no-double-count guarantee only together with §1a: each tier applies it
to the indirect specular term `S = W · R` and hands `S` on. It is not an energy-conservation proof
on its own (§9).

### 1a. The algebra is over R, and the frame is not R (issue #1325)

The frame colour is `C = D + E + S_direct + S`: diffuse, emission, direct specular, and the
**indirect specular term** `S = W · R`, where `W` is the lobe's BRDF weight per unit of incident
radiance — the split-sum `(F·A + B)`, times the ambient occlusion and the skin profile's specular
tint, exactly as the lighting pass composed it. The "over" above is applied to `R`, so a tier
replaces `S` and nothing else:

    S' = S + c_t · (W · L_t − S)          C' = C + (S' − S)

`S` is the term the colour holds when the tier runs: the lighting pass's own for the first tier,
the previous tier's `S'` for the next. Chaining two tiers this way is the "over" on `R`, scaled by
`W` (`ChainedTiersAreTheOverOnTheLobeRadianceTimesTheWeight`). **Fresnel lives in `W`, never in
`c_t`**: confidence says how much of the lobe an estimate may answer for, and `W` says how much light
the lobe reflects. A black hit removes `c_t · S` and leaves `D`, `E` and `S_direct` alone.

Until #1325 both upper tiers applied `mix` to `C` itself, which scaled every other term by
`1 − c_t`. `DeferredLightingPass` now writes `S` and `W` to `IndirectSpecular` with a second draw of
the same shading body (`DeferredIndirectSpecular{,_MSAA}.glsl`), declared only when a tier runs.

## 2. Why bottom-up is the load-bearing decision

The obvious reading of "planar beats SSR beats rays beats probes" is top-down: let the best tier
claim its share and hand the *residual* down. That is the same algebra, and it is much worse to
build, because it forces every tier to know the confidence of every tier above it. Concretely: SSR's
confidence is a per-pixel value computed inside its trace, currently baked into the delta it writes.
A ray-query tier that had to claim `1 - c_ssr` would need that value transported through SSR's
five-stage denoiser chain (#708) — whose only spare lane, the signal's alpha, already carries view
depth on every path including the early-outs, deliberately.

Evaluated bottom-up, **no tier needs to know anything about the tiers above it.** Each one replaces
the specular term in whatever is already there (§1a). The ordering is expressed purely by *where a
tier sits in the frame*, not by data it has to be handed. That is why the ray tier could be inserted
below SSR without transporting a confidence through the denoiser chain. (#1325 later edited SSR's
shaders to replace `S` instead of the colour; no confidence travels through the chain.)

This is not a trick; it is the same reason back-to-front alpha compositing needs no per-layer
bookkeeping.

## 3. This is already how two of the boundaries work

The contract is largely a *statement* of existing behaviour, which is the main reason to trust it.

**probe over IBL** — `include/DeferredLightingShared.glsl` already does exactly
`mix(globalPrefilter, probeSpecular.rgb, probeSpecular.a)`. `oloSampleReflectionProbes` returns its
confidence in alpha. That is `c_probe`, already named and already used.

**SSR over what is below it** — `PostProcess_SSR.glsl` writes the signed delta of the specular
term, `c_ssr · (W · L_ssr − S)`, and `PostProcess_SSRComposite.glsl` adds it (§1a). Before #1325
the delta was `(reflTarget − baseColor) · blend`, the same "over" applied to the whole colour.
Adding instead of replacing *"double-counts and washes out (sky + object)"*, in SSR's own words.

So the hierarchy's bottom three boundaries were already contract-shaped. What was missing was the
statement, a tier between SSR and the probes, and a way to see which tier answered.

## 4. Where the ray-query tier goes, and what it may shade

**Position: below SSR, above the probes.** It fills exactly the gap SSR cannot cover — off-screen
and occluded hits — and it composites over the probe/IBL result that `DeferredLighting` produced.
Because SSR then replaces the term this tier hands on, by `c_ssr`, a pixel where SSR is confident is unchanged,
and a pixel where SSR fades out at the screen edge lands on a ray-traced answer instead of dropping
to a low-frequency probe. That edge drop is the seam this issue exists to remove.

    c_ray = hitValid · roughnessGate · rayTracingAvailable

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

> **State at `7c5aa1b98` (#1357).** The heap boundary above moved. Masked candidates are now
> alpha-tested through the descriptor heap (`include/HybridRayTracingAlpha.glsl`, the helper the
> shadow tier uses), and the tier stands down with `GPUSceneUnavailable` when the material heap is
> unresolved. Hits are still shaded from untextured factors, counted as
> `ReflectionTierStats::HitsShadedUntextured`; textured hit shading with a ray-footprint policy is
> #1355, not #805.

## 5. Fallback is loud and countable

There is no silent degradation (`docs/agent-rules/no-silent-fallbacks.md`). When ray tracing is
unavailable — a no-RT device, `OLO_VULKAN_NO_RAY_TRACING=1`, an empty TLAS, or the GL backend —
`c_ray` is pinned to 0 for every pixel. The algebra then collapses to exactly the
planar → SSR → probe/IBL hierarchy, because a zero confidence makes the delta of §1a zero.

That is the *reason* the raster-only output is byte-identical when the tier is off: it is not a
tested coincidence, it is `C + 0 · (…) == C`, and the tier hands `S` on unchanged. The pass reports the reason it stood down through a
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

**Ownership rule for planar (#1325).** Planar reflection has one consumer, `Water.glsl`, and there it
replaces only the water's own reflection term — `mix(cubemap, ssr, a)` then `mix(…, planar, a)`
inside the reflection, which the Fresnel then weights — never the water's lit colour. A deferred
planar resolve, if one lands, is a tier like the others: it replaces `S` by §1a and nothing else.

## 7. Consequences

- The composite order is now load-bearing and belongs in the render graph, not in a shader's head:
  the ray tier must run after `RayTracingScenePass` (by-name dependency) and before `SSRRenderPass`.
- A new tier is added by composing `C + c · (W · L − S)` at the right depth, defining its
  confidence, and handing its `S'` to the next tier (§1a). It cannot double-count as long as the
  bottom tier stays pinned at 1, it replaces only `S`, and Fresnel stays in `W`.
- **The bottom tier must never report a confidence below 1.** That is the single invariant the whole
  guarantee rests on; a probe/IBL tier that "admits it doesn't know" would leave energy unclaimed
  and darken the frame. Uncertainty at the bottom is expressed by widening the lobe, never by
  lowering the weight.
- Confidence is *arbitration*, not an estimator weight. Since #1325 neither tier folds a weight into
  it: Fresnel and the lobe's directional albedo are in `W`, and SSR's `vndfWeight` (`G2/G1`, whose
  expectation over the VNDF `W` already is) only rejects a sample below the horizon.
- `S` and `W` belong to the G-Buffer surface. A pixel a forward overlay (a transparent, a particle)
  drew over after the lighting pass still has the surface's `S` under it, as the tiers' normal and
  depth inputs always have; the clamp at zero bounds what that can subtract.
- The tier debug view is part of the contract, not a nicety: a hierarchy whose selection cannot be
  seen per pixel is a hierarchy nobody can review.

## 8. Where the contract lives, so a reviewer can check it

| Piece | File |
|---|---|
| The weight algebra and the `Σ w = 1` identity | `Renderer/ReflectionTier.h` (`ComputeReflectionTierWeights`) |
| The ratchet that proves it, over a swept grid and the degenerate inputs | `tests/Rendering/ReflectionTierContractTest.cpp` |
| probe over IBL (`c_probe` in an alpha lane, already shipping) | `shaders/include/DeferredLightingShared.glsl` |
| The ray-query tier and its `c_ray` | `shaders/RayTracedReflection.glsl`, `Renderer/Passes/RayTracedReflectionPass.{h,cpp}` |
| SSR over the ray tier (the delta composite) | `shaders/PostProcess_SSR.glsl`, `shaders/PostProcess_SSRComposite.glsl` |
| The term a tier replaces, and its twin (§1a) | `shaders/include/ReflectionTierComposite.glsl`, `Renderer/ReflectionTier.h` (`ComposeSpecularTier`) |
| `S` and `W`, exported from the lighting body | `shaders/include/DeferredLightingShared.glsl`, `shaders/DeferredIndirectSpecular{,_MSAA}.glsl` |
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

## 9. Review correction: convex weights are not an energy proof (2026-09-29, #1357)

**Rule: a set of weights that sums to one proves a convex combination of its operands, and nothing
about energy unless every operand is the same physical quantity.** §1 as first written called
`Σ w_t = 1` "the whole no-double-count guarantee" and "energy conservation as a ratchet". The weights
did telescope, but the shaders applied the "over" to the whole lit colour `C = D + E + S_direct + S`,
not to `R`: the ray tier computed `mix(baseColor, reflTarget, c)` with Fresnel folded into `c`, and
SSR added `(reflTarget − baseColor) · blend`. Diffuse, emission and direct light were therefore scaled
by `1 − c`, and the operands were in different units, since `S` already carried the split-sum weight
`W`. #1325 (commit `2012abe67`, PR #1517) measured the old SSR removing up to 1.12 × `S` on the band
and up to 77 % of a pixel's colour beyond it.

What holds at `7c5aa1b98` is three pieces together: the weight identity on `R`, the term replacement
`C' = C + c · (W · L − S)` with Fresnel only in `W` (`include/ReflectionTierComposite.glsl`, CPU twin
`ComposeSpecularTier`), and `S` handed from tier to tier (`RTReflectionSpecular` into SSR). That is a
no-double-count and no-attenuation guarantee. It is still not an energy-conservation proof of the lobe
itself, which depends on `W`, the BRDF split owned by the #1336 lighting-signal contract, and the
`max(·, 0)` clamps.

Pinned by `ReflectionTierContractTest.ABlackReflectedHitRemovesTheSpecularTermAndNothingElse` (with a
whole-colour negative control), `…ConfidenceZeroIsACopyAndOneIsAFullReplacement`,
`…ChainedTiersAreTheOverOnTheLobeRadianceTimesTheWeight`,
`ScreenSpaceReflection.DeltaCompositeIsTheSpecularTierHandOff`,
`ReflectionTierTermEvidenceTest.SSRReplacesTheSpecularTermAndLeavesTheRestOfTheColour` (GL deferred,
SSR) and `LightingSignalContract.*`. **Unexecuted:** no automated test runs the ray tier's GPU
composite in `RayTracedReflection.glsl`; PR #1517 checked it live on Vulkan only.
