# Groom fibre scattering: the lobe comparison behind #1247

Issue #1247 requires that measured/reference fibre lobes be compared **before** an approximation is
chosen. This is that comparison. Everything here is produced by
`OloEngine/src/OloEngine/Groom/GroomFibreScattering.{h,cpp}`, and every claim is re-run on every
build by `OloEngine/tests/Rendering/PropertyTests/GroomFibrePropertyTests.cpp` — so the evidence
fails loudly rather than ageing quietly.

**Three candidate approximations were rejected by measurement**, and that is the part of this
document worth reading: each of them is the obvious implementation. §6c records a fourth rejection —
of a *measurement method* that produced a confident wrong answer.

---

## 1. The model, and what its parameters are

A hair fibre is a dielectric cylinder with an absorbing interior. The model is **Chiang et al.
2016**, the decomposition pbrt-v3 §"Hair Scattering" implements: four paths, each with a
longitudinal lobe `M_p`, an attenuation `A_p` and an azimuthal lobe `N_p`.

| path | what it is | why it matters |
|---|---|---|
| R | reflected off the cuticle without entering | uncoloured — it carries the *light's* colour, which is why black hair has a white sheen |
| TT | in one side, out the other | absorbed once; fires when the light is **behind**; the whole character of pale hair |
| TRT | in, off the far wall, out | absorbed twice; the secondary highlight, and what makes blonde hair glow |
| residual | every path with three or more internal bounces | folded into one closed-form term so the four attenuations sum to **exactly** one |

It was chosen over the cheaper closed-form fits (Karis' UE4 / Frostbite azimuthal approximations)
for two reasons that are properties rather than preferences: it has a matching
**evaluate / sample / pdf** triple, and its attenuations are energy-conserving **by construction**.

The authored parameters and their measured sources:

| parameter | meaning | source |
|---|---|---|
| eumelanin, pheomelanin | pigment concentrations against measured absorption spectra `(0.419, 0.697, 1.37)` and `(0.187, 0.4, 1.05)` | d'Eon et al. 2011, table 1 |
| base colour | the reflectance the coat should end up with, inverted to an absorption by bisection against this renderer's own albedo (§6b) | — |
| β_M, β_N | longitudinal and azimuthal roughness | Chiang et al. 2016 eq. 7 |
| α | cuticle scale tilt, degrees; default 2° | Marschner et al. 2003, measured 2–3° |
| η | index of refraction; default 1.55 | keratin |

**Lobe width ratios are not authored**: `v_TT = v_R / 4` and `v_TRT = 4 v_R` are Marschner's
measured ratios, and they are derived rather than exposed so an author cannot break the measured
relationship between the two highlights.

### Far field, not near field, and that is forced by #1246

The near-field model takes `h`, the ray's offset across the fibre in [-1, 1]. A strand here is
rasterised as a **one-pixel-wide ribbon whatever its true width**
([groom-strand-visibility.md](../agent-rules/groom-strand-visibility.md) rule 2), so the
across-ribbon coordinate is a coordinate on the *widened quad* and is not `h`. Integrating `h` out
is the only honest reading of the geometry — and it buys a second thing: the far-field response
depends on the azimuth **difference** alone, so it needs the strand tangent and **no binormal**.
A screen-facing ribbon has no meaningful binormal, so a model that needed one would have to invent
it per frame and it would swim as the camera turned.

---

## 2. REJECTED: the plain h-quadrature

The obvious far-field implementation averages N point samples of the near field over `h`. Measured
against the converged far field (N = 256), on a 17 629-point grid of `(θ_i, θ_o, φ)` triples at
β_M = β_N = 0.3:

**Table 1 — relative RMS and max error vs the converged far field**

| fibre | N | point sampled RMS | point sampled max | node widened RMS | node widened max |
|---|---|---|---|---|---|
| dark  | 1  | 2.186 | 88.39 | 0.297 | 3.99 |
| dark  | 4  | **0.928** | **23.15** | 0.178 | 2.69 |
| dark  | 8  | 0.423 | 10.95 | 0.138 | 2.27 |
| dark  | 32 | 0.064 | 1.47  | 0.053 | 1.16 |
| brown | 4  | **0.797** | **21.17** | 0.156 | 2.32 |
| brown | 8  | 0.346 | 8.76  | 0.114 | 1.81 |
| brown | 32 | 0.051 | 1.17  | 0.043 | 0.93 |
| pale  | 4  | **0.542** | **12.13** | 0.135 | 1.59 |
| pale  | 8  | 0.179 | 3.60  | 0.073 | 1.21 |
| pale  | 32 | 0.023 | 0.50  | 0.019 | 0.40 |
| red   | 4  | **0.750** | **20.25** | 0.151 | 2.17 |
| red   | 8  | 0.317 | 7.94  | 0.106 | 1.73 |
| red   | 32 | 0.047 | 1.06  | 0.039 | 0.84 |

Errors are relative to the reference's own RMS over the grid, so "0.928" means the error is 93 % of
the signal.

**At N = 4 the plain rule is 54–93 % RMS away from the truth and individual angles are wrong by 12
to 23 times the signal's RMS.** It converges — at N = 32 it is within 6 % — but N = 32 is 32 Fresnel
evaluations, 32 transmittance exponentials and 96 logistics per fragment per light. It is correct
only in a limit this does not run at.

### Why it fails, and what the error looks like

Each node contributes a narrow azimuthal logistic centred on that node's exit azimuth, and adjacent
nodes' exit azimuths are `|dΦ_p/dh| · Δh` apart. When that gap is wider than the logistic, the sum
is N separate **spikes** rather than one lobe. The RMS number understates it, because the artefact
is not error magnitude, it is *banding*: a coat rendered this way bands visibly as the strand
tangent turns.

**Table 2 — azimuthal total variation, relative to the reference's (1.0 = as smooth as the truth)**

| fibre | N | point sampled | node widened |
|---|---|---|---|
| dark  | 2  | 7.14 | 0.62 |
| dark  | 4  | **7.37** | 1.05 |
| dark  | 8  | 5.76 | 2.12 |
| brown | 4  | **2.57** | 1.17 |
| pale  | 4  | **1.58** | 1.14 |
| red   | 4  | **2.36** | 1.17 |

A response with seven times the reference's total variation is not a slightly noisy version of it.

---

## 3. ACCEPTED: the node-widened quadrature, N = 4

The fix follows from the diagnosis: widen each node's azimuthal lobe to at least cover its own slice
of `h`, so the rule becomes a piecewise **reconstruction** of the far field rather than a point
sample of it. The widening is analytic —

```
dΦ_p/dh = 2p · dγ_T/dh − 2 · dγ_O/dh,   s_eff = max(s, min(0.25 · |dΦ_p/dh| · Δh, π))
```

— and near |h| = 1 the derivative diverges, which is exactly where the fibre's caustic is, so a
large widening there is the correct answer rather than a failure of the bound.

What remains is a **known azimuthal blur**: a declared approximation with a measured size, where the
spikes were an artefact with none.

**N = 4 is the shipped default**, and it is the smoothness column that picks it rather than the RMS
column. RMS falls monotonically with N, but table 2's dark row shows the widened rule's total
variation is closest to the truth at N = 4 (1.046) and drifts *up* at N = 8 (2.120): past N = 4 the
widening no longer covers the gaps for the R-dominated dark fibre. N = 4 is simultaneously the
cheapest useful order and the smoothest. The order stays authorable (1–32) because a groom seen in
close-up may want more.

Cost is linear in N: 4 Fresnel evaluations, 4 transmittance exponentials and 12 logistics per
fragment per light. The longitudinal term — the expensive part, a ten-term Bessel series — does
**not** depend on `h` and is hoisted out of the loop entirely, evaluated once per lobe.

---

## 4. Reference predictions the model reproduces

These are the checks that the implementation is the published model rather than something with the
same shape. Each is a number the geometry predicts analytically.

**Table 3**

| prediction | expected | measured |
|---|---|---|
| R longitudinal peak, α = 2° | +2α = +4.0° | **+4.0°** |
| TRT longitudinal peak, α = 2° | −4α = −8.0° | **−8.3°** |
| TRT caustic azimuth, η = 1.55 | the ±glint pair | **±18.6°** from retroreflection, i.e. 37.2° apart |
| pale fibre, backlit TT/R | TT dominates | **181.8** |
| dark fibre, backlit TT/R | absorbed away | **0.85** |
| any fibre, frontlit TT/R | TT does not reach the eye from the front | **0.000** |

The last three rows are acceptance criterion 3 as a number: **what separates pale hair from dark
hair is not brightness, it is which path carries the light.** A pale fibre backlit is carried by
transmission at 182× its surface reflection; a dark fibre has absorbed that transmission away and is
left with the same white sheen it had from the front. A model that merely scaled brightness with
pigment would reproduce neither.

No goniophotometric measurement rig data is in this repository. "Reference" above means the
converged model plus the published measured constants it is parameterised by; the predictions in
table 3 are the checks that stand in for a measurement we cannot run here, and they are stated as
such rather than dressed up.

---

## 5. Energy

**Table 4 — albedo with σ_a = 0 (expect 1.0)**

| β_M | N = 1 | N = 4 | N = 8 |
|---|---|---|---|
| 0.1 | 0.9999 | 0.9999 | 0.9999 |
| 0.3 | 0.9997 | 0.9997 | 0.9997 |
| 0.6 | 0.9999 | 0.9999 | 0.9999 |
| 0.9 | 1.0000 | 1.0000 | 1.0000 |

Within 0.03 %, and the residual is the 512×512 angular quadrature's own error, not the model's.

### The Bessel branch that was costing 0.2 % of it

`log I0(x)` is evaluated as a series below x = 12 and an asymptotic expansion above. Transcribed
from pbrt-v3, the two branches are **1.50 % apart at the crossover** — the series 2.04 % low and the
asymptote 0.57 % low against a long-double reference — because pbrt writes the correction as
`0.5 * (… + 1/(8x))`, which halves a term that belongs outside that factor, and stops the series at
ten terms. The seam sits at `cos θ_i cos θ_o / v = 12`, which at the default roughness is inside the
angles a coat is actually shaded at.

Un-halving the term, adding the next one (`9/(128x²)`) and extending the series to fourteen terms
puts both branches within **0.005 %** of the reference and the jump at **0.0002 %**. The crossover
stays at 12 because that is where the two error curves cross. Table 4's β_M = 0.3 row moved from
0.9978 to 0.9997 as a result — the discontinuity was showing up as lost energy.

**Energy conservation does not depend on N**, and that is structural rather than lucky: the four
attenuations sum to one at every `h`, and the quadrature is a convex combination of values that each
already sum to one. Lowering the quality knob makes the lobes blurrier; it cannot make the fibre
brighter.

The sampling triple is checked the same way but by a different road: `GroomFibreSampledFurnace`
draws directions through `GroomFibreSample` and averages value/pdf, reaching 1.0000 across every
roughness pair tested. The two roads agree only if evaluate, sample **and** pdf all agree — which is
why the triple exists in a slice with no stochastic consumer. #1248's dense-coat transport is a
stochastic estimator, and an independently written density there would converge beautifully to the
wrong image.

---

## 6. REJECTED: sampling the environment around the fibre's normal circle

The environment term needs the fibre's response to a non-punctual source. The obvious idea — sample
the environment at K directions spaced around the circle perpendicular to the strand, where the
fibre scatters most — was measured against the true spherical integral first.

**Table 5 — ratio to the true integral**

| fibre | sin θ_o | circle rule, K = 4 | mean attenuation × cos θ_o |
|---|---|---|---|
| dark   | 0.00 | 2.199 | 0.790 |
| dark   | 0.85 | 0.037 | 0.715 |
| brown  | 0.00 | 4.670 | 0.963 |
| brown  | 0.85 | 0.031 | 0.829 |
| pale   | 0.00 | 5.748 | 1.022 |
| pale   | 0.85 | 0.108 | 0.969 |
| smooth | 0.00 | **11.253** | 0.940 |
| smooth | 0.60 | **0.000** | 0.915 |
| smooth | 0.85 | **0.000** | 0.863 |
| rough  | 0.85 | 0.936 | 0.643 |

The circle rule spans **0.00 to 11.25** of the truth. The zeros are the damning entries: at a smooth
fibre's grazing angles the scattering cone has tilted away from the perpendicular plane entirely, so
the circle misses it and the coat's ambient goes to *exactly zero*. That is a silent, total loss of
the environment term on precisely the fibres and angles hair is usually seen at.

**What ships instead** is the mean attenuation over the fibre's width, times cos θ_o. It is each
path's albedo — because the longitudinal and azimuthal factors are each normalised to one,
integrating the BCSDF over the sphere leaves exactly the sum of the attenuations — and the cosine is
the extra factor of cos θ_i the true integral carries, evaluated at its mean value on the scattering
cone. Measured band: **[0.643, 1.112]**, mean 0.913, across dark, pale, coloured, smooth and rough
fibres at four view angles.

It also costs nothing: the quadrature loop already computes every attenuation it averages. And it
uses **the same attenuations the direct lighting uses**, which makes acceptance criterion 4's
"scene lights and environment lighting use consistent material parameters" an identity rather than a
promise.

The declared approximation is that the environment is **uniform over the directions the fibre
scatters from**. A coat under a strongly directional sky will not pick that direction up from its
ambient term.

---

## 6b. REJECTED: Chiang's colour inversion, used directly

The `BaseColor` mode needs an absorption that reproduces an authored colour. Chiang et al. 2016
eq. 9 is the published inversion, and using it directly is wrong **here**:

| authored colour | rendered albedo via the published fit | via bisection |
|---|---|---|
| 0.1 | **0.758** | 0.100 |
| 0.3 | **0.926** | 0.300 |
| 0.6 | **0.986** | 0.600 |

The fit answers *"what absorption makes a fibre **assembly** look like this colour"* — it has
inter-fibre multiple scattering baked into it, which is exactly why it is the right constant for a
production path tracer. Nothing in this slice transports light between fibres (#1248 owns that), so
the absorption it asks for is far too low: it expects the neighbours to do the darkening and there
are no neighbours. A coat authored at 0.1 renders at 0.76.

**What ships** is a bisection against the model's own head-on albedo — twenty iterations over a
monotone function, once per groom, not per fragment. Authored colour is rendered colour to within
0.01.

The published fit stays callable (`GroomFibreSigmaAFromColor`), because it becomes the correct
answer the moment #1248's density transport exists, and because
`ChiangsAssemblyFitIsRejectedForASingleFibre` measures the gap rather than describing it.

**A fibre cannot be darker than its own surface reflection.** R never enters the fibre, so no
absorption removes it; an authored colour below that floor (~0.04 at these indices) saturates at the
maximum absorption. That is a fact about hair — it is why black hair still has a white sheen — not a
limitation of the solver.

---

## 6c. A measurement trap this work fell into: tone-mapped ratios

Worth recording because it produced a confident, wrong conclusion before it was caught.

The pale-versus-dark criterion was first tested as *"the pale-to-dark ratio of mean coat luminance
is larger backlit than frontal"*. Measured, it is smaller:

| lighting | dark | pale | ratio |
|---|---|---|---|
| frontal | 0.0847 | 0.2748 | 3.25 |
| backlit | 0.2567 | 0.7345 | **2.86** |

The frame is **tone mapped**. Backlit, the pale coat sits at 0.73 on the curve's compressive
shoulder while the dark one is at 0.26 on the near-linear part, so the *displayed* ratio contracts
exactly where the *radiance* ratio expands. A ratio of tone-mapped means is not a ratio of
radiances, and the physical claim is about radiance.

The claim is now measured where it lives — **the TT lobe, through the material's own diagnostic
view**. Same geometry, same light, same exposure; only the pigment moves and only transmission is
rendered. That is both a sharper test and a use for the separated contributions the issue asked
for.

**The general rule: do not compare two tone-mapped means whose values sit on different parts of the
curve.** Compare a single lobe, or compare at matched brightness.

---

## 7. Declared approximations, in one place

1. **Far field** — `h` is integrated out. Forced by #1246's one-pixel ribbon; see §1.
2. **N = 4 node-widened quadrature** — the far-field lobe is reproduced to 0.10–0.18 relative RMS,
   with an azimuthal blur in place of the authored roughness at the nodes' scale. §3.
3. **Uniform environment** — §6, band [0.64, 1.11].
4. **No multiple scattering between fibres at all.** #1248 owns density transport. The BaseColor
   mode therefore inverts against the SINGLE-FIBRE albedo (§6b), so an authored colour is the colour
   that renders today — rather than through Chiang's assembly fit, which assumes neighbours this
   slice does not have.
5. **One material per groom entity, not per group.** A cooked groom carries groups (a scalp and its
   eyebrows are different groups) and a production shader would give each its own material. Omitted
   deliberately: per-group materials need a per-group draw split in `GroomRenderPass`, which is a
   change to the visibility slice's geometry cache and not to its shading.
6. **No shadowing.** Inter-fibre occlusion is #1248's. A coat lit here is lit as if every strand were
   alone.

---

## Where the evidence lives

- **The comparison, re-run by CI:** `OloEngine/tests/Rendering/PropertyTests/GroomFibrePropertyTests.cpp`.
  Both quadrature rules stay callable so the rejection in §2 is re-run on every build.
- **The CPU/GPU twin:** `OloEngine/tests/Rendering/PropertyTests/GroomFibreGpuParityTest.cpp` renders
  `OloEditor/assets/shaders/tests/GroomFibreParityProbe.glsl` and compares every texel against the
  C++ model at a 2×10⁻³ relative tolerance.
- **The pixels:** `OloEngine/tests/Rendering/PropertyTests/GroomFibreVisualEvidenceTest.cpp` writes
  `OloEditor/assets/tests/visual/GroomFibre_GL_<Path>[_<Angle|Fibre|Lobe>].png` and the
  `GroomFibreOff_*` controls.
- **The rules a future change must not break:**
  [groom-fibre-scattering.md](../agent-rules/groom-fibre-scattering.md).
