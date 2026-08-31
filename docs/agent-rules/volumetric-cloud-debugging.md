# Debugging a volumetric cloud field that renders as a "uniform veil"

Origin: issue #633 (weather director / time-of-day / volumetric cloudscape).
The first live run of the cloud raymarch produced a featureless pale sky at
every coverage setting. Four *different* root causes produced nearly identical
"uniform veil / no cloud structure" symptoms, plus two observation traps that
made correct code look broken. Written down so the next volumetric feature
doesn't re-pay the afternoon.

It has grown since: causes five and six came from the full suite rather than
the editor, and causes seven and eight from issue #723 (volumetric shadow
maps), which also added the observation that separates a real self-shadowing
signal from every veil below. Eight causes, one symptom — read the whole list
before concluding which one you have. Cause eight is the one to suspect when
the feature is *provably* correct upstream and the frame still will not move.

## The four look-alike causes (all were real, all needed separate fixes)

1. **The pass output was silently dropped downstream.** The candidate-ladder
   rule (glsl-shaders.md §9) is *every* downstream consumer, not just the
   immediate neighbors: `CloudsColor` was added to the Fog and Precipitation
   ladders, but with both of those passes disabled every later pass
   (ChromAb → … → Final) fell back past the clouds output. Diagnosis that
   pinned it: `CloudsRaw` had content while the final frame didn't.
2. **A remap whose low edge sits outside the operand's value range.** The
   Nubis-style base shape `remap(perlinWorley, fbm - 1.0, 1.0, 0, 1)`
   compresses the whole field into ~[0.65, 0.95] because `fbm - 1` is deeply
   negative — no downstream coverage threshold can carve holes in a field
   that never goes low. The erosion floor must sit *inside* the noise's
   actual range (`fbm * 0.85` here). Generalization: any
   `remap(x, lo, hi, …)` where `lo` is provably below `min(x)` is a no-op
   dressed as shaping — check the operand's real distribution, not the
   textbook formula.
3. **A procedural input texture with the wrong statistics.** The CPU weather
   map's coverage channel was contrast-stretched around an *assumed* mean of
   0.5; the FBM's true mean was ~0.75, so the channel saturated high and the
   sky never cleared anywhere. Fix: normalize the generated field against its
   own measured min/max instead of assumed constants. A GPU-side shader probe
   (`o_Color = vec4(weather.r, noise.r, fbm, 1)`) exposed the distribution in
   one capture — measure inputs before tuning the math that consumes them.
4. **Feature scale vs. field-of-view mismatch.** With ~7.5 km coverage
   features (30 km weather-map repeat) and the editor camera's 30° FOV, the
   visible sky patch (~2 km of layer) sits entirely inside ONE weather blob —
   a *working* field reads as a uniform veil from almost every viewpoint.
   Before concluding the field is broken, compute
   `visible layer extent ≈ 2·tan(fov/2)·(layerHeight − cameraHeight)` and
   compare against the feature size; view the layer from above, or shrink the
   authored scales, to see the actual structure.

## The fifth cause (found in the full suite, not the live editor)

**A weather-director test leaks storm state into every later visual test.**
`WeatherSystem::ApplyImmediate/Tick` writes the process-global Renderer3D
fog/wind/precipitation/snow settings *by design*; the `RendererAttachedTest`
snapshot does NOT cover those structs (the exact trap
VolumetricFogVisualEvidenceTest documents for `FogSettings`). A matrix test
ending on Storm left storm fog over the water/bloom/planar-reflection
goldens — 8 cross-test failures whose symptoms (RMSE 20+, "feature reads
inert") looked nothing like their cause. Any test that drives the weather
director must snapshot/restore all four settings structs + reset the
cloudscape render state in TearDown. Diagnosis that pinned it: the 8 tests
passed in isolation and failed only when the atmosphere matrix ran first
(gtest alphabetical order made the new `Atmosphere*` suite run before all of
them).

## The sixth cause (also found in the full suite — the reverse leak)

**Goldens rebased standalone pin the settings-UN-applied renderer state.**
`Renderer3D::ApplyRendererSettings()` derives the live depth-prepass /
Forward+ / culling flags from `RendererSettings` — but nothing derives them
in a pristine test process (the #534 boot divergence), so a standalone rebase
runs with the prepass off while the full suite runs with it on (any earlier
test that calls apply — `RendererSettingsBootstrapTest`'s TearDown does —
flips the flags process-wide, and the editor itself applies at boot). The
delta hides in the *textured ground*, not the sky: high-frequency texture
edges (grid lines) and far-ground mip bands shift a few grey levels, which is
sub-threshold on bright day frames but RMSE ~22 on dark night frames. The
un-applied state also renders those grid lines z-precision-dashed — the
applied state is both the editor-equivalent one and the stabler one. Rule:
a visual-evidence fixture must call `Renderer3D::ApplyRendererSettings()`
after `EnableRendering` (order-independence), and goldens must be rebased in
that state. Diagnosis that pinned it: identical RMSE digits across runs
(deterministic, not noise), suite bisection down to a single read-only test
whose TearDown applies settings, and a per-band pixel diff showing the top
(sky) third byte-identical while only ground texture edges moved.

## The seventh cause: a spatially CONSTANT term inside a light-path integral

Origin: issue #723 (volumetric shadow maps — self-shadowing for clouds and fog
volumes). This one was designed around rather than paid for, but it is the
uniform veil the *next* light-path feature will produce, so it belongs here.

The moment you add an integral along the LIGHT direction — a light march, a
volumetric shadow map, a deep shadow map — every term you fold into it is
multiplied by a path length. A term that varies in space carries occlusion
information. A term that is spatially CONSTANT does not: it contributes
`exp(-k · L)` where `L` is the path length through the *fitted volume*, not
through the medium. That is a veil, and a worse one than a static offset,
because `L` changes whenever the volume is refitted — as the sun rotates, as
the camera translates, as a fog volume appears — so it reads as an unstable
haze whose cause is nowhere near the code that changed.

`FogSettings::AbsorptionCoefficient` is exactly such a term in this engine.
`FroxelFogScatter.comp` adds it to every froxel's extinction as a floor, which
is right for the VIEW-ray integration (`FroxelFogIntegrate.comp` walks metres
per slice, and the floor is what keeps a nearly-empty froxel from dividing by
zero). Folded into a light path of a few hundred metres it is
`exp(-0.02 · 300) ≈ 0.0025` — the entire volume goes black, uniformly, and the
strength knob merely dims the blackness. `VolumetricShadow_Generate.comp`
integrates `combinedDensity` and deliberately not the floor; the comment there
says so, because the "missing" term looks like an oversight to the next reader.

**Rule:** only spatially varying density belongs in a light-path integral. A
constant belongs in the view-ray integration, where its path length is the
quantity being measured rather than an artefact of a fitted box.

**The probe that separates it:** rotate the light 90° and re-capture. Real
occlusion moves with the light — the dark side of the medium swaps ends. A
constant-term veil only changes magnitude, because all that moved was `L`.

## The eighth cause: the term you changed is not the term the pixel shows

Origin: issue #723, found live over MCP after the first cloud A/B pair came back
visually identical while the shadow VOLUME was provably correct.

A volumetric shader composites several radiance terms, and a change usually
touches exactly one of them. If another term dominates the composite, a
correct and large change to yours is invisible — and every diagnosis aimed at
"why is my term zero?" is aimed at the wrong question, because it is not zero.

For the cloudscape the terms are the SUN in-scatter (Beer-Lambert, phase
weighted, shadowed) and the AMBIENT term (`u_CloudAmbient * mix(0.25, 0.75,
heightFrac)`, unshadowed by construction — a shadow map fitted to the sun
direction says nothing about sky light). Self-shadowing modulates only the
first. Measured on the same enabled/disabled pair, same scene, same frame:

| `AmbientScale` | frame delta |
|---|---|
| 1.0 (stock) | 0.34 grey levels (0.32%) |
| 0.1 | 1.62 (1.63%) |
| 0.0 | 2.68 (2.61%) |

Nothing about the shadowing changed across those rows. Only how much of the
pixel it was allowed to account for.

**Two more multipliers sit in front of the same term**, and all three stack:

- **Phase.** The sun term carries `cloudPhase(cosTheta, g)`. A camera looking
  across the light is on the weak part of a forward-scattering lobe, so the
  term it modulates is small before shadowing is applied at all. Looking toward
  or away from the sun roughly doubled the same differential.
- **Saturation from the other direction.** `cloudLightOpticalDepth` marches
  1400 m of cone. At the stock `Density` of 1.0-1.6 that alone reaches an
  optical depth of ~20, so `exp(-od)` is already ~1e-9 and there is nothing
  left for a shadow map to remove. A DENSE deck is the one regime where
  self-shadowing provably cannot change a pixel.

So the regime where the effect is visible is bounded on both sides: dense
enough to see, thin enough not to saturate, with the sun low enough that the
light path leaves the cone's reach. Live sweep of the deck underside, moderate
density, ambient isolated — sun elevation vs frame delta: **60 deg 0.16%,
30 deg 0.21%, 12 deg 17.0%, 5 deg 32.9%.** That two-orders-of-magnitude swing
is the signature of a working directional shadow, and it is also why a test
that picks one sun angle can conclude anything it likes.

**How to tell this apart from a broken feature — in one step.** Capture the
INTERMEDIATE, not the frame. `olo_render_capture_target` on the producer
(`VolumetricShadowVolume`, imported for exactly this reason) answers "does the
data exist and does it have structure?" independently of every multiplier
downstream. In this case slice 2 of the cloud cascade came back nearly uniform
and slice 62 came back as a crisp cloud-shaped field — the running integral
building along the light ray, unambiguous — which turned "the feature does
nothing" into "the feature works and the composite hides it" without a single
shader edit. Do this BEFORE the binary-search debug-colour loop below; it is
one call and it splits the problem in half.

**Do not fix this by rebalancing the look.** Turning the ambient term down to
make your feature visible is changing the art direction to flatter the code.
Author the *test* in the isolating configuration (and say why in the test),
and report the stock-configuration magnitude honestly.

## Telling "darker" from "directionally darker"

The generalisation of the above, and the observation every self-shadowing
feature should be verified with, because ALL the look-alike causes in this
document produce "the medium got darker" and only the real thing produces
"the medium got darker ON THE SIDE AWAY FROM THE LIGHT".

Capture an A/B pair (feature off / feature on) from two aspects in the same
scene — one facing the light, one facing away — and assert on the DIFFERENCE
of the differences: the shaded aspect must lose more than the lit one. For
clouds that is a ground camera looking up at the deck's underside versus an
aerial camera looking down at its tops; for a fog volume it is the sunward end
versus the far end, with the light crossing the frame so the view-ray
extinction (which is symmetric) cannot fake the signal.

Why a differential rather than a golden: it cancels the driver, the exposure,
the phase function and the sun colour in one step, none of which a fixed-image
RMSE can separate from the effect under test — and it adds no new golden to
flicker. `VolumetricShadowVisualEvidenceTest` is the worked example.

## The three observation traps

- **"Toward the light" is the DARKEST direction through a medium, not the
  brightest — and the lower the sun, the more so.** A camera pointed at a
  5-degree sun looks ALONG the light path, through the greatest thickness of
  medium in the frame. The lit faces are the ones you see with the light BEHIND
  the camera. This inverted an assertion in `VolumetricShadowVisualEvidenceTest`
  ("the underside must darken more than the sun side"): the correct
  implementation measured 9.43 on the sun side against 6.48 on the underside,
  i.e. the exact reverse of the naive expectation, and the test failed on a
  working feature. If you are about to encode "the lit side" and "the shadowed
  side" as camera poses, work out which is which at the sun elevation you are
  actually testing.

- **`olo_render_capture_target` shows the EDITOR viewport camera's frame**,
  not the pose passed to `olo_screenshot`. A capture of a sky-effect buffer
  taken while the viewport camera looks at the ground is legitimately black.
  Reposition the *editor* camera (`olo_camera_set_pose` — pitch is in
  DEGREES, negative = up) before capturing pass-internal buffers.
- **Shader hot-reload + `#include` staleness is unproven territory** — when a
  reload of an including shader doesn't visibly change behavior after an
  include-only edit, cold-restart the editor before concluding the edit is
  ineffective (the restart also refreshes lazily-generated resources like the
  noise volumes, removing a second variable).

## The technique that worked (use it first, not last)

The binary-search shader-debug loop over MCP, ~40 s per hypothesis:
unconditional debug color at the top of `main()` → after the early-out →
visualize intermediate values (slab hits, camera reconstruction, raw texture
taps) — each iteration is an on-disk edit + `olo_shader_reload` +
`olo_render_capture_target {forceFrame:true}`. It converted "the sky is
uniformly wrong" into four *specific* facts in under an hour, where reading
the code found nothing (all four bugs were individually plausible-looking).

## Residual knowledge

- `kCloudExtinction` (CloudscapeCommon.glsl) is shared by the raymarch, the
  ground-shadow compute, the volumetric-shadow generator, and the CPU mirror in
  `CloudDensityMathTest.cpp` — the test re-declares it literally, so retuning it
  is a two-file edit by design.
- The cloud raymarch's light path is SPLIT at `cloudLightNearRange()` (1400 m,
  or the layer thickness if thinner): the cone march owns `[0, near]` and the
  volumetric shadow map owns everything beyond. They compose exactly rather
  than overlapping, because the map stores optical depth measured FROM the
  light — so sampling it at the point where the cone stops IS the remainder.
  Move the cap and you move both halves; drop the map and the layer above the
  cap stops shadowing anything, which is what it did before #723 and why a
  kilometre-thick deck was lit identically top and bottom.
- The volumetric shadow volume has NO temporal term, deliberately: a fixed
  midpoint quadrature over a fixed slice grid is a pure function of the frame's
  settings. That is what lets #723 satisfy "no golden-test flicker" by
  construction instead of by tuning a history weight. If you ever add jitter
  there, make it a function of the cell rather than of the frame index.
- In-scatter radiance integrates to several times the sun intensity through a
  kilometre-scale medium; artistic single-scatter albedo factors (~0.5) on
  the sun term are load-bearing, not cosmetic.
- The editor's viewport camera FOV is 30° — half the usual game FOV. Cloud
  scales tuned "to look right" through it will read twice as large in a
  60° runtime camera.

## A different signature: cloud puffs in front of terrain

Origin: issue #987. This looked like a temporal-history leak, but the puffs
were already present in `CloudsRaw`. Two depth-classification errors in the
half-resolution raymarch combined at the silhouette:

1. One cloud invocation represents a 2x2 footprint in the full-resolution
   scene depth. A filtered or centre sample can select sky even when another
   pixel in that footprint contains terrain. For conventional depth, fetch
   all four texels and use their minimum as the conservative occluder; clamp
   the coordinates so odd-sized viewports duplicate the final row or column.
2. Sky is the exact clear sentinel `1.0`, not “anything above `0.9999`”. With
   Drift's 1000 m far plane, valid ridge depth reached about `0.999904`, so a
   broad epsilon classified the terrain itself as sky.

The decisive probe was a same-frame `CloudsRaw` capture plus four exact
`SceneDepth` texels under one affected half-resolution pixel. Pin both cases
in an `R32F` render-pass contract: RGBA8 cannot represent the far-depth values
that distinguish valid geometry from the clear sentinel.
