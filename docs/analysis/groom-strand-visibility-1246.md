# Groom strand visibility: the composition comparison, and what it selected (#1246)

Issue [#1246](https://github.com/drsnuggles8/OloEngineBase/issues/1246) asks for a **bounded
comparison of viable coverage/composition approaches on measured evidence — near silhouette, dense
overlap, sub-pixel coverage, depth/composition cost and memory — before an approach is recorded as
selected.** This document is that comparison and that record.

**The decision: stochastic (hashed) alpha, with an opaque alpha-cutoff ribbon as the declared
fallback whenever no temporal resolve is running.** Alpha-to-coverage and weighted-blended OIT were
both measured and both rejected, each for a specific reason below.

## The measurement, and why it is arithmetic rather than screenshots

Every approach here produces a plausible picture of hair. The differences live in the *fraction of a
pixel* a strand covers, which no screenshot settles. So the comparison is computed against a ground
truth: the same strand geometry rasterised at **16×16 samples per pixel** and box-filtered, which is
the analytic answer to "what fraction of this pixel is strand" to within 1/256.

It lives in `OloEngine/src/OloEngine/Groom/GroomCoverage.{h,cpp}` and is driven by
`OloEngine/tests/Rendering/PropertyTests/GroomCoveragePropertyTests.cpp`. **Every number below is
re-derived by that test on every CI run**, and each claim this document makes is one of its
assertions — so the evidence behind the decision fails loudly if it stops being true, rather than
ageing quietly in a PR body.

The model is CPU-only and deterministic (fixed-seed integer hashes, IEEE-754 throughout, no
`<random>`), so these are **not machine-dependent measurements**: the same numbers come out on any
host. That is a stronger property than a timing, and it is why the quality comparison is here and
only the *cost* measurements below are attributed to hardware.

**What the model is a model of.** A curve segment is a tapered band with **square ends** — one quad,
exactly what `GroomStrandMesh` emits and what `GroomStrand.glsl` widens — and a strand narrower than
one pixel is widened to one pixel with its alpha scaled by how much it was widened. That widening is
the whole mechanism of sub-pixel coverage, and every mode below is a different answer to "now turn
that alpha back into coverage".

> An earlier revision modelled the band as a round-capped capsule. The caps are ~0.79 px² each
> *regardless of how thin the strand is*, because the rasterised half width has a half-pixel floor —
> so on a 17 500-segment sub-pixel case the model reported about a fifth of the silhouette as
> coverage of a shape the GPU never draws. The failing test that exposed it is the reason the
> stochastic mode's silhouette area now reads 99–100 % instead of 105–120 %.

## The one fact that drives everything else

**Real hair is sub-pixel at every framing a person would actually use.** A human hair is 17–180 µm;
the reference grooms use 70 µm. Projected:

| case | camera | resolution | mean projected half-width |
|---|---|---|---|
| scalp, filling the frame | 0.30 m | 480×270 | **0.027 px** |
| scalp, mid-shot | 1.0 m | 960×540 | **0.013 px** |
| scalp, full figure | 2.5 m | 480×270 | **0.003 px** |
| animal pelt (110 µm guard hair) | 0.30 m | 480×270 | **0.040 px** |

A strand only reaches one pixel of *width* at roughly 6 cm from the camera at 720p — inside the
skull. So "how does this approach behave below one pixel" is not an edge case to handle; it is the
entire problem, and an approach that degrades there has no regime left in which it is the right
choice.

## Results

Coverage error is against the 16×16 reference, over the pixels either the mode or the reference
touches. `bias` is the *signed* mean — it separates a mode that is noisy-but-unbiased from one that
systematically drops coverage, which `mean |e|` cannot. `area %` is the reproduced silhouette area
as a fraction of the truth. `extra` is the additional render-target memory at that resolution.

### Near silhouette — scalp at 0.30 m, 480×270, 17 278 segments

| mode | mean \|e\| | rmse | max \|e\| | bias | area % | extra |
|---|---|---|---|---|---|---|
| Opaque ribbon (cutoff 0.5) | 0.177 | 0.226 | 0.828 | −0.177 | 0.0 % | — |
| Alpha-to-coverage ×2 | 0.176 | 0.225 | 0.828 | −0.173 | 2.2 % | 4.9 MiB |
| Alpha-to-coverage ×4 | 0.160 | 0.210 | 0.785 | −0.152 | 14.0 % | 14.8 MiB |
| Alpha-to-coverage ×8 | 0.101 | 0.144 | 0.703 | −0.091 | 48.6 % | 34.6 MiB |
| Stochastic alpha, 1 frame | 0.252 | 0.354 | 1.000 | −0.002 | 99.0 % | — |
| **Stochastic alpha, 8 frames** | **0.096** | **0.130** | 0.742 | **−0.000** | **99.9 %** | **—** |
| Weighted-blended OIT | 0.023 | 0.032 | 0.253 | +0.000 | 100.1 % | 1.5 MiB |

### Dense overlap — animal pelt at 0.30 m, 480×270, 24 000 segments

| mode | mean \|e\| | rmse | max \|e\| | bias | area % |
|---|---|---|---|---|---|
| Opaque ribbon | 0.164 | 0.197 | 0.895 | −0.164 | 0.0 % |
| Alpha-to-coverage ×2 | 0.164 | 0.197 | 0.895 | −0.164 | 0.0 % |
| Alpha-to-coverage ×4 | 0.112 | 0.141 | 0.691 | −0.090 | 45.0 % |
| Alpha-to-coverage ×8 | 0.081 | 0.114 | 0.770 | −0.065 | 60.0 % |
| Stochastic alpha, 1 frame | 0.249 | 0.353 | 1.000 | −0.002 | 98.5 % |
| **Stochastic alpha, 8 frames** | **0.100** | **0.130** | 0.719 | **−0.001** | **99.6 %** |
| Weighted-blended OIT | 0.026 | 0.037 | 0.455 | −0.000 | 99.8 % |

### Sub-pixel — scalp at 2.5 m, 480×270, 17 500 segments

| mode | mean \|e\| | rmse | max \|e\| | bias | area % |
|---|---|---|---|---|---|
| Opaque ribbon | 0.0727 | 0.123 | 0.523 | −0.073 | **0.0 %** |
| Alpha-to-coverage ×2 | 0.0727 | 0.123 | 0.523 | −0.073 | **0.0 %** |
| Alpha-to-coverage ×4 | 0.0727 | 0.123 | 0.523 | −0.073 | **0.0 %** |
| Alpha-to-coverage ×8 | 0.0727 | 0.123 | 0.523 | −0.073 | **0.0 %** |
| Stochastic alpha, 1 frame | 0.119 | 0.247 | 0.996 | +0.004 | 105.6 % |
| **Stochastic alpha, 8 frames** | **0.055** | **0.085** | 0.535 | **−0.000** | **99.3 %** |
| Weighted-blended OIT | 0.009 | 0.014 | 0.116 | −0.000 | 99.5 % |

Those four identical rows are the result, not a formatting accident.

### Resolution — scalp at 1.0 m

| resolution | mean half-width | opaque ribbon area | stochastic 8-frame area |
|---|---|---|---|
| 240×135 (low) | 0.003 px | 0.0 % | 102.4 % |
| 480×270 (native) | 0.006 px | 0.0 % | 100.4 % |
| 960×540 (high) | 0.013 px | 0.0 % | 100.2 % |

### Temporal behaviour, static camera

| mode | frame-to-frame Δ coverage | 1-frame mean \|e\| | 8-frame mean \|e\| |
|---|---|---|---|
| Opaque ribbon | **0.000000** | 0.082 | 0.082 |
| Stochastic alpha | 0.932 | 0.082 | **0.044** |

The zero is exact and it is the important half of this table: **the cutoff's error is fixed pattern,
and a temporal resolve cannot remove fixed pattern.** The stochastic mode's error is noise, which is
precisely what a temporal resolve *can* remove — and does, halving it over eight frames.

### Memory at 1920×1080

| mode | extra render-target bytes |
|---|---|
| Opaque ribbon | 0 |
| Stochastic alpha | 0 |
| Weighted-blended OIT | 23.7 MiB |
| Alpha-to-coverage ×4 | 237.3 MiB |
| Alpha-to-coverage ×8 | 553.7 MiB |

## The decision, and the three rejections

### Rejected: alpha-to-coverage

**It cannot represent coverage below 1/(2·samples), so on real hair it degenerates into the hard
cutoff it was supposed to beat — while costing a multisample target.** The sample mask is
`round(alpha × S)` bits: at 8 samples a fragment needs alpha ≥ 0.0625, i.e. a projected half-width
of 0.031 px, to claim a single sample. The measured half-widths above are 0.003–0.040 px. In the
sub-pixel case all three sample counts produce output *bit-identical* to the cutoff's, which
`GroomCoverageComparison.SubPixelStrandsDefeatTheHardCutoff` now asserts as an equality.

Two further strikes, either of which would be enough on its own:

- **It needs a multisample target, and four of the six `{backend} × {path}` cells do not have one.**
  `Renderer3DRenderGraphSetup.cpp` pins the forward scene target to `Samples = 1`; MSAA exists only
  on the deferred G-Buffer. So the mode would be a deferred-only quality tier.
- **Its mask is a deterministic function of alpha**, so two overlapping strands at the same alpha
  claim *the same samples*. That is visible above as the persistent negative bias under dense
  overlap (−0.065 even at 8 samples, reproducing 60 % of the coat).

It costs 237–554 MiB at 1080p to be worse than a free alternative. The `AlphaToCoverage` enum value
is retained in `GroomVisibility.h` so the comparison can name it and `SelectGroomComposition` can
refuse it by name (`AlphaToCoverageUnimplemented`); the pass implements no sample mask.

### Rejected: weighted-blended OIT

**It is the most accurate mode measured, and it writes no depth.** Its coverage is
`1 − ∏(1 − αᵢ)`, which *is* the union of independent coverages, so it wins every accuracy column —
0.009 mean error in the sub-pixel case, four times better than the selected mode. The engine already
has the passes (`OITPrepareRenderPass` / `OITResolveRenderPass`, `include/OITCommon.glsl`), so
adopting it would have been the smallest diff here.

It is rejected on acceptance criterion 2: *"curves render with width and **depth-correct overlap**
against the body and ordinary scene geometry."* Weighted-blended OIT accumulates into a separate
target and composites at the end of the transparent band. Strands would neither occlude geometry
drawn after them nor be occluded by it, and self-overlap within the coat would be a weight function
rather than a depth test. A coat that does not resolve against the body it grows on fails the
criterion however good its coverage numbers are.

It remains a first-class enum value, and `SelectGroomComposition` *grants* it to a caller that
declares `RequiresDepthComposition = false` — so a future strand-only capture path can have it
without re-litigating this. Ordinary scene rendering sets that flag and gets
`DepthCompositionRequired`.

### Rejected as the primary mode, retained as the fallback: opaque alpha-cutoff ribbons

It is the only mode available unconditionally, and that is exactly its job. Its failure mode is not
aliasing — it is **losing the coat**: 0.0 % of the silhouette area reproduced in three of the four
cases, with a uniformly negative bias. A hard cutoff on sub-pixel strands is a thinning-hair defect,
not a jaggies defect.

### Selected: stochastic (hashed) alpha

- **Unbiased.** Signed bias ≤ 0.004 in every case; silhouette area 99.3–100.4 % across three
  resolutions, against the cutoff's 0.0 %.
- **Depth-correct.** It resolves coverage by discarding whole fragments, so surviving fragments
  write ordinary depth and criterion 2 is satisfied by the ordinary depth test.
- **Order-independent**, so no per-strand sort, on any path.
- **Free.** No extra target on any backend or path; 0 bytes in the memory table.
- **Portable.** Pure fragment-shader arithmetic — no sample masks, no new RHI state, identical on
  OpenGL and Vulkan, identical on Forward, Forward+ and Deferred.

Its cost is honest and is the reason for the fallback: a single frame is *worse* than the cutoff
(0.119 vs 0.073 sub-pixel) because the error arrives as noise. It only becomes the best available
mode once something averages it. So `SelectGroomComposition` **refuses** it when neither engine TAA
nor a temporal upscaler is running, and reports
`TemporalResolveUnavailable` rather than shipping a noisy coat.

## What this does not settle

- **Shading.** The coat is neutral-lit by design; fibre scattering is #1247 and dense-coat
  self-shadowing is #1248. Every number here is a coverage number.
- **LOD.** The strand budget in `GroomStrandMesh.h` is a resource bound, not a distance-based
  representation switch. Stable coverage under a strand/card/mesh LOD transition is #1252.
- **G-Buffer participation.** Strands render in a forward-style pass into the scene colour target
  on every path, so they receive no deferred lighting and cast no shadow. Named here rather than
  approximated, because a half-lit coat is exactly the thing criterion 4 forbids being mistaken for
  a finished tier.
- **The eight-frame history length** is a stand-in for what TAA and FSR2 actually converge over. It
  is a defensible figure (the engine's TAA feedback defaults to 0.9, an effective history of ~10
  frames) but it is a model of the resolve, not a measurement of it. The live captures in the PR
  are what tie it to real pixels.

## Backend status: both backends verified

Verified on an RTX 4090, Windows, Debug, `build-cached`.

**OpenGL** — all three paths draw the coat with identical pixel counts (81 825 px differ from the
strands-off control, max channel delta 146/255); an opaque slab in front hides 77 % of it
(81 825 -> 19 073 strand pixels) with no bleed-through; live editor reports 0 shader errors, 0
`[error]` lines, 0 VUIDs.

**Vulkan** — same scene, same camera: the coat renders, the slab occludes it, 0 `[error]` lines and
0 VUIDs. On both backends the seam engages and disengages live: with TAA off the log carries the
refusal sentence verbatim, and toggling TAA on clears it (proved by the reason being RE-logged on
the way back down, which only happens on a CHANGE of dominant reason).

### The Vulkan bug this cost, because it is the kind that ships

**The strand pass rendered nothing at all on Vulkan, and every diagnostic said it was fine.** The
backend was active, the pass ran and logged, it declared its render-graph resources, the draw was
recorded rather than dropped (`PrepareDrawCommon` warns on every one of its early-outs and none
fired), the vertex pull and the UBO both carried correct data, and there were 0 errors and 0 VUIDs.

The cause was one missing `abs()`:

```glsl
return projection[1][1] * viewportHeight * 0.5;   // pixels per world unit at w == 1
```

Vulkan's clip space has +Y downwards, so the engine uploads a projection whose `[1][1]` is
**negative** there. That made pixels-per-unit negative, so every strand got a negative projected
half width, `oloGroomWidenedAlpha` clamped it to 0, and the alpha test discarded **every fragment**.
The engine already guards the same matrix element the same way — `Renderer3D.h` stores
`|cull projection[1][1]|` and `RenderPipeline.cpp` takes `std::abs` of it — so this was a
convention that existed and was not followed.

It is worth recording how it was found, because the obvious hypotheses were all wrong. Two live
shader probes settled it in two runs: the first replaced `gl_Position` with a fixed on-screen quad
and the fragment output with an unconditional colour — the quad appeared, which cleared the pass,
the pipeline, the render scope, the depth state and the draw. The second kept the fixed quad but
encoded a PULLED vertex attribute in red and a UBO lane in green; both came back non-zero, which
cleared the vertex pull and the uniform buffer and left only the arithmetic between them.

Note that a shader validated by `glslc` is not a shader validated on Vulkan: this compiles
perfectly, and so does the cross-stage uniform-block mismatch that preceded it (see
`docs/agent-rules/glsl-shaders.md`).

## GPU cost and memory, on named hardware

<!-- Filled in from the live editor run; see the PR body for the captures these came from. -->

## Considered options

- **Alpha-to-coverage (MSAA sample mask).** Rejected: cannot express coverage below 1/(2·samples),
  so it is bit-identical to a hard cutoff on real hair widths; needs a multisample target that four
  of six backend×path cells do not have; its deterministic mask under-covers dense overlap; and it
  costs 237–554 MiB at 1080p.
- **Weighted-blended OIT.** Rejected: most accurate mode measured, but writes no depth, which
  acceptance criterion 2 requires. Retained as a selectable mode for a caller that declares it does
  not need depth composition.
- **Sorted per-strand alpha blending.** Rejected without measurement: it is order-*dependent* by
  construction, so it does not fit the comparison harness at all (which is the harness working as
  intended — see `GroomCoverageModel.EveryModeIsIndependentOfSubmissionOrder`), and a per-frame sort
  of a million strands is not a viable production cost.
- **A per-pixel strand fragment list (deep opacity / linked-list OIT).** Rejected as out of scope for
  a first slice: unbounded per-pixel memory, and the engine has no per-pixel linked-list machinery to
  build on. Worth revisiting if #1248's dense-coat transmittance needs a deep buffer anyway.
- **Opaque alpha-cutoff ribbons.** Not rejected — selected as the permanent, always-available
  fallback tier, and as the baseline every other mode is measured against.
