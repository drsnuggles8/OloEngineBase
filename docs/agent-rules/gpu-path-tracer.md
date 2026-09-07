# The GPU reference path tracer — what it is for, and the rules that keep it an oracle

Issue #1055 (#979 Phase 2). Code: `Renderer/Passes/GpuPathTracerPass.{h,cpp}`,
`Renderer/PathTracing/EmissiveTriangleTable.{h,cpp}`, `Renderer/PathTracing/GpuPathTracerTypes.h`,
`OloEditor/assets/shaders/GpuPathTracer.glsl`, `include/PathTracerSampler.glsl`. Tests:
`tests/Rendering/PathTracing/GpuPathTracer*Test.cpp`, `PathTracerSamplerParityTest.cpp`,
`ClosureV2SampleGpuParityTest.cpp`, `tests/Rendering/PropertyTests/GpuPathTracerFallbackTest.cpp`.

Read this before touching the tracer, the CPU reference it mirrors, or anything that consumes its
output as ground truth.

---

## 1. It mirrors the CPU tracer term by term, or it is not a reference

**Rule: a change to `PathTracer.cpp`'s transport, sampler dimension order, or light model is a change
to `GpuPathTracer.glsl` in the same commit, and vice versa.** The GPU tracer is comparable with the
CPU one because the two draw the *same* random numbers for the same (pixel, sample, dimension) and
spend them in the same order: pixel jitter, then per bounce the emissive NEE select + point, the BSDF
lobe + shape, and the Russian-roulette draw. `PathTracerSamplerParityTest` pins the sampler to the
bit; `GpuPathTracerDeviceParityTest` pins the whole Cornell box to a written-down budget (2% on
region means, 1% on the frame mean at 64 spp). A budget that has to widen is a bug in one tracer, not
a tolerance to raise: the noise the two share cancels in the difference.

Two consequences that are easy to break by accident:

- **The light density is one global constant.** Emissive triangles are selected uniformly by area
  over the whole set, so the pdf the BSDF-hit MIS side needs is `1 / total area` without knowing
  which triangle was hit. The GPU table (`EmissiveTriangleTable`) is built on the CPU from the same
  mesh sources the raster draw indexes, in extraction order; `GpuPathTracerContractTest` checks it
  against `ReferenceScene::GetEmissiveTriangles()` triangle for triangle.
- **Every hit shades with ITS closure.** `PtEvaluateBRDF` / `PtBsdfPdf` / `PtSampleBRDF` dispatch on
  the material's `ClosureVersion` the way `PBRClosureBSDF.h` does: the Legacy sampler (GGX-NDF
  importance sampling plus cosine, the v2 lobe probability) is mirrored in the shader, and both
  closures are pinned by their own Cornell parity test.
- **Textures follow one convention on both tracers:** level 0, bilinear, REPEAT; the colour maps
  (albedo, emissive) are sRGB-decoded per texel, the data maps (metallic-roughness, normal) are read
  linear; albedo rgb times the factor, metallic = blue and roughness = green, emissive rgb times the
  factor, the normal map in the triangle's analytic UV tangent frame (`ReferenceScene::ApplyNormalMap`
  and the shader's `PtApplyNormalMap` are one formula). Emissive records carry UVs and the map so NEE
  and the emitter-hit path see one radiance. The GPU indexes the descriptor heap itself
  ([vulkan-shader-heap-indexing.md](vulkan-shader-heap-indexing.md)); where it cannot, hits shade
  from the factors and masked geometry traces as solid, both counted.

## 2. Accumulation goes through the temporal-history registry and restarts on any camera change

**Rule: a path tracer cannot reproject, so any change of pose restarts the sum — detected bitwise
on the UNJITTERED view and projection.** `RenderPipeline` hands the pass `TemporalProjectionMatrix`;
handing it the jittered `ProjectionMatrix` makes a converging shot restart every frame with TAA on,
which reads as "the tracer never converges" rather than as an error.

The four RGBA32F planes (radiance sum + count, squared sums, first-hit albedo, first-hit normal) are
`TemporalHistoryEffect::PathTracer` entries acquired in `PopulateBlackboard` and extracted from the
pass's own attachments every frame. Three things about that are load-bearing:

- **The registry tokens are hashed into the graph fingerprint**, next to the ray-traced shadow ones.
  Every invalidation bumps a generation, and that bump is what makes the cached `Setup` drop the
  history handles it was bound to. Without it a restarted accumulation keeps adding to the sums it
  was told to forget.
- **A scene mutation is a dirty GPU Scene range.** `RecordTable::Commit` marks a slot dirty only when
  its bytes changed, so after the commit any non-empty range means an instance moved, a material
  factor was edited, a light or the environment changed. `Renderer3D::EndScene` turns that into an
  unscoped `InvalidateTemporalHistories(SceneMutated)`; the cause maps to the
  `TemporalHistoryDependency::SceneContent` bit, which only the path tracer's planes declare, so
  TAA/SSR/SSGI keep reprojecting by their own declaration rather than by a filter at the call site.
  The planes do not declare `Jitter`: a TAA or FSR2 toggle must not throw the sum away.
- **An animated scene restarts every frame, and the pass says so.** One moving entity dirties a
  range every frame, so the sum never gets past `SamplesPerFrame`; `ConsecutiveRestarts` counts it,
  the panel shows it, the log warns once after eight in a row.
- **The sample index is the accumulated count.** `N` frames at 1 spp and one frame at `N` spp draw
  the same sample indices, which is what makes a capped run reproducible whatever frame it began on
  (`GpuPathTracerDeviceParityTest.AccumulationOverFramesDrawsTheSameSequenceAsOneFrame`).

## 3. The fallback is structural; the reason is counted

`PathTracerColor` is declared only when the pass is enabled *and* its shader loaded, and the
shader is created only where `RenderCommand::SupportsRayTracing()`. Everywhere else the alias chain
never sees it and the frame is byte-identical (`GpuPathTracerFallbackTest`, the path every CI runner
takes). When the pass does run and cannot trace, it uploads a ZERO TLAS address and the shader
passes the input colour through and **carries the four history planes forward unchanged**: a
stood-down frame is a no-op on the accumulation, not a restart. Only the registry restarts the sum,
and the pass's per-pixel count mirrors the registry; a shader that zeroed the planes on its own
would leave the count claiming a converged image for a one-frame one.

`GpuPathTracerFallbackReason` is resolved most-fundamental-first and reported once per change, from
`ResolveAvailabilityForFrame` in the per-frame wiring (`RenderPipeline::ConfigurePassesForFrame`,
after the GPU Scene commit), so the verdict and the counters are fresh whether or not the graph
culls the pass. `Execute` adds the one reason only it can see: no target. Two are easy to miss:
`EmissiveTableNotGathered` (the setting flipped on between `BeginScene`, where the gather is decided,
and `EndScene`; one frame is skipped) and `PunctualLightsBeyondShaderBound` (a live light at a slot
past `OLO_PT_MAX_LIGHTS` lights the raster frame but not this tracer; counted).

A committed hit whose GPU Scene record is out of range or inactive (geometry the TLAS still holds
but the tables no longer describe) is a hit on a black opaque surface, not a miss: a miss would
credit the path with environment radiance through an occluder.

## 4. Things that bit while building it

- **`sampler` and `sample` are reserved words in GLSL.** A parameter named `sampler` fails with
  "unexpected SAMPLER, expecting RIGHT_PAREN", a local named `sample` with "unexpected SAMPLE", at
  the declaration, not at a use. The code uses `pathSampler` and `result`.
- **The SSBO namespace is full, and a `StorageBuffer` publishes itself at its binding on
  construction** (both backends). A buffer that is only ever reached by device address is created
  with `StorageBuffer::kNoBinding`: both backends then skip the construction-time and `Bind()`-time
  publication, so it never touches the shared indexed-binding state. Do not park such a buffer on a
  slot some other table owns; an arbitrary out-of-range number is not a way out either, because
  `VulkanBindingState` warns on it.
- **Never `SetData` into a by-address buffer a submitted frame may still read.** `SetData` on Vulkan
  writes the persistent allocation the shader's device address points at (the snapshot mechanism
  serves bound SSBOs, not addresses), so the previous frame's draw would read a torn table. The
  emissive and material-texture tables therefore publish a *fresh* buffer whenever their bytes change
  and drop the old `Ref`: the backend's deferred reclaim keeps the old allocation alive until the GPU
  is past every frame that could reference it, and the pass resolves the address every frame.
- **Vulkan off-screen targets are top-down; GL's are bottom-up.** The shader derives its pixel seed
  and primary ray from the CPU film's row convention (row 0 = top) under `#ifdef OLO_VULKAN`, and the
  device test's readback needs no flip. The primary ray uses `inverse(P * V)` in the engine's GL
  clip convention, deliberately *not* `RHIProjectionSeam`'s adjusted inverse: it never derives a ray
  from a backend texture coordinate.
- **A `.glsl` test probe in `assets/shaders/tests/` includes `include/…` relative to the shader
  ROOT** (a production shader in `compute/` names `../include/…`), so validating a probe with glslc
  needs `-I <root>` and validating a production shader must not pass one.

## 5. Running it

- **Editor:** Post-Process panel → "GPU Path Tracer (reference)", or the MCP field registry group
  `pathtracer` (`GpuPathTracerEnabled`, `…SamplesPerFrame`, `…MaxSamples`, `…DebugView` …). Vulkan
  only; the panel and `olo_pathtracer_stats` show the sample count, the counted limits and why the
  tracer stood down.
- **AOVs:** `OloEditor/assets/benchmark/manifests/material-lab-furnace.pathtracer.yaml` captures
  `PathTracerAccum` (rgb = sum, a = count), `PathTracerAlbedo`, `PathTracerNormal`,
  `PathTracerVariance` as `.hdr` through `olo_benchmark_capture` under `--rhi=vulkan`. The warm-up
  frames are the accumulation.
- **Parity:** `OloEngine-Tests.exe --gtest_filter=GpuPathTracerDevice.*` on a machine with an RT
  device writes `assets/tests/visual/GpuPathTracer_CornellBox.png` beside the CPU reference frame
  and prints the per-region numbers.

Sphere-area lights are spherical emitters on both tracers: `PathTracer.cpp`'s `ViewSphereLight`
defines the model (radiance reproducing the raster's diffuse irradiance at the receiver, sampled by
solid angle with MIS, visible to camera and bounce rays, non-occluding) and the shader mirrors it.
Out of scope, on purpose: an RT pipeline / SBT, a denoiser, mip selection (level 0 everywhere).
