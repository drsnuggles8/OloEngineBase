# The screen-space denoiser chain

Rules for touching the SSGI / SSR denoiser stages: the trace, the pre-blur, the temporal resolve,
the post-blur and the guided upscale (`SSGIRenderPass`, `SSRRenderPass`,
`OloEditor/assets/shaders/PostProcess_SSGI*.glsl`, `PostProcess_SSR*.glsl`, and the shared kernel in
`include/SpatialDenoise.glsl`). Built in issue #708 on top of the temporal resolve from #902 and the
shared surface-history layer from #976.

Companion to [stochastic-sampling-and-temporal-resolve.md](stochastic-sampling-and-temporal-resolve.md),
which owns the sampler and the resolve itself. This file owns what happens on either side of them.

---

## 1. A stage that runs at a different resolution than the G-Buffer needs its OWN guide

**Rule: every stage of the chain is guided by the guide attachment the trace writes, never by the
full-resolution G-Buffer. If you add a stage, read the guide.**

The chain runs at the *trace band* — half the scene band whenever `SSGIHalfResolution` is on. The
G-Buffer is at the scene band. Guiding a half-resolution filter with a full-resolution normal or
depth means the filter is rejecting taps by silhouettes that do not exist in the image it is
filtering: every edge is rejected on both sides, so the noise along every silhouette never gets
filtered and never gets accumulated either, and no amount of history length fixes it because the
temporal surface test is making the same mistake.

So `PostProcess_SSGI.glsl` writes a second attachment, packed exactly like G-Buffer RT1
(`rg` = octahedral world normal, `b` = roughness, `a` = AO), at the trace band. The pre-blur, the
resolve, the post-blur and the upscale all weight against **that**, and `SSGISurfaceHistory` is
extracted from **that**, so both sides of the temporal surface test are the same quantity at the
same resolution.

The guided upscale is the one exception, and it is not a counter-example: it runs at the scene band
and deliberately reads *both* — the full-res depth and normal for the pixel it is shading, and the
trace-band guide for each tap it is choosing between. Deciding which half-res tap belongs to the
pixel in front of the viewer is precisely a cross-resolution question.

## 2. Sample the guide with `texelFetch`, never `texture()`

**Rule: any read of an octahedrally-encoded normal is an integer texel fetch.**

A bilinear tap between two oct-encoded normals interpolates across the encoding's fold, and the
decoded result points somewhere neither neighbour does. On a silhouette that is the difference
between "reject this tap" and "accept a tap whose normal is 90 degrees wrong".

The same applies to the trace's own centre reads. `PostProcess_SSGI.glsl` fetches its shading
point's depth, normal and albedo with `texelFetch` at `FullResTexel(uv)` rather than sampling: at a
half-resolution fragment a bilinear tap averages a 2x2 block, and across a silhouette that averages
a foreground depth with a background one into a position on neither surface. The ray then starts
inside geometry and the pixel goes black.

What the ray *march* samples is still an ordinary filtered lookup at an arbitrary UV — only the
centre reads are snapped.

## 3. The geometry test is a PLANE DISTANCE, not a depth difference

**Rule: measure a tap's distance along the centre pixel's normal. Never compare view depths.**

A tap on the same flat floor as the centre pixel, seen at a grazing angle, differs in view depth by
metres while lying exactly in the centre's tangent plane. It is the most valuable tap there is — the
same surface — and a depth-difference test throws it away, leaving grazing floors unfiltered and
noisy while flat-on ones look fine. `OloDenoisePlaneWeight` measures along the normal, which keeps
it and still rejects the wall behind it. Pinned by
`ScreenSpaceDenoiseMath.PlaneWeightKeepsGrazingCoplanarTaps`.

The tolerance is **relative to the view depth**, not a world-space constant and not a pixel width:
the chain runs at two resolutions in the same frame, so a pixel-width tolerance would mean two
different tolerances for the same surface. It is kept numerically equal to the resolve's own
`SSGI_DEPTH_TOLERANCE`, because the spatial and temporal halves of a denoiser disagreeing about what
a surface is produces artifacts that look like neither one's bug.

## 4. Resolution is a graph-topology input; a radius is not

**Rule: a setting that changes the SIZE of a chain resource must be hashed into the blackboard
fingerprint AND must invalidate the temporal histories. A setting that only changes a UBO value must
be neither.**

`SSGIHalfResolution` resizes the signal targets, the pre-blur and post-blur scratch, and all four
SSGI histories. Miss the fingerprint hash and toggling it reuses a cached graph whose targets are
the wrong size (the #530 class — see [render-pipeline-caches.md](render-pipeline-caches.md)). Miss
the history invalidation and the resolve reprojects a history allocated at another resolution, which
reads every fetch at the wrong place: not an error, a uniform smear that persists until the camera
happens to move enough to reject it.

The two blur radii are the opposite case. They change nothing about what is declared, so hashing
them would rebuild the graph on every slider drag. **This is why a radius of 0 skips the DRAW but
still declares the RESOURCE**: if the declaration depended on the radius, the radius would be a
topology input and would have to be hashed after all.

## 5. The history takes the RESOLVE's output, never the post-blur's

**Rule: `ExtractHistoryTexture` reads the temporal resolve's attachment. The post-blur is downstream
of the extraction, not upstream.**

Accumulating a filtered copy of the estimate compounds the filter with itself frame after frame, and
the disocclusion widening — which is *supposed* to be temporary — gets baked into the history and
then takes the full history length to wash out. Widening after the extraction is what lets the
post-blur hide fresh noise without paying for it later.

## 6. Every stage needs its input in a real texture

**Rule: do not fold two stages into one shader.**

Each stage gathers a NEIGHBOURHOOD of the stage before it — the pre-blur's Poisson disc, the
resolve's 3x3 clip box, the post-blur's disc, the upscale's 2x2 footprint. A gather needs the whole
previous draw to have finished writing, so five stages are five draws. They live in one render-graph
node (the `CloudscapeRenderPass` shape) with `AllowSamePassReadWrite` on each intermediate, not five
nodes.

## 7. What transfers from a diffuse denoiser to a specular one, and what does not

**Rule: guide a specular filter on ROUGHNESS. Never on variance.**

This is the one place the SSGI and SSR chains genuinely differ, and it is worth being explicit
because "apply the same denoiser to SSR" sounds like it should just work.

Indirect diffuse is smooth everywhere, so blurring it more where it is noisier is always safe. A
mirror carries the sharpest detail in the frame, and its reflection of a high-contrast edge is
legitimately high-variance — a variance guide would blur precisely the pixels that must stay sharp.
What sets the correct filter width for a reflection is the width of the specular lobe the sample was
drawn from, which is roughness. `OloDenoiseRoughnessRadius` ramps from 0 below the knee, so a mirror
is not filtered at all.

For the same reason SSR gets **no half-resolution trace and no upscale** (a reflection is exactly the
signal half resolution destroys) and **no quad ray distribution** (it draws one VNDF sample per
pixel, so there are no strata to subdivide across the quad).

## 8. A pre-blur turns sparse-hemisphere noise into blobs; keep its radius small

**Rule: without an energy-preserving firefly pre-filter ahead of it, keep the pre-blur radius at
about 1 trace-band pixel. Judge it by looking at a magnified crop, not by a noise metric.**

The pre-blur spreads each ray HIT over its own radius. Where the hemisphere is sparse — a surface
whose rays mostly leave the screen, so a few pixels get a large contribution and most get zero —
that is a blob generator: fine grain becomes soft low-frequency mottling at the blur's scale.

**Every noise metric says this is an improvement**, because a 3x3 high-pass residual cannot see
structure at the blur radius. Measured on the #708 evidence scene (floor patch, 4 rays, local
high-pass noise, against an 8-ray unfiltered baseline of 1.215):

| pre-blur radius | noise | mean | magnified crop |
|---|---|---|---|
| 0 | 0.875 | 142.04 | clean |
| 1 | 0.802 | 143.35 | blobs faint |
| 2 | 0.800 | 143.37 | blobs clearly visible |

Radius 1 keeps 99.7% of radius 2's noise reduction. The reference avoids this differently — it runs
an energy-preserving firefly clamp (`rtgi_pre_filter`) *before* its pre-blur, which is what lets it
use a base width of 64. This chain has no such stage, so it keeps the radius small instead.

**How to see it:** crop the region to ~120x90 pixels and scale it 4x with nearest-neighbour. At 1:1
the two arms look equally fine and the metric is the only thing talking.

### 8a. A disc tap that rounds to (0,0) is the centre counted twice

**Rule: push every disc tap at least one texel off the centre (`OloDenoiseTapOffset`), and never
`ivec2(round(offset))`.**

The Poisson points have magnitudes from 0.16 to 0.95, so at a radius of 1-2 texels three or four of
the eight round straight back onto the centre. The centre is then accumulated four or five times,
the kernel is far weaker than its "8 taps" suggests, and its shape is centre-heavy — the same
"weight one sample many times" failure the out-of-bounds branch in each blur exists to prevent, and
completely invisible in the code. It shipped as the default pre-blur radius here until a review
caught it. Pinned by `ScreenSpaceDenoiseMath.EveryDiscTapLandsOffTheCentreTexel`.

The table in §8 was re-measured after fixing this, because a filter-strength measurement taken
against a kernel that is not the kernel you think it is answers a different question.

## 9. A per-pixel-fixed stratum phase is a BIAS, not noise

**Rule: if you split a stratum grid across neighbouring pixels, rotate the assignment per frame.**

With the quad phase pinned to the pixel's parity, pixel p draws `u1` from
`(4r + phase + jitter) / (4N)` for `r = 0..N-1`. The jitter is in `[0,1)`, so the union over r is N
strata of width `1/(4N)` — total measure exactly **1/4** of the domain, forever. Each pixel
converges to the mean of its own quarter of the integrand, which no temporal resolve can remove
because it is not noise.

Rotating by the frame index makes each pixel visit all four phases over four frames while any single
frame still has the quad covering four complementary phases. The rotation is by a whole stratum, so
it permutes which strata a pixel owns rather than moving `u1` within the domain — which is the
distinction from the failure `OloSampleStratified2D`'s comment documents.

Worth knowing how this one was found: it was made while chasing the mottling in §8, on the theory
that the fixed phase caused it. It did not, and the fix changed the rendered frame by no visible
amount. It is kept because the measure argument is correct on its own — but the artifact hunt is
what surfaced it, and the A/B is what separated the two causes.

## 10. Register every new texture-binding name

A new `layout(binding = N) uniform sampler2D u_Foo` must be added to
`ShaderBindingLayout::IsKnownTextureBinding` for slot N, or `ShaderReflectionBindingTest` fails with
"not recognised by ShaderBindingLayout::IsKnownTextureBinding". The chain's guide appears at three
different slots under the same name, because each stage has a different number of inputs ahead of
it; that is fine, and each slot needs its own entry.

## 11. Validate the shaders with `glslc` before spending a build

Ten shaders change together here and a Debug build takes 30+ minutes after it wins the build lock.
Split each `#type` section into a temp file **in the shader's own directory** (so the relative
include model matches the engine's) and run `glslc` on it with **no `-I`** — see
`validate-shaders-with-glslc-first` in the persistent memory store for the full recipe and why the
`-I` matters.

---

## Appendix: the reference this is derived from

The chain's shape, the Poisson-disc set, the golden-angle per-frame rotation and the
plane-distance/normal weight pair come from the Denoisinator in Timberdoodle
(`src/rendering/rtgi/`, Apache-2.0), ported from HLSL compute to GLSL fullscreen draws. Its
`denoisinator.md` argues the ordering this chain follows: a cheap stochastic spatial pass *before*
the temporal one and a small cleanup *after*, rather than either alone — spatial-then-temporal
shimmers in motion, temporal-then-spatial reacts slowly and needs one expensive wide filter.

Two deliberate departures, both noted at their functions in `include/SpatialDenoise.glsl`:

- **The reference does not guide on variance at all**, on the grounds that a per-pixel variance
  estimate of a one-ray-per-pixel signal chases its own noise. That objection is about variance
  measured on the RAW trace. What this chain reads is the variance the temporal resolve has already
  accumulated over up to 255 frames, taken relative to the mean so exposure cannot move it, with the
  history-length term dominating wherever the history is too short for it to mean anything.
- **The reference's geometry tolerance is scaled by the world-space width of a pixel.** This chain
  filters at two resolutions in the same frame, so that would mean two tolerances for one surface;
  it uses a tolerance relative to view depth instead (see §3).
