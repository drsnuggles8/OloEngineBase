# Stochastic sampling and temporal resolve

Applies to: `OloEditor/assets/shaders/include/StochasticCommon.glsl`,
`include/TemporalResolve.glsl`, the VNDF block in `include/PBRCommon.glsl`,
`OloEngine/src/OloEngine/Renderer/BlueNoise.h`, and any pass that adopts them.

Written while building the shared sampler for issue #706. Nothing here broke in
production — which is the point. Every defect this document describes renders a
plausible image, and three of the four would have shipped silently.

---

## 1. Blue noise is a claim about ERROR, not about numbers

The reflex reading of "use blue noise" is "use better random numbers". It is
not, and holding the wrong model leads you to test the wrong thing.

At a fixed sample count the per-pixel error is whatever the estimator's variance
says it is; a blue-noise sampler does not reduce it. What it changes is **where
that error sits in the screen-space frequency spectrum**. White noise spreads
error energy evenly across all spatial frequencies, including the low ones — and
low-frequency error is exactly what a 3×3 neighbourhood filter, a temporal
resolve, and the human visual system cannot remove. Push the same energy into the
high frequencies and every one of those three attenuates it for free.

So the property to assert is **the spectrum of the error image**, not any
property of the sample values. Uniform values, correct mean, full range and a
plausible histogram are all true of white noise — which is exactly why any metric
that the new and the replaced formulation could *both* pass needs to be paired
with an assertion that the replaced one fails it. (Metrics that only one
formulation could ever satisfy do not need the pairing; the rule is about
discrimination, not about decorating every number with a negative.)

Measured on this engine's tile (64×64, void-and-cluster), as the ratio of mean
low-band power (radius 1..8) to mean high-band power (radius 16..32):

| field | low/high power | lag-1 neighbour correlation |
|---|---|---|
| the tile itself | **0.0002** | −0.284 |
| one frame of the animated sampler | **0.057** | — |
| a single-sample error image (nonlinear f) | **0.240** | — |
| white noise, same metrics | **0.80 – 1.01** | −0.010 |

Two things to take from that table. The tile is spectacularly blue in isolation;
by the time it has been advanced in time and pushed through a nonlinear
estimator, the advantage is **~4×**, not ~5000×. That is still the whole point —
4× less of the error the eye actually sees, at zero extra ray cost — but quote
the 4×, not the 5000×.

---

## 2. You cannot evaluate blue noise procedurally

A hash gives white noise. A low-discrepancy sequence (Sobol′, R2, Halton) is
*low-discrepancy in its own index*, not blue in screen space. There is no cheap
closed form for "value at pixel (x, y) that is blue-noise-distributed across
(x, y)" — the property is global to the field, so the field has to be
**constructed**, offline or at startup, and then looked up.

That leaves three options, and it is worth knowing why this repo picked the third:

1. **Heitz et al. 2019 ranking/scrambling tiles** — what issue #706 cites. Best
   in class, but the tiles come out of a simulated-annealing optimiser and ship
   as ~1 MB of binary supplemental material. Vendoring an unreviewable blob or
   re-running the optimiser were the only two ways to get them.
2. **A committed blue-noise PNG** — cheap, but every property claim then attaches
   to *a file*, not to code, and nothing in the repo can regenerate or re-verify
   it.
3. **Generate it at startup with void-and-cluster** (Ulichney 1993) — 105 ms in
   a Debug build for 64×64×2 channels, deterministic, and every property in §1's
   table is a test against the same code the renderer runs.

Void-and-cluster is worth understanding rather than copying, because its shape
explains why (2) is unsatisfying: it does not filter a random pattern into a blue
one (you cannot — a low-pass filter cannot distinguish the low frequencies you
want gone from signal). It maintains an **energy field** and repeatedly moves the
most crowded set pixel into the emptiest gap, so the result has no cluster and no
void at any scale, which *is* the spatial statement of "no low-frequency energy".

Two implementation traps, both silent:

- **The kernel must wrap.** The tile is repeated across the screen, so a pixel
  near the left edge is a neighbour of one near the right. Forget it and the
  pattern is blue in the interior and clustered along the seams — on screen, a
  faint grid at the tile period, which reads as a *tiling* bug and sends you to
  look at the sampler's wrap logic instead of the generator's.
- **Cost is O(pixels²)** because every step scans for an argmin/argmax, and it
  scales worse than the pixel count suggests. Measured, Release, two channels:
  64×64 takes **12 ms**, 128×128 takes **466 ms** — 38×, well past the 4× the
  pixel count predicts, because the scans stop fitting in cache. In a Debug
  editor startup that is ~4 s. And the larger tile is spectrally **identical**
  (low/high band power 0.0002 either way, neighbour correlation −0.272 vs
  −0.284), so there is nothing on the other side of the trade.

**Two channels, generated independently.** A 2D sample takes one component from
each. Deriving the second from the first — `fract(first * golden)` and relatives —
collapses to a short ramp over the quantised first channel and biases every
sample; see [glsl-shaders.md](glsl-shaders.md) §11, where exactly that shipped.

**And "independently" is harder than it looks — this one actually bit.** A
counter-based PRNG (SplitMix and friends) advances its state by a fixed
increment, so *every* seed lands somewhere on one cycle: two seeds are the same
sequence at different offsets. Seed it with `seed * <the increment>` and seeds 1
and 2 end up exactly **one draw apart**.

That is what the first version of `BlueNoise.h` did. The two channels' initial
patterns were 99.3% identical, the relaxation kept them that way, and the
finished channels correlated at **+0.55** — so a 2D sample's two components were
not independent at all. Both channels were still perfectly good blue noise
individually: full range, mean 0.5, low/high power 0.0002, neighbour correlation
−0.28. **Every per-channel metric passed.** Only the cross-channel correlation
saw it (`StochasticSampler.TileChannelsAreIndependent`), and the tell that
confirmed the diagnosis was that far-apart seeds behaved fine — 1000/2000
correlated at +0.012 while 1/2 correlated at +0.547, which is a *stream offset*
signature and nothing else.

The fix is to XOR the avalanched seed into every counter step rather than using
it as a starting offset, giving genuinely distinct streams. The general rule:
**a per-stream seed must be mixed into each draw, not used to pick a position in
one shared sequence** — and if you must do the latter, never derive the offset
from the generator's own increment.


---

## 3. Animating the tile costs some of the property, and it is not obvious

The standard temporal advance is `fract(tile + frameIndex * R2)`. The usual
justification — "a toroidal shift of a blue-noise field is still blue" — is about
a shift in **space**. This is a shift in **value**, with wraparound, so pixels
whose values straddle the wrap swap their relative order and some of the
carefully-built structure is destroyed.

Measured: low/high power goes from **0.0002 on the raw tile to 0.057 on an
advanced frame**. Still ~18× better than white, but not free, and not what the
folklore implies.

The advance is still right — without it each pixel's temporal sequence is a
constant and a temporal resolve accumulates one still image's worth of noise
forever. Just assert **both** halves: that consecutive frames are genuinely
redrawn (measured cross-frame correlation −0.12), *and* that each frame is still
blue. A test that only checks the first passes on a sampler that has degenerated
to white.

Per-pixel temporal quality is best measured as **largest gap in the sorted
sequence**: over 64 frames, R2 gives 1.25× the ideal 1/N, white noise 4.70×.

**The engine's counter must advance unconditionally; what a PASS samples with is
a separate question.** `TAAJitterFrameIndex` is zeroed when TAA is off and
`CloudFrameIndex` only moves when clouds are on; either would freeze the sampler
for every other pass. #706 added `StochasticFrameIndex` in `Renderer3D`'s data,
advanced once per frame in `RenderPipeline` regardless of which passes are
enabled, wrapped to 2²⁰ so it stays exactly representable in the `f32` the params
UBOs carry it in.

**But a pass with no accumulator behind it should sample with a FROZEN index**,
and this is easy to get backwards. Advancing the sampler is what lets a temporal
resolve converge; with nothing averaging the result it only replaces static grain
with grain redrawn every frame, which is strictly worse to look at.

For a while SSR and SSGI had no history of their own, so the only thing
accumulating them was TAA and `RenderPipeline` passed them
`TAAEnabled ? StochasticFrameIndex : 0`. That gate was documented here as
becoming **wrong** the moment either pass gained a history buffer, and it did:
**#902 gave both passes their own accumulator and the gate is gone** — the index
now advances unconditionally at the UBO fill, which is what makes them converge.
The rule the gate expressed still stands for the next pass that adopts the
sampler without adopting the resolve: *advance the counter, but freeze what a
pass SAMPLES with until something is averaging it.*

---

## 3a. A blue-noise rotation is only valid on a dimension that WRAPS

This is the mistake that actually shipped in the first draft of #706, survived a
full test suite, and was caught only by rendering the frame.

The standard way to give every pixel the same low-discrepancy point set with a
per-pixel offset is a **Cranley-Patterson rotation**: `fract(sample + offset)`.
It is correct, it preserves the set's discrepancy, and it is what every
blue-noise-mask paper does. So the obvious construction is to rotate **both**
components of a 2D sample by the two channels of the tile.

That is wrong whenever a dimension is not toroidal, and a hemisphere **radius**
is not. `OloCosineHemisphere` maps `u1` through `sqrt(u1)`, so wrapping `u1`
from 0.95 to 0.05 does not nudge the sample — it converts a grazing ray into a
near-normal one. The rotation therefore destroys precisely the stratification
that `u1 = (rayIndex + 0.5) / rayCount` gives for free.

Measured, modelling SSGI's own integral (cosine-weighted hemisphere gather,
8 rays/pixel, per-pixel error RMS against a converged reference):

| sampler | smooth integrand | hard-edged (occluder) |
|---|---|---|
| interleaved-gradient (what was there) | 0.0218 | 0.0755 |
| **rotate both dimensions** | **0.0349** | **0.0856** |
| stratify dim 0, jitter within the stratum | **0.0124** | **0.0574** |

Rotating both was **worse than the noise it replaced**, on both integrands, and
the rendered frame agreed (1.38× more low-frequency noise energy). The fix is
ordinary stratified sampling with a blue-noise jitter *inside* each stratum:
`u1 = (rayIndex + blue.x) / rayCount`. The stratification survives intact, and
the jitter is what decorrelates neighbouring pixels — which the old sampler
never did at all, since it used the same `u1` at every pixel and its dimension-0
error was therefore a screen-wide constant (a bias, not noise).

**Rule:** before rotating a sample dimension, ask what the consumer does with
it. Rotate the toroidal ones (an azimuth); stratify-and-jitter the ones that
map monotonically onto a geometric quantity (a radius, a distance, a mip).
Pinned by `StochasticSampler.DimensionZeroStaysStratifiedAcrossSamples`, whose
paired negative asserts the rotated form fails it.

**And note which instrument caught it.** The CPU test said the new sampler won by
4.2×, because it modelled a *single* sample of a smooth function on the unit
square — where a rotation is exactly right and there is no stratification to
destroy. The model was wrong in two ways at once (one sample instead of N, unit
square instead of hemisphere), and both errors flattered the change. Only
rendering the actual pass disagreed.

---

## 3b. Structured noise is the artifact; RMS will not show it

When the corrected sampler was measured against the old one in a real rendered
frame, the summary numbers were almost a tie: total noise RMS 3.5% lower,
low-frequency band energy within 4%. On those numbers alone the change looks
barely worth making.

The visualisation said otherwise: the old frame's residual is a **regular
cross-hatched lattice**, the new one is unstructured grain. Quantified over the
same crop:

| | spectral crest (max/mean) | energy in the top 0.1% of frequency bins |
|---|---|---|
| interleaved-gradient | **515** | **77%** |
| shared blue-noise sampler | **39** | **13.5%** |

Three quarters of the old sampler's noise energy sat in a handful of
frequencies — a coherent repeating pattern, which is the same family of artifact
as the GTAO "goosebumps" lattice in [glsl-shaders.md](glsl-shaders.md) §11.
Structured noise of a given energy is far more visible than unstructured noise of
the same energy, and no temporal filter or small-radius denoiser removes a
coherent lattice; both are built on the assumption that the error decorrelates.

**So when comparing samplers on a rendered frame, measure spectral peakiness, not
just RMS or band energy** — and look at the amplified residual, because a lattice
is instantly obvious to the eye and nearly invisible to a single scalar.

### How to run the A/B at all

Shaders are runtime assets, so two versions can be compared with **no rebuild**:
run `OloEngine-Tests.exe --gtest_filter='SSGIVisualEvidence*'`, swap the `.glsl`,
run again, and diff the PNGs it writes to `OloEditor/assets/tests/visual/`.

Two things will silently defeat that:

- **The shader cache.** This used to require deleting the cached binary by hand
  between arms — since issue #906 the cache key is a content hash of the
  preprocessed source, so editing the `.glsl` changes the key automatically and
  the previous compile is never served. If you're on a pre-#906 checkout,
  delete `OloEditor/assets/cache/shader/opengl/<Name>.glsl.cached_*` between
  arms, or the engine serves the previous compile and both runs produce a
  **byte-identical** image. That happened here, and the log said "Compiling
  shader … from source" while it did it.
- **The obvious capture tools.** `olo_screenshot` and `olo_render_capture_target`
  were both measured returning byte-identical images at SSGI intensity 0.0 and
  8.0, with `frameIndex` advancing. Neither reflects live post-process settings.
  The offscreen evidence tests set their own state in C++ and are the reliable
  instrument; `driver.ps1 -Action shot` also responds if you need the live window.

**Validate the instrument before believing a null result:** patch the pass to
emit something blatant (`indirectDiffuse = vec3(0, 1, 0)`) and confirm the output
moves. Two identical frames are far more often a stale cache than a real tie.

---

## 4. The VNDF weight is the silent-failure archetype

GGX importance sampling comes in two flavours and they are not interchangeable.
`ImportanceSampleGGX` samples the full normal distribution D; `sampleGGXVNDF`
samples the distribution of **visible** normals. At grazing angles — the angles
screen-space reflections are used at — sampling D draws mostly masked
microfacets whose contribution is discarded, so the same ray budget buys far less.

With VNDF sampling the specular estimator collapses to

```
f * cos(theta_L) / pdf  ==  F * (G2 / G1)
```

`ggxVNDFWeight()` is that `G2/G1`. **Omitting it is the mistake to be afraid of:**

- Nothing throws. No test that checks "is the image plausible" notices.
- `G2/G1 <= 1` always, so the error is one-signed: the pass is **too bright**,
  permanently.
- **The bias is smallest exactly where it is easiest to look.** Measured against
  brute-force hemisphere integration: dropping the weight overestimates by
  **+1% to +7% at roughness 0.4**, but **+17% to +19% at roughness 0.7**. Check
  it on a smooth surface and it looks correct.

The test that catches it is a comparison against a brute-force integration of the
same single-scattering BRDF (this repo: `StochasticSampler.VndfEstimatorMatchesBruteForce`,
which agrees to five decimal places) — **paired with an assertion that the
unweighted form FAILS the same comparison**. Without that second half the test
passes on the bug it was written for.

**Alpha convention.** These functions use `alpha = roughness²`, matching
`distributionGGX`. Unbiasedness requires the Λ paired with the *D you sampled*,
so the VNDF block carries its own `ggxSmithLambda`.

They used to disagree with `geometrySmithHeightCorrelated`, which squared
roughness only once. That was recorded here as a pre-existing D/G inconsistency
"left alone because moving it would shift every lit golden" — and the second
half of that sentence was **wrong**, which is why it survived two more issues.
#904 checked instead of assuming: the mismatched function's only caller chain
(`cookTorranceBRDFEnhanced` → `calculateLightContributionEnhanced`) had **zero
references repo-wide**, so nothing rendered used it and no golden moved. It is
now `visibilitySmithGGXCorrelated`, on `alpha = roughness²` like everything
else, and `ReferenceBRDFTest.HeightCorrelatedVisibilityMatchesTheVndfLambda`
asserts the exact algebraic identity between it and the `ggxSmithLambda` above.

The general lesson is worth more than the fix: **"changing this would move every
golden" is a claim about the call graph, and it is one grep.** Costed at "a
whole rebake plus a visual audit", it was never re-checked; the grep took a
minute and the rebake turned out not to exist. Verify reach before you price a
correctness fix out of scope.

**Fresnel goes with the microfacet.** A VNDF-sampled ray must evaluate Schlick
against `dot(V, H)`, not `dot(V, N)`. On a smooth surface H == N and it makes no
difference, which is how the wrong one survives.

---

## 5. A temporal resolve clips; it does not clamp

Four questions, always the same four: where was this pixel (reprojection), is it
the same surface (disocclusion), is its value still plausible (neighbourhood
clip), how much do I keep (feedback).

The one worth spelling out is the third. Constraining the history to the
neighbourhood box **componentwise** moves it to the nearest point of the box,
which can be a colour the neighbourhood never contained — a rejected history
shifts *hue*. Clipping it along the segment toward the box centre keeps it on a
line between two colours that are both in the frame, so it desaturates instead.
The difference is invisible on a grey test scene and obvious on a saturated one,
which is why TAA's componentwise clamp survived as long as it did.

Two more that are easy to get subtly wrong:

- **Disocclusion must be tested on view-space distance, relative.** Device depth
  is wildly nonlinear, so a fixed tolerance on it means centimetres near the
  camera and kilometres far away — a rejection test that works only at the
  distance you happened to test it from, which is worse than none because it
  looks like it works.
- **The motion-scaled feedback needs a sub-pixel dead zone.** Any jittered pass
  moves ~1 px frame to frame by construction. Without a dead zone a stationary
  camera reads as motion, feedback collapses toward its floor, and a fraction of
  the current frame bleeds through every frame — a faint shake that reads as "the
  resolve is broken" rather than "the ramp starts at zero".

---

## 6. Adopting the utility in a new pass

`#define OLO_BLUE_NOISE_GLOBAL_SAMPLER` **after** `include/BindlessHeap.glsl`
(the bindless branch of the tile declaration expands `OLO_HEAP_TEX_2D`), then
`#include "include/StochasticCommon.glsl"`. On the C++ side, `BlueNoiseTexture.h`
has `CreateBlueNoiseTexture()` / `DestroyBlueNoiseTexture()` / `BindBlueNoiseTexture()`;
call them from `Init()`, the destructor and `Execute()`.

Three things to know:

- **`TEX_BLUE_NOISE` is slot 17, and 17 is also `UBO_FOG` and
  `SSBO_INSTANCE_DRAW_INDIRECT`.** Fine across GL's disjoint namespaces and fine
  on Vulkan — except **inside one shader**, where the single-set model makes it a
  real collision. That is the ADR item A2 trap that moved `TEX_DDGI_VISIBILITY`
  off 57 in #691. So: no shader may sample the tile and also declare uniform or
  storage block 17. `StochasticSampler.NoShaderCollidesWithTheBlueNoiseSlot`
  parses the shader tree (following includes transitively) rather than trusting
  anyone to remember.
- Slot 17 was chosen because it is **below** `TEX_SHADER_GRAPH_0` and therefore
  does not move `MAX_ENGINE_TEXTURE_SLOTS` or the derived `HEAP_IMAGE_SLOT_BASE`.
  Adding a slot *above* it is also a shader edit — see the `OLO_HEAP_IMAGE_BASE`
  row in [README.md](README.md) §2.
- **Each pass owns its own texture** (8 KiB — 64 × 64 × RG8, one mip). The
  shared thing is the CPU tile
  (`BlueNoise::GetTileRG()`, a POD array). That is deliberate: a shared *GPU*
  object would be the lazy-static-with-a-lifetime shape
  [lazy-static-release-ownership.md](lazy-static-release-ownership.md) is about.

**Temporal resolve has a prerequisite the sampler does not: a history buffer for
the pass's own signal.** SSR and SSGI both composited into the scene colour, so
their output target was *not* accumulable — temporally blending it would smear the
base colour too. #706 deliberately stopped short of fixing that and shipped the
sampler for those two passes alone; **#902 did the structural work**, and §6a
below is what it cost. `PostProcess_CloudscapeResolve.glsl` remains the obvious
next adopter of the resolve itself (it was left alone in #706 because its signal
is RGBA — alpha carries transmittance — and the cloud goldens are unusually
sensitive, see
[volumetric-cloud-debugging.md](volumetric-cloud-debugging.md)).

---

## 6a. Retrofitting a resolve onto a compositing pass: four things that are not obvious

From #902, which gave SSR and SSGI each a signal target, a resolve and a history.
None of these is exotic; all four are the kind of thing you only discover by
starting.

**1. A resolve needs the current frame's signal in a TEXTURE, so the pass becomes
three draws, not one.** The tempting shape is one shader that computes the signal
and blends the history in the same pass. It cannot work: the neighbourhood clip
gathers a 3×3 of the *current* signal, and eight of those nine neighbours have not
been computed yet inside that draw. So the shape is A (signal) → B (resolve) → C
(composite), three fullscreen draws inside ONE render-graph node, with the two
intermediates as graph scratch declared under the pass's own gate and marked
`AllowSamePassReadWrite`. `CloudscapeRenderPass` already had exactly this
structure and is the template — copy it rather than re-deriving it.

**2. What accumulates must be a SIGNAL whose miss value is zero, not the
composited colour.** For SSGI that is the raw indirect diffuse (no base, no
intensity). For SSR the composite is `mix(base, reflection, blend)`, which is not
a sum — so the accumulable quantity is the **delta** `(reflection - base) * blend`,
and the composite becomes the plain add `base + delta`. That identity is what lets
draw C reproduce the old replace/mix resolve exactly. The test worth writing is
the negative half: **a miss must contribute exactly 0**, where the pre-split output
of a miss was `base` — and accumulating *that* is the smear the whole exercise
exists to remove.

**3. The disocclusion test needs a depth history, and the signal's ALPHA is where
it goes.** `OloTemporalDepthConfidence` compares this frame's view depth against
*the previous frame's stored depth at the reprojected UV*, which nothing in the
frame hands you — the depth buffer only has this frame. Rather than a second
history texture, pack the positive view depth into the signal's alpha: the history
copy carries all four channels (`CopyImageSubData`), so last frame's depth arrives
for free. The trap that follows: **every early-out in the signal shader must still
write alpha.** A sky pixel or a roughness-rejected pixel that returns before
filling it stores depth 0, compares as a 100% relative error next frame, and reads
as a permanent disocclusion — the resolve then quietly never accumulates there.

**4. Only `Execute` knows whether the history exists, so only `Execute` can fill
that UBO lane.** Whether a history was imported this frame is decided in
`PopulateBlackboard`, which runs *after* the per-frame pass configuration that
fills the UBO. Snapshotting "is there history" at configure time is a frame late,
and on the very first frame it is wrong in the dangerous direction — the resolve
blends against an uninitialised buffer. Fill the static lanes (feedback, gamma,
the user toggle) at the UBO upload and have the pass patch the runtime lanes with
a partial `SetData(&vec4, sizeof(vec4), offsetof(...))` once it has actually
resolved the handles. `CloudscapeRenderPass` solves the same problem the other
way — a `SetHistory()` call issued after `PopulateBlackboard` — which works too;
what does not work is deciding it before.

**And the ordinary graph obligations still apply.** A new history flag gates an
import, so it must be hashed into the blackboard fingerprint or the
false→true transition on the first successful frame never re-runs
`PopulateBlackboard` and the import never lands
([render-pipeline-caches.md](render-pipeline-caches.md), and the third and fourth
entries in the `HashBool` block in `RenderPipeline.cpp` are the previous two
victims of exactly this). And the new scratch targets are transients, so
`OLO_RG_POISON_TRANSIENTS` / `OLO_RG_DISABLE_ALIASING`
([render-graph-transient-aliasing.md](render-graph-transient-aliasing.md)) are the
first two levers to reach for if the accumulation ever looks like it is reading
someone else's buffer.

---

## 7. Two samplers in one engine

`Renderer/PathTracing/PathSampler.h` (Owen-scrambled Sobol′, CPU, for the offline
reference tracer) predates this work and optimises something different:
bit-reproducibility and per-pixel convergence for an oracle image.
`StochasticCommon.glsl` optimises screen-space error distribution at 1–2 spp.

They are allowed to differ in what they optimise. They are **not** allowed to
disagree about the sequence, or a future reader has to work out which definition
of "stratified" a given pass meant. So the GLSL is a bit-exact transcription of
PathSampler's integer math — same `ReverseBits`, same Laine-Karras permutation,
same nested-uniform scramble, same Sobol′ direction numbers, same 24-bit
`ToUnitFloat` — and `StochasticSampler.GlslSobolMirrorsPathSampler` asserts they
produce identical values.

A transcription can drift, and a mirror living in a test file drifts *with* it
without failing. `ShaderCarriesThePathSamplerConstants` reads the magic numbers
back out of the `.glsl` and checks them, because those constants **are** the
sequence.

The one structural difference, and it is load-bearing: **the GPU sampler's seed
does not depend on the pixel.** Every pixel walks the *same* Sobol′ point set,
offset by a blue-noise rotation — that is what makes the per-pixel error a rigid
function of the rotation and therefore blue across the screen. A per-pixel seed
gives each pixel an independent, still-low-discrepancy sequence whose errors are
mutually *white*, which is the ordinary sampler this exists to replace. Pinned by
`AllPixelsShareOneSequenceOffsetByTheTile`.
