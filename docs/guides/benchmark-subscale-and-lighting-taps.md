# Sub-scale capture and lighting taps (#1526)

Two capture features of the benchmark core (`Renderer/Benchmark/`), both reachable from the
test-binary host (`--olo-capture-manifest`) and the editor host (`olo_benchmark_capture`).
The manifest schema they extend is in [renderer-benchmarks.md](renderer-benchmarks.md).

## Sub-scale capture

**Two mechanisms make the internal size differ from the display size. Name one per manifest.**

| mechanism | manifest | what the targets hold | what the capture reads |
|---|---|---|---|
| upscaler (FSR1, FSR2) | `RendererSettings.Upscale: Quality` (+ `UpscaleTechnique`) | the scene band is **allocated** at the preset's scale; the post chain is display size | every target whole: scene-band AOVs at internal size, `Beauty` at display size |
| dynamic render scale | `Output.RenderScale: 0.667` with `Upscale: Off` | every target stays display size; the whole chain draws into its `[0, render)` corner, with no upscaler | a display-sized target as its rendered corner; any other target whole |

Combining the two is refused at parse time: the dynamic scale would shrink the upscaler's
already-reduced band into a corner the upscaler does not read. `Output.RenderScale` must be a
finite value in `[0.25, 1.0]` (the render graph's clamp range), so a manifest cannot name a
scale the capture did not run.

`result.json` says which mechanism ran, under `output.actual`:

| field | meaning |
|---|---|
| `renderWidth` / `renderHeight` | the size the scene was really rasterized at, under either mechanism |
| `displayWidth` / `displayHeight` | the presented size |
| `renderScale` | the dynamic scale in force (1.0 under an upscaler) |
| `upscalerRenderScale` | the `Upscale` preset's scale (1.0 when Off) |
| `internalSource` | `upscaler-scene-band`, `dynamic-render-scale` or `native` |
| `upscaler.resolved` / `.fallback` | what reconstructed display resolution (`native`, `spatial`, `temporal`) and, for a Temporal request that ran FSR1, why (`backendNotOpenGL`, `msaaResolved`, ...); `null` if no frame resolved it |

Each attachment also records `textureWidth` / `textureHeight` and `croppedToRenderRegion`.

Before #1526, `renderWidth` read only the dynamic scale, so every upscale capture reported its
0.667 band as a native render (the #1338 `gl-controls` results say 1920 × 1080 for a
1280 × 720 `SceneColorHDR`), and a dynamic scale below 1.0 was refused because the whole-texture
read would have captured the dead margin.

**FSR2 is not run-twice deterministic.** Its lock decay reads the wall clock by contract, so a
`Temporal` capture's `Tolerance.RepeatRmse` must allow for it. On Vulkan a Temporal request runs
FSR1 and records `fallback: backendNotOpenGL`.

## Lighting taps

**A tap replaces the lit colour with one term of it, on every raster path and both backends.**
`SceneColor` then holds that term, linear HDR, Rec.709. Selected by
`RendererSettings.LightingTap` in a manifest, `olo_render_set_debug_view` in a live editor, or
`PostProcessSettings::LightingDebugTap` in code.

| tap | holds |
|---|---|
| `DirectDiffuse`, `DirectSpecular` | f · L · cos · visibility over the lights the raster loop owns |
| `IndirectDiffuse` | the ambient rung × its visibility (material AO × screen-space AO), plus a traced diffuse tier (ReSTIR GI) |
| `IndirectSpecular` | the ambient rung's specular × the same visibility |
| `Remainder` | everything outside the split: ReSTIR DI direct, ReSTIR PT indirect, transmission, emission |
| `ShadowVisibility` | the primary directional light's full visibility (shadow map, VSM or traced; × cloud and contact shadow), greyscale [0,1]; 1 where no directional light shades |
| `ReflectionHitDistance` | not a colour tap: SSR writes its hit distance (view-space metres, 0 = no hit) into `SSRGuide`'s alpha in place of its confidence |

**The five radiance taps partition the lit colour**: their sum is the tap-off `SceneColor` to
fp16 rounding. Two exceptions, written down rather than hidden: on a skin pixel the specular taps
are before the skin profile's specular tint, and a transmissive (glass) material blends its
environment over the tap.

**Reflection confidence is always available**: `Source: SSRGuide, Derive: channel-a` (SSR's
arbitration confidence, #1057). Both reflection AOVs exist on Deferred with SSR on only, because
the screen-space reflection tier runs there only.

**Which surfaces carry a tap.** On Deferred, every G-Buffer surface: the tap is applied in the
lighting pass (per sample under MSAA). On Forward and Forward+, every lit forward shader: meshes
(`PBR_MultiLight`, `_Skinned`), heightfield and voxel terrain, foliage cards and impostors. The
shaders that compose their own sum record the same five terms from their own halves; voxel terrain
derives its diffuse half from the split closure only while a tap is selected. Surfaces with their
own shading (sky, water, particles, grooms) keep their colour, so mask by what you are measuring.

**Zero cost and bit-identical when off.** The tap is a float in the camera block's former padding
lane, set on the main view only (a mirror, probe or shadow view renders its ordinary frame). The
shaders record the terms with plain stores and return the lit colour unchanged when no tap is
selected. A selected tap also stops skin diffusion, which adds into scene colour; that gate is how
the tap reaches the frame-graph fingerprint.

## Evidence

Measured on 2026-10-01 on the RTX 4090 box (Debug, clang-cl), #1526's branch.

**Sub-scale, OpenGL (artefact-backed):**
`UpscaleFramingEvidenceTest.BenchmarkCaptureReadsTheRenderedFrameAtEveryInternalScale` drives the
capture core at 1024 × 683. FSR1 and FSR2 Quality on Forward and Deferred read Beauty at 1024 × 683
with native framing and `SceneColor` at the 683 × 455 band, resolved `spatial` / `temporal`. A
0.5 dynamic scale on Forward reads Beauty and `SceneColor` as the 512 × 341 corner with every
subject at half its native position and size, both before and after an upscaler has run
(`SubScaleCapture_GL_*.png`). The pre-#1526 whole-texture read of the same frame fails the corner
check, and Deferred refuses the scale. Partial-integrated GL numbers are in
[integrated-renderer-budgets-1338.md](../analysis/integrated-renderer-budgets-1338.md).

**Sub-scale, Vulkan (live, editor `olo_benchmark_capture`):** the integrated Forward FSR1, FSR2 and
0.667-scale manifests and the Deferred FSR1 and FSR2 manifests all completed with no validation
message and no warm-up timeout. Every upscale run reports `renderWidth 1280`, Beauty 1920 × 1080
and scene-band AOVs at 1280 × 720; FSR2 requests resolve to `spatial` with
`fallback: backendNotOpenGL`; the 0.667 run reads all three attachments as the 1280 × 720 corner,
and its Beauty shows the whole scene.

**Lighting taps, OpenGL (artefact-backed):** `LightingTapEvidenceTest` — the five radiance taps sum
to the lit colour on Forward, Forward+ and Deferred from two angles, under Deferred MSAA 4 and under
FSR1 (0 of 39 580–147 516 lit pixels outside 1 % + 2e-3, worst 0.09 %); dropping any one term fails
the check; every term carries energy and the shadow tap shows shadowed and sunlit floor
(`LightingTap_GL_*.png`: lit, the five terms, shadow, |sum − lit| × 64). SSR on a glossy floor:
21 067 pixels with confidence > 0.1, 90 % of them with a hit distance in the tapped run (mean
1.6 m, max 13.8 m; the remainder is per-frame ray jitter between the two runs), and the hit-distance
tap leaves `SceneColor` bit-identical.

**Lighting taps, Vulkan (live):** MaterialLab in the editor, TAA off, `SceneColor` probed on a 9 × 7
grid for each tap: 63 of 63 lit probes partition within 2 % on Forward, Forward+ and Deferred (worst
0.075 %).

**Taps off is the shipping frame:** see the PR's shader-swap A/B (the same binary rendering the base
commit's shaders and these, tap off, compared hash for hash).
