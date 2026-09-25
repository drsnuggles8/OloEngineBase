# Every lit-surface term has one owner, and every signal says what it stores

Issue #1336. Code: `Renderer/LightingSignalContract.h` (who owns each term, resolved per frame),
`include/PBRCommon.glsl` (`oloComposeReflectedLighting`, `oloComposeSurfaceRadiance`,
`oloNormalizedIrradiance`), `include/AmbientLadder.glsl` (the one ambient ladder). Tests:
`LightingSignalContractTest` (every configuration), `LightingSignalContractGpuTest` (the production
composition and ladder against physics-derived answers), `LightingSignalCompositionEvidenceTest`
(enable/disable on the real deferred pipeline).

## The rules

1. **A term has one owner, a light partition, or a confidence mixture — never two additive
   estimates.** A second estimator of a term *replaces* the first over the part it answers for. A
   double count looks like plausible light, and a still frame cannot show it.
2. **Every hand-off names its quantity**, from `LightingSignalKind`: radiance *L*; normalized
   irradiance *E/π*; irradiance *E*; contribution (*f·L·cos*, already BRDF-weighted); visibility;
   confidence. *E* becomes *E/π* in exactly one place, `oloNormalizedIrradiance`.
3. **A visibility multiplies only the term it was computed for.**
   - Material and screen-space AO multiply the ambient term.
   - A light's shadow map, RT mask, cloud shadow and contact shadow multiply that light.
   - Emission is never occluded.
   - A traced tier (ReSTIR GI or PT) is never multiplied by AO.
4. **Raster paths compose through `oloComposeReflectedLighting` then `oloComposeSurfaceRadiance`**,
   so what multiplies what is written down once.

## Ownership (from `ResolveLightingSignalOwnership`)

| Term | Owner(s) | How they combine |
|---|---|---|
| Direct diffuse/specular | loop (directional) + tiles (punctual) on Forward+/Deferred; loop alone on Forward; loop + **ReSTIR DI** when DI is live | partitioned by light, and by pixel: a skin pixel declines DI |
| Indirect diffuse | **ambient ladder**; or ladder + **SSGI**; or **ReSTIR GI**; or **ReSTIR PT** | exclusive, except SSGI, which replaces the ladder over the directions it resolved |
| Indirect specular | probe/IBL bottom tier + RT reflection + SSR (ADR 0020); or **ReSTIR PT** | confidence mixture |
| Emission | the surface | added once |
| Transmission | leaf / skin lobes | added once, gated per light by that light's visibility |

`LightingSignalContractTest` walks every combination of path and live technique, and asserts:
- every term has an owner;
- two owners are only ever a declared partition or mixture;
- ReSTIR GI never owns specular;
- hybrid tiers own nothing without a G-Buffer.

## What each signal stores

| Signal | Stores | Consumed by |
|---|---|---|
| IBL irradiance cube | *E/π* | ladder IBL rung |
| Lightmap (atlas, G-Buffer RT5) | *E* + coverage | ladder rung 1, via `oloNormalizedIrradiance` |
| Probe volume (`sampleProbeVolumeIrradiance`) | *E*: baked SH through `evaluateSHCosineIrradiance`, DDGI atlas | ladder rung 2 via the conversion; ReSTIR GI bounce ÷π |
| ReSTIR DI radiance | contribution, both lobes, combined | deferred, outside the split |
| ReSTIR GI radiance | contribution, **diffuse lobe only** | deferred diffuse half, no AO |
| ReSTIR PT radiance | contribution, both lobes | deferred, outside the split |
| SSGI signal | **signed** contribution delta against the ladder | SSGI composite (`base + delta`) |
| AO buffer | visibility (ambient) | DeferredLighting; forward: every lit forward shader, through `include/ForwardScreenSpaceAO.glsl` (#1452) |
| Contact shadow | visibility (Lights[0]) | DeferredLighting's loop |
| Reflection probe sample | radiance + confidence | mixed over the prefilter, before the DFG weight |

## What #1336 found and fixed

Each of these was a local choice that looked right.
- **DDGI and lightmaps were π× too bright.** The ladder's helpers compute `kD·X·albedo`, which is
  Lambertian only for *X = E/π*. The IBL cube stores that; the lightmap and DDGI store *E* and went
  in raw. Baked SH evaluated radiance, not irradiance. The old notes called SH "π too dark" and never
  noticed the other two were π too bright.
- **Screen-space AO multiplied the finished frame**: direct light, emission, transmission, and the
  ReSTIR GI term its own comment says AO must not touch. On Deferred the AO buffer exists before
  lighting, so DeferredLighting now applies it to the ambient split. AOApply runs there only for the
  debug view.
- **Contact shadows multiplied the finished frame** after SSR. They now multiply Lights[0]'s visibility
  inside the loop; the post pass keeps only the debug view.
- **SSGI added on top of the ladder.** The ladder had already counted the sky behind every wall that
  SSGI's rays hit. SSGI now re-selects the ladder's diffuse rung at the receiver, using the same
  function and AO. It emits `kD·albedo·Σw·L/N − (Σw/N)·ladder`, which is signed, and a miss
  contributes nothing. The furnace case in `LightingSignalCompositionEvidenceTest` pins this: walls
  that leave exactly the ladder's assumed radiance must not change the floor.
- **ReSTIR GI evaluated the full closure** while the reflection tiers kept indirect specular, so
  glossy light was counted twice. It now evaluates the diffuse half.
- **The deferred pass kept its own copy of the ladder.** It now calls `evaluateAmbientLadderSplitEx`.
- **Parity**:
  - Forward and ray-tier surfaces now take the deferred roughness floor, and the ray tiers take its
    wetness too.
  - The RT-reflection hit's sun term gained its 1/π.
  - ReSTIR PT's sky is now `uniform + cube`, as the oracle's is.
  - RT shadows no longer write 0 for back-facing pixels, which had erased transmission.
  - Forward terrain AO no longer darkens direct light.
- **Forward terrain, voxel terrain and foliage kept their own ambient math** (a flat fill, no IBL or
  probes). They now call the shared ladder, with the controls in `u_TerrainAmbientLadder` and
  `u_LeafIds.w`. Voxel terrain had also been uploading no terrain UBO of its own.
- **Skin pixels took ReSTIR DI's light**, which has no skin lobe, tint or diffusion. They now decline
  DI and keep the light loop.
- **ReSTIR PT's value was filed as diffuse**, so the diffusion pass blurred its specular. It is now
  added outside the split.

## Declared approximations (kept on purpose)

- **Legacy split-sum specular uses `F_roughness·A + B`.** Changing it moves every Legacy pixel;
  ClosureV2 is the corrected closure (ADR 0016).
- **SSGI takes the ladder's radiance as uniform over directions** (the rung's *E/π*). It is exact for
  a uniform field.
- **Raster samples the mip chain; the reference tracer uses level 0** (ADR 0022). Hybrid hit shading
  uses material factors (#805, #1355).

## Owned elsewhere, or counted

- **#1325**: RT reflections and SSR still `mix()` over the whole lit colour. Replacing only the
  specular term needs the old specular as its own signal.
- **ReSTIR DI** stands down when a live light sits past its shader slot bound
  (`ReSTIRDIFallbackReason::LightsBeyondShaderBound`). While DI is live the deferred pass skips the
  clustered tiles, so that light would otherwise light nothing. Two light sources are still unlit,
  and both are accepted:
  - emissive triangles past the encodable index, counted in `EmittersBeyondEncodableIndex`. The
    raster paths never light from emissive geometry at all, so this is no worse than DI off.
  - emissive-geometry light on skin pixels, now that skin declines the tier.
- **Groom** divides the (already *E/π*) irradiance cube by π again (`GroomStrand.glsl`), making it
  π too dark (#1450, owned by the groom work).
- **Deferred's point-light tile evaluator disagrees with the forward light loop** (#1457). It is a
  direct-term parity bug, not an ownership one, and predates this work.
- **Media** (fog, volumetrics) is applied after surface composition, to reflections included, and
  the reference tracer has none.

## Forward screen-space AO (#1452)

Forward and Forward+ apply screen-space AO the way Deferred does: to the ambient term, inside the
shader that composes it. `SelectScreenSpaceAOApplication` answers `AmbientTermInLighting` on every
path, and `PostProcess_SSAOApply` runs only for the AO debug view.

- **The prepass is its own node.** `ScenePrepassPass` clears, batches and runs ScenePass's bucket
  depth-only. With a forward AO buffer produced, it also writes the view normal of scene attachment 2
  through `DepthNormalPrepass*.glsl`, which call the colour pass's own normal function
  (`include/ForwardShadingNormal.glsl`). With AO live the prepass is forced on even where the
  settings leave it off.
- **The order is prepass, AO, colour.** The graph runs `ScenePrepassPass`, then
  `GPUDrivenOcclusionPrepassPass` (the HZB-culled instances' share), then SSAO or GTAO and the
  sphere proxies, then ScenePass's colour half.
- **A surface reads it only if the prepass drew it.** That is PBR (static and skinned) and terrain
  and voxel terrain. `CommandDispatch` publishes the AO buffer and `ForwardAODepth` (the prepass
  depth, copied once) at `TEX_SSAO` / `TEX_POSTPROCESS_DEPTH`, and ScenePass republishes them last,
  right before its colour draws (Forward+ light culling rebinds slot 19 in between). The camera
  block's `ScreenSpaceAOParams` says whether they are live; it starts every frame not live. A
  mirrored replay (planar reflection) suspends them.
- **Anything not in the prepass takes none,** because at its pixels the AO buffer holds the occlusion
  of the surface behind it: blended PBR (`u_AlphaMode == 2`), water, groom strands and foliage. On
  Deferred, water, groom and transparents take none either. Foliage is the one gap: it writes the
  G-Buffer on Deferred and gets its own AO there, but it is not in the forward prepass, so forward
  foliage has no screen-space AO until it is (#1474).
- **Unlit writers have no ambient term, so they apply nothing.** These are skybox, light cubes, grid,
  particles, decals and fluid. There are 18 scene-framebuffer writers, not ~45.

## Snow is a material layer (#1451)

Snow adds no second estimate of any term. `include/SnowLayer.glsl` is the one definition, and every
path uses it:

- **The covered fraction blends the material** (albedo, roughness, metallic, AO, emission) before
  lighting. The ordinary closure then lights snow with shadows, every light, the ambient ladder and
  screen-space AO. The old overlay mixed in a second snow BRDF that ignored shadows and used a flat
  0.15 x albedo ambient.
- **Sparkle is a specular lobe of the directional lights**, gated by each light's own visibility.
  Directional lights are used because every path evaluates them in its loop.
- **Subsurface is the blur of the diffuse half.** Snow pixels hand `(diffuse, -weight)` to scene
  attachment 4. `SSSPass` adds `strength * (blur - diffuse)` into scene colour before the
  transparents. Scene alpha is not a snow channel.
- **Deferred carries the weight in G-Buffer RT3.a** and the snow-filled normal in RT1.
  `DeferredLighting` rebuilds the shading normal and adds the sparkle with the same functions.

| Signal | Stores | Consumed by |
|---|---|---|
| Hand-off `.a < 0` (scene attachment 4) | snow weight, negated | `SSS_Blur.glsl`; read as "no profile" by skin diffusion |
| G-Buffer RT3.a / scene attachment 3 `.a` | snow weight (the material profile) | DeferredLighting; TAA reactivity |

## How to add a technique

1. Add it to `LightingEstimator` and to `ResolveLightingSignalOwnership`, with its composition.
2. Decide what its buffer stores, and write that down in its producer's header comment.
3. Compose it through the two PBRCommon functions, or state why it cannot.
4. Add an enable/disable case where the correct answer is **zero change**. A white furnace, an
   emission-only scene or a scene without the term's light source makes a double count read as a
   number, not an opinion.
