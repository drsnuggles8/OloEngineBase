# Debugging a volumetric cloud field that renders as a "uniform veil"

Origin: issue #633 (weather director / time-of-day / volumetric cloudscape).
The first live run of the cloud raymarch produced a featureless pale sky at
every coverage setting. Four *different* root causes produced nearly identical
"uniform veil / no cloud structure" symptoms, plus two observation traps that
made correct code look broken. Written down so the next volumetric feature
doesn't re-pay the afternoon.

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

## The two observation traps

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
  ground-shadow compute, and the CPU mirror in `CloudDensityMathTest.cpp` —
  the test re-declares it literally, so retuning it is a two-file edit by
  design.
- In-scatter radiance integrates to several times the sun intensity through a
  kilometre-scale medium; artistic single-scatter albedo factors (~0.5) on
  the sun term are load-bearing, not cosmetic.
- The editor's viewport camera FOV is 30° — half the usual game FOV. Cloud
  scales tuned "to look right" through it will read twice as large in a
  60° runtime camera.
