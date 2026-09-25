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
| IBL irradiance cube | *E/π* | ladder IBL rung; groom environment term, as the sky's average radiance *L* (`oloGroomFibreEnvironmentRadiance`) |
| Lightmap (atlas, G-Buffer RT5) | *E* + coverage | ladder rung 1, via `oloNormalizedIrradiance` |
| Probe volume (`sampleProbeVolumeIrradiance`) | *E*: baked SH through `evaluateSHCosineIrradiance`, DDGI atlas | ladder rung 2 via the conversion; ReSTIR GI bounce ÷π |
| ReSTIR DI radiance | contribution, both lobes, combined | deferred, outside the split |
| ReSTIR GI radiance | contribution, **diffuse lobe only** | deferred diffuse half, no AO |
| ReSTIR PT radiance | contribution, both lobes | deferred, outside the split |
| SSGI signal | **signed** contribution delta against the ladder | SSGI composite (`base + delta`) |
| AO buffer | visibility (ambient) | DeferredLighting; forward: AOApply |
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

## Fixed since

- **#1450: the groom's environment term** divided the (already *E/π*) irradiance cube by π again,
  so every coat read π too dark beside a Lambertian surface under the same sky. It also ignored the
  sky's IBL intensity. `GroomEnvironmentFurnaceTest` pins both against all three cube producers.

## Declared approximations (kept on purpose)

- **Forward screen-space AO multiplies the composed colour.** The forward AO buffer is built from the
  forward pass's own normals, after the lighting that would need it. Removing this needs the forward
  pass to export its ambient term, a new attachment that all ~45 forward writers must write (#1452).
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
- **The snow SSS blur** masks by scene-colour alpha, which every non-snow writer sets to 1 (#1451).
- **Deferred's point-light tile evaluator disagrees with the forward light loop** (#1457). It is a
  direct-term parity bug, not an ownership one, and predates this work.
- **Media** (fog, volumetrics) is applied after surface composition, to reflections included, and
  the reference tracer has none.

## How to add a technique

1. Add it to `LightingEstimator` and to `ResolveLightingSignalOwnership`, with its composition.
2. Decide what its buffer stores, and write that down in its producer's header comment.
3. Compose it through the two PBRCommon functions, or state why it cannot.
4. Add an enable/disable case where the correct answer is **zero change**. A white furnace, an
   emission-only scene or a scene without the term's light source makes a double count read as a
   number, not an opinion.
