# Skin: layered specular, pore filtering and expression detail

Transport version 3 (`SkinEvaluationModel::LayeredSpecular`), issue #1243. It adds three things to
a skin profile and touches the **specular half only** — the diffusion of version 1 and the
transmission of version 2 are underneath it and cannot see it, because #1231 split the two.

- a **convex mixture of two GGX lobes** in place of the single one;
- a **roughness filtered by the screen-space variance** of the shading normal;
- a **pore band whose strength follows the face's expression**.

Maths and every physical decision: [`Renderer/SkinLayeredSpecular.h`](../../OloEngine/src/OloEngine/Renderer/SkinLayeredSpecular.h).
Shader side: [`include/SkinLayeredSpecular.glsl`](../../OloEditor/assets/shaders/include/SkinLayeredSpecular.glsl).

## Authoring

Five fields, in the `.oloskin`'s `Specular` block. All five are read **only** at transport
version 3; a profile stays where its author left it.

| field | default | what it does |
|---|---|---|
| `LobeMix` | `0.0` | `w` — fraction of the specular carried by the broad lobe. 0 is the single-lobe answer, bit-identical to version 2. |
| `LobeRoughnessScale` | `3.0` | `s` — broad-lobe **roughness** = narrow × s. Only consulted when `LobeMix > 0`. |
| `NormalVarianceStrength` | `0.5` | `sigma^2` — how much measured normal variance reaches the roughness. **0 turns the pore filter off.** |
| `DetailStrength` | `0.0` | extra pore-band gain at a neutral expression. 0 leaves the normal map untouched; **-1 removes the pore band entirely.** |
| `ExpressionDetailGain` | `0.0` | extra pore-band gain at a fully expressed face, added to `DetailStrength`. |

The two defaults have **opposite polarity on purpose**. `LobeMix` and the detail fields are effects
an author opts into, so they default to neutral. `NormalVarianceStrength` is a *correction*: a
version-3 profile that filtered nothing would ship the sparkle the version exists to remove, so
turning it **off** is the deliberate act.

`LobeRoughnessScale` scales **perceptual roughness, not alpha.** They differ by a square, and the
experiment's `broad` column reports an **alpha** ratio — take its square root before typing it into
this field. A fitted alpha ratio of 1.3 is a roughness scale of 1.14, not 1.3, and typing 1.3 gives
a lobe two thirds wider than the one that was measured.

## The measured comparison

`experiments/skin-specular-reference/compare_lobes.py` (~20 min; `--quick` for ~10 s, which
reproduces every conclusion below at coarser resolution). It builds a patch of skin micro-geometry —
furrows as creases, pores as pits, a patchy lipid film — far below the pixel, integrates the **true**
aggregate response of a pixel footprint over it, and scores candidate models against that. A lobe
mixture is a *result* there, never an input.

Relative RMS of each model's ensemble response against the integrated truth:

| footprint | fixed | mip | toksvig | kaplanyan | kaplanyan, σ² refit | fitted σ² | 2-lobe fit |
|---|---|---|---|---|---|---|---|
| 16 µm  | 10.2%  | 0.2%   | 2.7%  | 13.4% | 3.6%  | 0.05 | 0.4% |
| 31 µm  | 10.4%  | 0.9%   | 8.4%  | 27.5% | 9.7%  | 0.05 | 1.4% |
| 62 µm  | 11.3%  | 4.1%   | 21.1% | 51.8% | 22.8% | 0.05 | 2.4% |
| 125 µm | 11.0%  | 8.9%   | 50.1% | 71.7% | 31.6% | 0.05 | 6.2% |
| 250 µm | 54.8%  | 56.5%  | 73.3% | 46.6% | 10.1% | 0.15 | 5.4% |
| 500 µm | 138.0% | 138.9% | 82.6% | 23.2% | 11.3% | 0.70 | 7.7% |

*fixed* = one authored roughness for every distance, tuned at the finest footprint — what the engine
shaded before. *mip* = the footprint's own averaged roughness. *toksvig* = the ideal variance
estimator. *kaplanyan* = the one that ships, at the published default strength. Four results drove
the design:

1. **A fixed authored roughness fails at distance, and filtering is what fixes it.** It holds around
   10–11% out to a 125 µm footprint and then collapses: 55% at 250 µm, 138% at 500 µm. The shipping
   filter takes that 500 µm figure to 23%. This is the structural win and it needs no fitting.

2. **A roughness mip is not a filter.** Averaging the roughness map and stopping there tracks the
   fixed value almost exactly at the coarse end (139% vs 138%): it pulls roughness toward the smooth
   regions while the normal variance that should have raised it is discarded. Half of this filter is
   not optional.

3. **The second lobe is a real improvement, and the comparison is not free.** The fitted mixture
   scores 0.4–7.7% across the whole range against the estimator's 13–72%. But the mixture is a
   **three-parameter fit** (both lobe widths and the weight) and the estimator has none, so that gap
   is an *upper bound* on what layering buys, not a like-for-like score. What the fit says without
   ambiguity is *where* the lobe earns its place: its weight runs 0.05–0.10 at the finest footprints
   and 0.55–0.60 at the coarsest — that is, once a pixel straddles regions of *different* roughness
   (an oily plateau beside a dry cheek). That is the claim "layered specular" makes, and it is why
   `LobeMix` defaults to 0.

4. **One variance strength cannot serve every distance.** The fitted `sigma^2` runs 0.05 at every
   fine footprint and then 0.15 and 0.70 — a factor of fourteen — and refitting it moves the
   estimator from 13–72% to 4–32%, most of the gap to the ideal. Hence an authored field, not a
   `#define`.

### Sparkle

Camera pushed in over a shrinking, drifting footprint at a grazing view; mean frame-to-frame change
relative to the mean response. The **truth** row is real change in the surface under the pixel, so
it is the floor — no model should be asked to beat it.

| model | change | vs floor |
|---|---|---|
| point sample | 62.6% | 2.3× |
| mip only | 34.3% | 1.3× |
| toksvig | 30.5% | 1.1× |
| **kaplanyan (ships)** | **26.2%** | **1.0×** |
| truth | 26.9% | — |

The shipping filter lands *on* the floor — a hair under it, i.e. very slightly over-smoothing, which
is the safe side. That ratio is the issue's second acceptance criterion as a number.

## Why the variance is measured in screen space

The textbook answer is Toksvig: mip the normal map without renormalizing and read the variance off
`|N|`. It is the better estimator over most of the range — 8% against 28% at 31 µm — and **this
engine cannot use it.** (At the coarsest footprints the screen-space estimator wins instead, because
it also sees the geometric curvature that dominates there, which Toksvig cannot.)

`decodeTangentNormal` in `include/PBRCommon.glsl` reconstructs `z = sqrt(1 - x² - y²)` from the
sampled `xy` instead of sampling blue. It has to: a two-channel BC5 normal map has no blue channel
and sampling it inverts the normal (#440). Reconstruction **renormalizes**. Whatever the mip chain
averaged, what leaves that function is unit length at every mip, so the Toksvig factor is 1.0
always — a filter that compiles, runs, costs ALU and does nothing.

That trap is worth naming because it is invisible: the code looks right, the maths is right, and the
answer is identically the unfiltered one. Reaching for Toksvig here means changing the decode first,
which changes every normal-mapped surface in the engine.

So the estimator is Tokuyoshi & Kaplanyan's: the variance of the **final** shading normal from its
screen-space derivatives. Its weakness is measured, not hidden — a screen-space estimator sees
variation *between* pixels, and the quantity that belongs in the roughness is the variation *within*
one. Those coincide only near one texel per pixel; magnified past that (a close-up head, this
feature's whole subject) it over-widens, and shrunk past it, it under-corrects. At the published
default strength it misses the true hemispherical energy by up to 18% in **both** directions across
the range (0.82× at 125 µm, 1.17× at 500 µm). That is what `NormalVarianceStrength` is for.

## Energy

The mixture is **convex**, not additive:

```
specular = narrow + w * (broad - narrow)
```

Its directional albedo is a convex combination of two single-lobe albedos, so it is bounded above by
the larger of them for every `w` in [0, 1]. It cannot exceed what *one* lobe of either width would
have returned, whatever the author sets — a property of the expression, not of a parameter range.
`SkinSpecularMix` is the only place that expression is written, on either side.

At `w = 0` it returns the narrow lobe **exactly**, so "layering off" is the pre-#1243 frame bit for
bit and a golden image can assert against it.

## The expression detail, and its history

The detail weight is derived from `MorphTargetComponent::AppliedWeights` — the weights the surface
**currently on the GPU** was built from — as the clamped sum of their magnitudes.

Not from `Weights`, and that is the whole of the issue's third criterion. `Weights` is what the
surface *will* be built from next time the morph pass runs. Reading it would make the shading lead
the geometry by a frame, and in that frame the surface would shade differently with **no
deformation-history rejection behind it** — `Scene::OnUpdateRuntime` rejects history when
`AppliedWeights` moves, not when `Weights` does. A temporal upscaler would reproject a detail change
that never happened: smearing on a face, and nothing at all in a still.

Deriving it from `AppliedWeights` makes the correctness *inherited* rather than re-implemented. The
weight is a pure function of them, so it can only change when they change, and when they change the
history has already been rejected.

### What it is not

One scalar for the whole surface. It deepens the pores the normal map already has, **everywhere**,
in proportion to how far from neutral the face is. It does not know that a brow furrows while a
cheek does not — per-region wrinkle response needs a mask per morph target, which is a facial-rig
authoring tool. Issue #1243's scope boundary excludes that in those words; #1245 owns it.

## Limits, stated

- **The pore band comes out of the normal map itself**, as the difference between the fragment's mip
  and the same map two mips coarser — not from a second detail texture. It cannot desync from the
  base normal, costs one extra tap of a texture already resident, and modulates the detail the
  artist authored. A dedicated wrinkle map is not in this version.
- **The lobe mixture applies to punctual and clustered lights, not to IBL.** Layering the
  image-based specular costs a second prefiltered cubemap fetch for the 1–2 points the second lobe
  buys; the filtered roughness *does* flow into IBL, so the large win is already there.
- **Sphere-area lights get one lobe.** Their representative-point evaluator splits the BRDF
  differently, so evaluating it twice and mixing has no physical reading.
- **GPU-Scene batched draws take the lane from the per-draw UBO**, like the #1242 thickness before
  it: for a single-material draw they are the same numbers; a batch of two skin materials with
  different lobes would use the UBO's.

## Where the code is

| thing | file |
|---|---|
| The maths, the energy argument, the measured numbers | `Renderer/SkinLayeredSpecular.h` |
| The authored fields and their bounds | `Renderer/SkinProfile.h` |
| The shader transcription | `include/SkinLayeredSpecular.glsl` |
| The reference comparison | `experiments/skin-specular-reference/compare_lobes.py` |
| CPU maths tests | `tests/SkinLayeredSpecularTest.cpp` |
| GLSL-is-the-CPU-maths tests | `tests/Rendering/PropertyTests/SkinLayeredSpecularParityTest.cpp` |
