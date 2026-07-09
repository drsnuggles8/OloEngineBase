# Camera-relative rendering — every world-space GPU upload is a site (issue #429)

Large world coordinates (tens of km from the origin) exceed f32 precision: a
mesh at world x = 45000 has a ULP of ~0.004, and the GPU's `ViewProjection *
worldPos` then cancels two ~45000 operands to a small result — catastrophic
cancellation whose rounding changes as the camera moves, i.e. **vertex jitter
and shadow swim**. Camera-relative rendering fixes it by rendering in a space
centred on the camera: a per-frame **render origin** `O` (the camera position
snapped to a coarse grid) is subtracted from every world position/matrix
*before* it reaches the GPU, so the GPU works near 0 where f32 is precise.

The math + the grid-snap rationale live in
[`Renderer/CameraRelative.h`](../../OloEngine/src/OloEngine/Renderer/CameraRelative.h)
(`ComputeRenderOrigin`, `MakeModelRelative`, `MakeViewRelative`,
`MakeViewProjectionRelative`, `MakePositionRelative`), pinned by
`CameraRelativeTest.cpp`. The visual before/after is `CameraRelativeVisualEvidenceTest.cpp`.

## The one invariant to internalise

`O` is **grid-snapped** (`kRenderOriginGridSize`, default 1024). Two consequences:

1. **Within the first grid cell `O == (0,0,0)` exactly**, so every shift below is
   a byte-identical no-op. *Every near-origin scene, and the entire existing test
   suite, is unaffected.* This is the safety gate: if a shift is wrong you find
   out far from origin, never near it.
2. `O` is always an integer multiple of 1024. So **any world-space *pattern* whose
   repeat period divides 1024** (integer grid lines, power-of-two texture tiling)
   is invariant under the shift for free.

## Two coordinate spaces — know which one a consumer is in

The renderer draws geometry **render-relative** (models, camera, shadows, lights
are all shifted by `O`). Two families of shader then read world positions, and
they get them in *different* spaces:

### A. Interpolated world position → RELATIVE → add `O` back for patterns
Geometry shaders receive `worldPos = Model_rel * localPos` (a `v_WorldPos`
varying), which is **render-relative**. Lighting/fog *differences*
(`u_CameraPosition - worldPos`, `lightPos - worldPos`) are invariant because both
operands are relative. But a shader that samples an **absolute-world *pattern***
(triplanar tiling `worldPos.xz * tiling`, procedural `noise(worldPos)`, a
world-anchored wave phase, a world grid/clipmap) must **add `O` back**:
`absWorldPos = v_WorldPos + u_RenderOrigin`. Never interpolate the *absolute*
position across a triangle — a ~45 km varying re-introduces the jitter; always
interpolate the small relative `v_WorldPos` and add the uniform `u_RenderOrigin`
in-shader.

`u_RenderOrigin` is the trailing member of the shared `CameraMatrices` block
(binding 0), appended after `u_PrevViewProjection` — see
[`CameraCommon.glsl`](../../OloEditor/assets/shaders/include/CameraCommon.glsl)
and `ShaderBindingLayout::CameraUBO` (now 288 B). **Most pattern shaders declare
their OWN inline camera block** rather than `#include`-ing CameraCommon, so each
needs the field added to its block (full-block shaders append `vec3
u_RenderOrigin; float _padding1;`; truncated blocks add the missing
`u_PrevViewProjection` tail first so the origin lands at std140 offset 272). A
fragment stage that lacks a camera block gets one added (mirror the vertex block
exactly for SPIR-V link validation).

### B. Depth-reconstructed world position → use the WORLD inverse-VP → ABSOLUTE
A post/overlay pass that rebuilds a world position from the depth buffer
(volumetric fog, decals, underwater fog/caustics, motion blur) should invert the
**world** view-projection, not the relative one. The depth was written by the
relative geometry (`ndc = VP_rel * worldPos_rel`), and

    inverse(VP_world) * ndc == translate(O) * worldPos_rel == worldPos_ABSOLUTE

so it reconstructs the *absolute* world position for free — identical to the
pre-#429 pipeline, so these passes need **no per-fragment add-back** and their
world-space UBO inputs (a decal's `WorldToLocal`, a fog volume's `WorldToLocal`)
stay world-space **unshifted**. The one caveat: such a pass usually also reads
the render-relative `u_CameraPosition` from the camera UBO — lift *that* to
absolute with `+ u_RenderOrigin` so its difference terms match the absolute
`worldPos` (volumetric fog does exactly this).

### B′. Depth-reconstruction whose OTHER inputs are relative → subtract `O` back to RELATIVE
Family B works only when *every* other input the pass combines with `worldPos`
is world-space (fog-volume `WorldToLocal`, decal `WorldToLocal`, underwater water
height — all unshifted). A pass that reconstructs from depth but whose other
inputs are **render-relative** must do the opposite: reconstruct absolute (it
shares the world inverse-VP) then **subtract `u_RenderOrigin`** to land back in
the relative space its inputs live in. Two live cases:

- **Deferred lighting** (`DeferredLighting.glsl` / `_MSAA` → `DeferredLightingShared.glsl`)
  reconstructs `worldPos` via the shared binding-8 world inverse-VP (absolute),
  but the camera position, `u_Lights[]`, the Forward+ tile SSBO, the CSM/spot/point
  light-space matrices and the light-probe bounds are all shifted **relative**
  (shared with the forward pass — they cannot be un-shifted). So it does
  `worldPos = ReconstructWorldPosGB(...) - u_RenderOrigin` immediately after
  reconstruction; every downstream `V = camPos - worldPos`, `lightPos - worldPos`,
  shadow projection and probe index is then all-relative and correct. (Its camera
  block gains `u_RenderOrigin` at offset 272, with a distinctly-named padding mat4
  so the tail doesn't collide with the binding-8 `u_PrevViewProjection`.)
- **Volumetric fog light-shafts** (`PostProcess_Fog.glsl::sampleShadowForFog`) —
  the fog body legitimately wants absolute `worldPos` (family B, for the world-
  anchored fog-volume patterns), but the *shadow* lookup multiplies it by the
  relative `u_View` (cascade select) and the relative `u_DirectionalLightSpaceMatrices`.
  So that one function subtracts `u_RenderOrigin` locally before projecting.

## The rule when you add a new world-space GPU upload

Ask: *does this value represent, or get combined with, a world position?* If yes:

- an **object/model matrix** → `MakeModelRelative` (translation −= O)
- a **matrix that consumes world positions** (view, view-projection, a light-space
  matrix) → `MakeViewProjectionRelative` (M · translate(O))
- a **bare world position / AABB corner / light position** → `− O`
- a **geometry-shader pattern on `v_WorldPos`** → `+ u_RenderOrigin` in-shader (A)
- a **depth-reconstruction pass with world-space other-inputs** → world inverse-VP
  → absolute; lift the relative camera position with `+ u_RenderOrigin` for
  difference terms (B)
- a **depth-reconstruction pass with render-relative other-inputs** (deferred
  lighting; the shadow lookup inside fog) → reconstruct absolute, then
  `− u_RenderOrigin` to return to relative space so it matches lights / shadow
  matrices / probe bounds (B′)

Then verify far from origin visually — a missed site is silent near origin.

## Enumerated sites (the audit list)

### Done — core (camera / matrices / transforms / shadows / lights)
- `RenderPipeline::PrepareFrame` — computes `data.RenderOrigin =
  ComputeRenderOrigin(ViewPos)` (gated by the `CameraRelativeEnabled` debug
  lever), builds the primary CameraUBO relative (incl. `RenderOrigin`), sets
  `InverseViewProjectionMatrix = inverse(**world** VP)` for the depth-recon
  passes (family B), propagates the origin to CommandDispatch. World
  `ViewMatrix`/`ViewProjectionMatrix` are **kept** (CPU frustum cull + depth sort
  keys + planar-reflection mirror need world space).
- `CommandDispatch::UploadCameraUBO` — packs the CameraUBO relative (+ RenderOrigin)
  from the stored world matrices + origin; the terrain/voxel inline uploads call
  it, and the planar-reflection mirror camera drives it too (one path, four uploads).
- Per-object transforms — `CommandDispatch::UploadModelInstance` + the instanced
  loop shift `Transform`/`PrevTransform` (covers mesh/depth/quad/terrain/voxel/
  decal-cube/water); `ShadowRenderPass` shifts every caster path;
  `GPUFrustumCuller::Cull` and the legacy `OcclusionCuller` proxies shift their inputs.
- Foliage grass (`FoliageRenderer::Render`/`RenderShadows`) — the per-blade VBO
  holds **absolute** world positions and the pass uploads a single shared model
  matrix to the instance SSBO, so that matrix is `translate(−O)` (via
  `MakeModelRelative`), not identity: it shifts every blade into relative space
  for `gl_Position` while the shaders add `u_RenderOrigin` back for the world-
  anchored wind field (family A).
- GPU Hi-Z occlusion cull (`RenderPipeline` `HZBOcclusionInputs`) — the instance
  bounds it reprojects are shifted relative by `GPUFrustumCuller::Cull`, so its
  `PrevViewProjection` is made relative to the same origin
  (`MakeViewProjectionRelative`); otherwise `VP_world · relativeCenter` mis-culls
  far from origin. (The render origin is computed up-front in `PrepareFrame` so
  this cull can consume it.)
- Shadows — `ShadowMap::UploadUBO(origin)` shifts the sampling matrices + point
  positions; `ShadowRenderPass::RenderCascadeOrFace` shifts `lightVP` + point
  `Position` + casters identically, so rendered and sampled depth agree. That
  pass's shadow-camera UBO also sets `RenderOrigin` (not just the shifted
  matrices), so a caster shader that reconstructs an absolute world position —
  terrain snow-height displacement in `Terrain_Depth.glsl` — matches the lit
  surface instead of detaching far from origin.
- Lights — `Renderer3D::UploadMultiLightUBO` + `LightCullingBuffer::Update` shift
  the multi-light UBO and the Forward+ point/spot/sphere-area SSBO positions.

### Done — family B′ (depth-reconstruction whose other inputs are relative → subtract `O`)
- Deferred lighting (`DeferredLighting.glsl`, `DeferredLighting_MSAA.glsl`,
  `DeferredLightingShared.glsl`) — reconstructs absolute via the shared world
  inverse-VP, then `worldPos -= u_RenderOrigin` so it matches the relative camera
  / lights / shadow matrices / probe bounds. Without this the entire deferred lit
  path is off by `O` far from origin (wrong specular, mis-placed lights, vanished
  shadows, dropped probe GI) while looking correct near origin.
- Volumetric fog light-shafts (`PostProcess_Fog.glsl::sampleShadowForFog`) — the
  fog body stays absolute (family B), but the shadow-cascade lookup subtracts
  `u_RenderOrigin` because `u_View` and the light-space matrices are relative.

### Done — family B (depth-reconstruction → world inverse-VP → absolute)
- Decals (`Renderer3D::DrawDecal`, `Decal*.glsl`) — the cube renders relative
  (via UploadModelInstance), the shader reconstructs absolute world (world
  inverse-VP) and uses the world `inverseDecalTransform`.
- Volumetric fog (`PostProcess_Fog.glsl`) — reconstructs absolute world; the fog
  volumes' `WorldToLocal` stay world-space; the only add-back is
  `cameraPos = u_CameraPosition + u_RenderOrigin` for the distance/height/noise
  difference terms. (Its `MotionBlurUBO` gets an instance name so its
  `u_PrevViewProjection` doesn't collide with the camera block's.)
- Underwater fog + caustics + god-rays (`PostProcess_ToneMap.glsl`,
  `Renderer3D::UploadUnderwaterFogUBO`) — the underwater UBO keeps its world
  inverse-VP / camera position / water-surface height, so the reconstructed
  world is absolute and the caustic/dapple patterns are correct with **no shader
  change**.
- Light probe volumes (`UploadLightProbeData`) — `BoundsMin/Max − O` (the probe
  grid is indexed by the relative geometry `worldPos`, family A).

### Done — family A (geometry pattern shaders, `+ u_RenderOrigin`)
- **Terrain** — `Terrain_PBR`, `Terrain_GBuffer`, `Terrain_Depth`, `Terrain_Voxel`,
  `Terrain_Voxel_GBuffer`: triplanar tiling, the snow clipmap UV, the editor brush
  distance, and the snow height plane all rebuild `worldPosAbs = v_WorldPos +
  u_RenderOrigin`. (`SnowCommon.glsl` stays origin-agnostic — callers pass the
  absolute position.)
- **Water** — `Water.glsl` (VS/TCS/TES/FS): the Gerstner phase / FFT field / foam
  / normal-map / procedural noise are world-anchored. VS/TES pass the absolute
  position to `sumGerstnerWaves(...)` and subtract `u_RenderOrigin` off the
  returned *displaced* position to keep it relative for `gl_Position`; the FS
  adds the origin to every `v_WorldPos.xz` pattern sample.
- **Foliage** — `Foliage_Instance`, `Foliage_Instance_GBuffer`: the wind field is
  world-anchored, so `bladeWorldPos` gets `+ u_RenderOrigin`. This is only correct
  because `u_Model` is `translate(−O)` (see the core list — `FoliageRenderer`
  uploads that, not identity): the blade VBO is absolute, `u_Model` makes the
  interpolated `v_WorldPos` relative, and `u_Model · pos + u_RenderOrigin` recovers
  the absolute position for the wind sample. `Foliage_Depth` needs nothing (its
  only wind path reads the unshifted instance attribute).
- **Infinite grid** — `InfiniteGrid`, `InfiniteGrid_GBuffer`: `fragPos3D` is
  reconstructed from the *relative* view, so `Grid()` is fed `fragPos3D +
  u_RenderOrigin` (fixes the coloured world-origin axes; the regular integer
  lines were already fine by the 1024-multiple rule). The GBuffer variant's
  fragment has no camera block, so the origin rides a `flat` varying from the vertex.

### Done — 2D sprites (`Renderer2D`, CPU bake — a coordinate shift, NOT a shader change)

The only site so far where the detail is lost at **CPU bake time**
(`transform * local` computed in f32 at 45 km), not at GPU upload — a different
failure path from every family above, which is why it was a separate slice.
`Renderer2D` keeps its **own** render origin `O` (`s_Data.RenderOrigin`),
computed per `BeginScene` from the camera world position (`BeginSceneImpl`), and:
- uploads the 2D camera UBO **relative** — `MakeViewProjectionRelative(vpWorld, O)`
  (was the plain world VP). The three `BeginScene` overloads feed it the camera
  world position: `OrthographicCamera`/`EditorCamera` via `GetPosition()`, the
  runtime `(Camera, transform)` path via the transform's translation column.
- bakes every world vertex **relative**: the transform-matrix `Draw*` calls
  (`DrawQuad`/`DrawCircle`/`DrawRect(mat4)`/`DrawSprite`/`DrawString`) shift the
  matrix once with `MakeModelRelative(transform, O)` before `matrix * local`; the
  explicit-position calls (`DrawLine`/`DrawPolygon`/`DrawQuadVertices`) subtract
  `O` per vertex with `MakePositionRelative`.
- needs **NO shader change** — the 2D shaders only project `a_Position` /
  `a_WorldPosition` through the camera UBO, and `VP_rel * (worldPos − O) ==
  VP_world * worldPos`, so the shift is invisible to them (unlike the family-A
  pattern shaders that sample an absolute-world pattern and must add `O` back).
- gotcha: `DrawRect(mat4)` bakes its corners **relative** and feeds them to a
  private `DrawLineImpl` (no shift), because the public `DrawLine` subtracts `O`
  from its world-space endpoints — routing the already-relative corners through
  `DrawLine` would double-subtract.
- own debug lever `Renderer2D::SetCameraRelativeEnabled(false)` (separate from
  Renderer3D's — Renderer2D holds its own `s_Data.CameraRelativeEnabled`), pins
  `O` to (0,0,0). Pinned by `Renderer2DCameraRelativeTest.cpp` (CPU, ortho bake
  math) + `Renderer2DCameraRelativeVisualEvidenceTest.cpp` (near/far, ON vs OFF).
- **follow-up gap discovered:** other CPU-bake-to-world paths likely have the
  same latent issue — `UIRenderer` and `ParticleRenderer` both call
  `Renderer2D::BeginScene`/`DrawSprite`, so they now inherit the 2D shift for
  free, but any subsystem that bakes its own absolute-world vertices before
  upload (not through `Renderer2D`) is a separate audit.

### Remaining (follow-up)
- **Terrain GEOMETRY far from origin** — the terrain *patches* stop rendering far
  out because the CPU-side terrain quadtree LOD (`TerrainUBO::TessFactors`,
  computed from camera↔patch distance) loses precision at ~45 km, so patches
  degenerate/cull. This is independent of the (now-fixed) terrain *pattern*
  shaders and of camera-relative rendering — a separate large-coordinate slice
  for the terrain LOD/streaming system. (Foliage and the sphere/plane/grid all
  render fine at 45 km; terrain is the outlier.)
- **Floating-origin / origin rebasing** — the umbrella's other half (re-base the
  world as the player travels far, integrating with `SceneStreamer`).

## Debug / bisect lever

`Renderer3D::SetCameraRelativeEnabled(false)` pins `O` to (0,0,0), reverting to
exact pre-#429 world-space behaviour — used to A/B the feature (the visual
evidence test captures far-origin ON vs OFF) and to isolate a regression. Same
spirit as `OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL`. `Renderer2D` has its **own**
independent lever `Renderer2D::SetCameraRelativeEnabled(false)` (the 2D path
holds a separate `s_Data.RenderOrigin` / `CameraRelativeEnabled`), so the 2D
overlay and the 3D scene can be A/B'd independently.
