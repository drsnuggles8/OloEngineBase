# Deferred Renderer

> Status: feature-complete and ready for production. The default path
> remains `Forward` to keep small scenes lean; switch via
> RendererSettingsPanel → Rendering Path → `Deferred` when targeting
> high light counts.

## Overview

OloEngine supports three rendering paths, all sharing the same scene,
material, shadow, and post-processing systems:

| Path | When to use |
|------|-------------|
| `Forward` | Small light counts, transparent-heavy scenes, lowest driver overhead. Auto-upgrades to `Forward+` at a configurable light threshold. |
| `ForwardPlus` | Many lights via tile-based clustered culling; good default for complex scenes without G-Buffer memory cost. |
| `Deferred` | Decouples material shading from light count. Scales to high light counts with a fixed G-Buffer cost, supports MSAA with per-sample lighting, and emits a screen-space velocity buffer for TAA / motion blur. |

The deferred path reuses the `ForwardPlus` light culling data
(`MultiLightBuffer` UBO at binding 5) so the same scene description
drives all three paths.

## G-Buffer layout

4 colour render targets + shared depth buffer. Bindings live in
[`ShaderBindingLayout.h`](../OloEngine/src/OloEngine/Renderer/ShaderBindingLayout.h) (TEX_GBUFFER_*).

| RT | Format  | Contents                                                  |
|----|---------|-----------------------------------------------------------|
| 0  | RGBA8   | Albedo RGB + Metallic A                                   |
| 1  | RGBA16F | Octahedral view-space normal (xy) + Roughness (z) + AO (w)|
| 2  | RGBA16F | Emissive RGB + material flags A                            |
| 3  | RG16F   | Screen-space velocity (NDC_curr − NDC_prev) × 0.5          |
| D  | D24S8   | Depth (shared with lighting & post passes — matches the scene framebuffer's `Depth` format so `glBlitFramebuffer(GL_DEPTH_BUFFER_BIT, …)` succeeds) |

The class wrapping this is [`GBuffer`](../OloEngine/src/OloEngine/Renderer/GBuffer.h). Octahedral encode/decode helpers match between
[`PBR_GBuffer.glsl`](../OloEditor/assets/shaders/PBR_GBuffer.glsl) (encode) and
[`DeferredLighting.glsl`](../OloEditor/assets/shaders/DeferredLighting.glsl) (decode); the CPU mirror is verified in
`DeferredPropertyTests`.

## Render graph

```
ShadowPass → ScenePass → DeferredLightingPass → ForwardOverlayPass
         → FoliagePass → DecalPass → WaterPass
         → SSAO/GTAO → ParticlePass → SSSPass → PostProcessPass → … → FinalPass
```

In `Forward` / `ForwardPlus`, `DeferredLightingPass::Execute` and
`ForwardOverlayRenderPass::Execute` are both no-ops — they only run
when `Settings.Path == RenderingPath::Deferred` and `SceneRenderPass`
has produced a G-Buffer for them to sample.

`DeferredLightingPass` writes its lit RGBA16F output directly into the
`ScenePass` framebuffer's colour attachment 0 *and* blits the G-Buffer
depth into the scene FB's depth attachment, so every downstream pass
(foliage, decals, water, SSAO/GTAO, post-processing, selection outline,
UI composite) remains oblivious to the rendering path.

`ForwardOverlayRenderPass` runs immediately after lighting and renders
geometry that cannot participate in the G-Buffer MRT — skybox, terrain,
voxel terrain, infinite editor grid, and debug light cubes. It binds
only colour attachment 0 of the scene framebuffer so the forward
shaders these draws carry don't stomp the (now-unused) G-Buffer slots.
In Forward / Forward+ these commands are routed through `SceneRenderPass`
as usual.

## Lighting features

`DeferredLighting.glsl` is at feature parity with the Forward PBR
shader (`PBR_MultiLight.glsl`) for opaque surfaces:

- **Direct lighting** from `MultiLightBuffer` (binding 5) — directional,
  point, and spot lights evaluated against the G-Buffer-reconstructed
  world position and oct-decoded normal.
- **Forward+ tile evaluation** when `Settings.Path == ForwardPlus`
  clustering data is present — directional lights still go through the
  global list, point/spot contributions are gathered from the tile
  index buffer (SSBOs 9/10/11, UBO 25).
- **CSM directional shadows** (binding 8, cascade data from
  `ShadowData` UBO binding 6), **spot light shadows** (array at
  binding 13), and **point light shadows** (four cubemaps at
  bindings 14-17).
- **IBL ambient** (irradiance at 10, prefilter at 11, BRDF LUT at 12)
  gated by `DeferredLightingControls` UBO @ binding 30.
- **Light-probe ambient** blending (shader scaffolding in place; the
  probe volume SSBO bind will be wired as the probe system matures).
- **Cascade-debug visualisation** toggle in `DeferredLightingControls`.

## Controls

`RendererSettingsPanel` exposes:

- **Path** dropdown (`Forward`, `ForwardPlus`, `Deferred`).
- **MSAA sample count** (1 / 2 / 4 / 8) — hardware-multisample
  G-Buffer. Scene + decal geometry renders into a multisample
  `Framebuffer`; `GBuffer::Resolve()` blit-resolves each colour
  attachment plus depth to a single-sample copy before the lighting
  pass runs. The resolve happens inside `SceneRenderPass::Execute`
  after all MRT writes and before `DeferredLightingPass`.
- **OIT enabled** — weighted-blended OIT (McGuire & Bavoil 2013) for
  transparent particles. When off, particles keep the classic
  back-to-front alpha-blend path. When on, `ParticleRenderPass`
  accumulates into `OITBuffer` (RGBA16F accum + RG16F revealage
  sharing scene depth) with per-attachment blend funcs
  (`glBlendFunci`: accum = `ONE,ONE`; revealage = `ZERO,ONE_MINUS_SRC_COLOR`),
  and `OITResolveRenderPass` composites the result over the scene FB
  with blend factors `ONE_MINUS_SRC_ALPHA, SRC_ALPHA`. Order-independent
  behaviour is asserted by `OITResolveTest.OrderIndependentForTwoFragments`.
- **G-Buffer decals** — when `Deferred` is active, `Renderer3D::DrawDecal`
  routes commands through the `Decal_GBuffer` shader variant, and
  `SceneRenderPass` drains the `DecalPass` command bucket into the
  G-Buffer's albedo attachment *before* the lighting pass runs. The
  decal texels are therefore re-lit by `DeferredLightingPass`
  alongside the rest of the scene. Only RT0 is written (draw buffers
  + colour mask are configured to preserve the metallic channel packed
  into RT0.a and the untouched RT1/RT2/RT3). Forward/Forward+
  rendering remains a post-lighting transparent overlay as before.
- **Debug channel** — 0: lighting, 1: albedo, 2: normals,
  3: R/M/AO, 4: emissive, 5: velocity. Non-zero channels skip the
  lighting pass and blit the selected G-Buffer attachment to the
  scene framebuffer for inspection.

## Known limitations (future work)

- **Particle motion between frames is not captured.** The four
  particle forward shaders (`Particle_Billboard`,
  `Particle_Billboard_GPU`, `Particle_Mesh`, `Particle_Trail`) now
  emit velocity for scene FB RT3, but only for camera motion: the
  billboard vertex buffer and the `MeshInstanceData` UBO carry no
  previous-frame particle/instance position, so per-particle motion
  between frames is treated as zero. Static and slow-moving particle
  fields reproject correctly under camera motion; fast-moving
  particles still fall back to TAA's neighborhood clip on their own
  trajectory. Closing this gap requires a prev-instance stream
  (CPU) or a prev-particle SSBO (GPU path).
- **Time-varying forward displacement is approximated.** Water
  (Gerstner waves) and foliage (wind sway) emit velocity that
  captures camera and per-object motion only; the on-surface
  animation itself is *not* reprojected (doing so would require a
  prev-time uniform so the shader could re-evaluate displacement at
  `t - dt`). Rigid motion, zoom, panning, and strafing look correct
  under TAA; pure wave/wind motion still falls back to neighborhood
  clip which is fine for their relatively slow animation frequencies.

## Renderer capability matrix

All three paths share the same scene description, material system,
shadow maps, post-processing chain, UI, and editor tooling. The
differences are confined to how opaque geometry is shaded and how
lights are accumulated.

| Capability                                  | Forward | Forward+ | Deferred |
|---------------------------------------------|:-------:|:--------:|:--------:|
| PBR materials (metallic / roughness / IBL)  |   ✅    |    ✅    |    ✅    |
| Directional + CSM shadows                   |   ✅    |    ✅    |    ✅    |
| Point + spot shadow maps (up to 4 each)     |   ✅    |    ✅    |    ✅    |
| Animated skeletal meshes                    |   ✅    |    ✅    |    ✅    |
| Skinned per-bone motion vectors             |   ✅    |    ✅    |    ✅    |
| Per-instance motion vectors                 |   ✅    |    ✅    |    ✅    |
| Tile-based light culling (many point/spot)  |   ➖    |    ✅    |    ✅    |
| Auto-upgrade to Forward+ at light threshold |   ✅    |    —     |    —     |
| MSAA                                        |   ✅    |    ✅    | ✅ (per-sample lighting) |
| G-Buffer debug channels (5 attachments)     |   ➖    |    ➖    |    ✅    |
| Transparent OIT (water + particles + decals)|   ✅ (WBOIT) | ✅ (WBOIT) | ✅ (WBOIT) |
| Forward decals                              |   ✅    |    ✅    |    —     |
| G-Buffer decals (Albedo / Normal / RMA / Emissive) |  — |    —     |    ✅    |
| Skybox                                      |   ✅    |    ✅    |    ✅    |
| Infinite editor grid                        |   ✅    |    ✅    |    ✅    |
| Terrain (heightmap + splatmap + snow)       |   ✅    |    ✅    |    ✅    |
| Voxel / Marching-cubes terrain              |   ✅    |    ✅    |    ✅    |
| Foliage instancing (wind + fade)            |   ✅    |    ✅    |    ✅    |
| Water (reflection + refraction + OIT)       |   ✅    |    ✅    |    ✅    |
| Volumetric fog + atmospheric scattering     |   ✅    |    ✅    |    ✅    |
| Screen-space precipitation + snow accumulation | ✅   |    ✅    |    ✅    |
| SSAO                                        |   ✅    |    ✅    |    ✅    |
| GTAO (Ground-Truth AO)                      |   ✅    |    ✅    |    ✅    |
| Bloom / DOF / Motion Blur / Chromatic Ab.   |   ✅    |    ✅    |    ✅    |
| Tone mapping (Reinhard / ACES / Uncharted2) |   ✅    |    ✅    |    ✅    |
| Color grading                               |   ✅    |    ✅    |    ✅    |
| Vignette / FXAA                             |   ✅    |    ✅    |    ✅    |
| Temporal AA (TAA)                           | ✅ (per-object PBR velocity + Halton jitter) | ✅ (per-object PBR velocity + Halton jitter) | ✅ (full G-Buffer velocity + Halton jitter) |
| Subsurface scattering (SSS blur pass)       |   ✅    |    ✅    |    ✅    |
| Morph-target animation                      |   ✅    |    ✅    |    ✅    |
| Particle system (CPU + GPU)                 |   ✅    |    ✅    |    ✅    |
| Selection outline (editor)                  |   ✅    |    ✅    |    ✅    |
| Hot-reload shaders                          |   ✅    |    ✅    |    ✅    |

Legend: ✅ supported · ➖ not applicable / not available · — n/a by design.

### Choosing a path

- **Start with `Forward`.** The `ForwardPlusAutoSwitch` toggle will
  promote the renderer to Forward+ automatically once the point+spot
  light count crosses the configured threshold. Transparent-heavy or
  light-sparse scenes have nothing to gain from Deferred here.
- **Move to `ForwardPlus` explicitly** if you want the tile-culling
  cost to be paid unconditionally — useful when the scene has
  predictable high light counts.
- **Move to `Deferred`** when the scene has hundreds of small lights,
  when you want accurate per-object motion vectors (for TAA / motion
  blur that doesn't ghost), or when you need per-sample MSAA lighting.
  The G-Buffer costs ~20 bytes per pixel (RGBA8 + 2·RGBA16F + RG16F +
  D24S8) but scales to light count at a flat fragment cost.

## Tests

`OloEngine/tests/Rendering/PropertyTests/DeferredPropertyTests.cpp` —
L1 coverage for the octahedral encode/decode round-trip and the
`RendererSettings::DeferredSettings` defaults.

`OloEngine/tests/Rendering/PropertyTests/DeferredOverlayPassTests.cpp`
— L1 coverage for the per-object motion-vector plumbing: the
`ModelUBO` std140 layout (so `PrevModel` lands where
`PBR_GBuffer.glsl` reads it), `DrawMeshCommand::prevTransform`
placement / trivially-copyable invariant, and
`ForwardOverlayRenderPass` construction.

Both are registered in
[`test_catalogue.json`](../OloEngine/tests/scripts/test_catalogue.json)
as L1 tests. Full end-to-end deferred rendering (skybox + terrain +
moving meshes in Deferred mode) is verified through OloEditor's
sample scenes — the project's effective L10 smoke surface.
