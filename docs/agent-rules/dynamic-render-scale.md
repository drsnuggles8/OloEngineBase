# The dynamic render scale: sample in texture uv, reconstruct in frame uv

Applies to every screen-space pass and to anything that maps a projected point
to screen texels. Issue [#1537](https://github.com/drsnuggles8/OloEngineBase/issues/1537).

`Renderer3D::SetRenderScale(s)` keeps every display-sized target at display
size and draws the frame into its `[0, render)` corner, on all three paths and
both backends. The bounds are `render / physical` per axis (`DRSUBOData`,
`RenderGraph::GetRenderScaleBounds`), `(1, 1)` at scale 1.0.

## Rules

1. **Sample inputs 1:1 at texture uv; convert to frame uv only for UV-to-NDC.**
   `include/RenderScaleBounds.glsl`: `oloFrameUV(texUV)` before
   `uv * 2 - 1`, `oloTextureUV(frameUV)` after projecting back, and
   `oloInRenderedFrame` instead of a `[0, 1]` on-screen test. A velocity is a
   frame-uv delta: scale it by `oloRenderScaleBounds()` before it moves a
   texture uv. A world radius converted to pixels through the target height
   is multiplied by `bounds.y` (SSAO, skin diffusion).
2. **A shader whose binding 33 is a sampler defines `OLO_RENDER_SCALE_BOUNDS`
   before the include.** The deferred lighting shaders read it from
   `u_LightingFlags.yz` (filled from the G-Buffer's active viewport); the
   include otherwise declares the DRS UBO at 33. An unfilled block (all zero)
   reads as scale 1.0.
3. **Constants the CPU builds from the screen size use the rendered extent.**
   GTAO and sphere-proxy AO divide `NDCToViewMul` by the bounds; the occlusion
   cull's `HZBUVFactor` is multiplied by them, and a scale change drops the
   retained pyramid; the Forward+ grid and the reflection-probe clusters tile
   the rendered extent (`SceneRenderPass` sizes both from the shaded target's
   active viewport every frame). A fragment's `gl_FragCoord` is in the
   corner, so a tile scale built from the storage size sends it to the wrong
   cluster.
4. **The G-Buffer takes the render viewport from `SceneRenderPass` every frame**
   (`BeginSceneFrame`); the base `ApplyRenderViewport` only reaches `m_Target`.
   A pass that sets its own viewport after `Bind()` reads
   `GetActiveViewportWidth/Height`, never the specification.
5. **Not covered: virtual geometry**, which sizes its raster, cull and HZB from
   the G-Buffer storage ([#1822](https://github.com/drsnuggles8/OloEngineBase/issues/1822)); it changes nothing at scale 1.0.

## How to check

- `DynamicRenderScaleEvidenceTest`: two cubes and a point light's pool at
  scales 1.0, 0.75 and 0.5 on Forward, Forward+, Deferred and Deferred MSAA 4,
  in both test processes. Each subject must sit at native times the scale.
- `UpscaleFramingEvidenceTest.BenchmarkCaptureReadsTheRenderedFrameAtEveryInternalScale`:
  the benchmark capture core's crop on Forward and Deferred.
- `ShaderUnitRenderScaleBoundsTest`: the GLSL mapping, including the zero-bounds
  guard.
- Before adding the include to a shared header, list the shaders that reach
  that header and `grep -n "binding = 33"` them: one that declares a sampler
  or buffer at 33 must define `OLO_RENDER_SCALE_BOUNDS` first, or its SPIR-V
  compile fails, on Vulkan only.
