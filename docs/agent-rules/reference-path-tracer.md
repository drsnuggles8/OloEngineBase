# The offline reference path tracer — how to use it, and what it cannot tell you

Rules for validating a GI / lighting / BRDF change against the CPU reference tracer (issue #709), and for extending it.
Applies to: `OloEngine/src/OloEngine/Renderer/PathTracing/`, tests in `OloEngine/tests/Rendering/PathTracing/`.
Goldens and contract tests do not answer "is it correct?"; this unbiased integrator, shading with the engine's own BRDF, does.

## Rules

1. **Keep a render bit-identical at any thread count.** `PathSampler` stays stateless (pure function of pixel, sample index, dimension, seed) and one thread sums a pixel's samples in ascending order; parallelise over rows only. `PathTracerCornellBoxTest` asserts an exact hash including parallel vs sequential. [Details](reference-path-tracer-details.md#2-determinism-is-a-contract-not-a-nicety)
2. **Keep the CPU tracer headless and the CI gate intact.** `GpuPathTracerPass` (#1055, [gpu-path-tracer.md](gpu-path-tracer.md)) mirrors it and does not replace it. [Details](reference-path-tracer-details.md#1-what-it-is)
3. **Port the renderer's BRDF quirks, do not correct them.** `ReferenceBRDF.h` transcribes `PBRCommon.glsl` function for function: `distributionGGX` denominator clamped with `EPSILON` 1e-4, Schlick-GGX `k = (r+1)²/8` in `cookTorranceBRDF`, `kD = 1 - F` with F at the half vector. Mirror every GLSL BRDF function, including `visibilitySmithGGXCorrelated`. Pinned by `ReferenceBRDF.GgxNdfPeakIsEpsilonClampedAtLowRoughness`. [Details](reference-path-tracer-details.md#3-the-brdf-port-is-deliberately-unfaithful-to-textbook-math)
4. **Never reuse an evaluation-side guard inside a density.** `PdfGGX` uses the unclamped `DistributionGGXSamplingDensity`; the BRDF uses clamped `DistributionGGX`. Sampling roughness is floored at `MIN_ROUGHNESS` (0.04); evaluation roughness is not. `ReferenceBRDF.GgxSamplingMatchesItsDensity` must keep low-roughness cases (0.05, 0.1, 0.2). [Details](reference-path-tracer-details.md#a-pdf-describes-the-sampler-never-the-integrand)
5. **When you edit `PBRCommon.glsl`'s BRDF, edit `ReferenceBRDF.h` too.** `ReferenceBRDFGpuParityTest` (probe `assets/shaders/tests/PbrBrdfParityProbe.glsl`, 1% tolerance) is GPU-gated and skips headless; a green headless suite is not evidence the port is current. [Details](reference-path-tracer-details.md#the-anti-drift-guard)
6. **Compare the physical quantity, not composited pixels.** Use `PathTracer::EstimateIrradiance`, whose units match the DDGI atlas (`E = π·Σ(w·L)/Σ(w)`); if units are not pinned, pin them first. [Details](reference-path-tracer-details.md#compare-the-physical-quantity-not-the-pixels)
7. **Build both worlds from one description, and match the light model.** `DDGIReferenceParityTest` uses a single box table; port `calculateAttenuation` and pack `ReferenceLight::AttenuationParams` as `Scene.cpp` does (`(1, 0, m_Attenuation, m_Range)` for a point light). Differ from raster in transport, never in the light model. [Details](reference-path-tracer-details.md#build-both-worlds-from-one-description)
8. **Compare at relocated probe positions.** Read `DDGIProbeUpdatePass::GetProbeRecords()[i].OffsetN` and use `DDGI::ProbeWorldPosition`, not grid coordinates. [Details](reference-path-tracer-details.md#compare-positions-not-grid-coordinates)
9. **Assert pairwise ordering of the field as well as magnitude,** skipping pairs the reference calls a tie. [Details](reference-path-tracer-details.md#assert-the-fields-shape-not-only-its-magnitude)
10. **Keep comparison scenes inside the subset both worlds express.** `ReferenceSceneBuildOptions::MaterialMapProvider` and `::EnvironmentCubemap` default to absent (`LightmapSkyAndTextureBake.AProviderThatSuppliesNoMapsChangesNothingBitForBit`); only a bake may supply them. Maps and cubemaps sample at level 0 with the reference's own filter (ADR 0022). The glTF alpha mode is always mirrored. [Details](reference-path-tracer-details.md#1-what-it-is)
11. **Render the reference in both shading models.** `ReferenceMaterial::LambertianDiffuseOnly` is for DDGI's diffuse-only cache and nothing else; assert DDGI against the Lambertian one and report the cookTorrance gap separately. [Details](reference-path-tracer-details.md#the-methodological-point-which-generalises)
12. **Keep the DDGI bounce gather through `ddgiGatherIrradiance(..., volumeMargin, intensity)` with a one-probe margin.** The lit passes keep the zero-margin hard test. Fitting a volume to air-only killed multi-bounce (#751, [ADR 0007](../adr/0007-ddgi-hit-point-cache-gather.md)); `DDGIProbeUpdatePass::GetBounceCoverage()` returns -1 for "nothing to measure" and the inspector warns below 50%. Before relaxing a guard that "also keeps X safe", derive X's real bound; turning a dead path on is a review of every guard it should have. [Details](reference-path-tracer-details.md#status-fixed-751)
13. **Give a furnace scene `MaxBounces >= 2`.** The environment is collected only on ray escape; `MaxBounces == 1` is the direct-only reference. [Details](reference-path-tracer-details.md#6-things-that-will-bite-you)
14. **Do not trace under an environment with a concentrated emitter.** The environment is never next-event-estimated; it needs environment NEE and MIS first. [Details](reference-path-tracer-details.md#6-things-that-will-bite-you)
15. **Treat 8-bit sky cubemaps as linear and material textures as sRGB-stored.** `OpenGLTextureCubemap` uses `GL_RGB8`/`GL_RGBA8`; `ReferenceTexture::FromRgba8` applies the EOTF itself. Count unreadable (block-compressed) textures via `ReferenceTextureCaptor::GetStats().Failed`. [Details](reference-path-tracer-details.md#6-things-that-will-bite-you)
16. **Check emitter winding and use `ReferenceSceneFixtures.h` helpers.** `AddQuadGeometry` derives the normal from `cross(p1-p0, p2-p0)`; a wrong-facing emitter renders black. [Details](reference-path-tracer-details.md#6-things-that-will-bite-you)
17. **Leave `MaxRadianceClamp` off for ground truth** (it is a bias), and keep NEE-off as a real cross-check (`PathTracerFurnace.NextEventEstimationDoesNotBias`). `AddInstance` rejects non-uniform scale on purpose. [Details](reference-path-tracer-details.md#6-things-that-will-bite-you)
18. **Assert on region means, never per-pixel values,** and measure the raster path's own frame-to-frame noise first (`live-verification-noise-floor.md`). [Details](reference-path-tracer-details.md#6-things-that-will-bite-you)
19. **Make energy claims on the frame mean, not the max.** The max of a Monte Carlo image is a noise statistic (`CornellFurnaceCreatesNoEnergy`: peak 1.069 at 128 spp, 0.992 at 256); bound the tail as a fraction, keep the per-pixel ceiling at 1.5. Raise resolution for spatial assertions and samples for photometric ones. [Details](reference-path-tracer-details.md#7-sample-count-is-sometimes-a-correctness-parameter)
20. **Run the evidence flavour in a Release build.** `--olo-pathtracer-evidence` writes converged 192x192 / 256 spp head-on and raking PNGs (Debug is about 40x slower). The default PNGs are diagnostics, not goldens. Know what a correct frame looks like: dark ring around the ceiling emitter, red-left / green-right block faces, a near-uniform furnace. [Details](reference-path-tracer-details.md#8-evidence)

## Commands and checks

Converged, human-readable evidence frames (Release build):

```
--olo-pathtracer-evidence
```

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[reference-path-tracer-details.md](reference-path-tracer-details.md).
