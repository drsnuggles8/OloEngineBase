// =============================================================================
// DeferredLightingShared.glsl — shared PBR shading body for deferred lighting.
// Consumed by DeferredLighting.glsl (non-MSAA) and DeferredLighting_MSAA.glsl.
//
// Prerequisites (the caller must declare these identically-typed resources
// BEFORE including this file):
//   - CameraMatrices UBO at binding 0
//   - MultiLightBuffer UBO at binding 5
//   - ShadowData UBO at binding 6
//   - MotionBlurMatrices UBO at binding 8
//   - DeferredLightingControls UBO at binding 30
//   - samplerCube u_IrradianceMap (10), u_PrefilterMap (11)
//   - sampler2D u_BRDFLutMap (12)
//   - sampler2DArrayShadow u_ShadowMapCSM (8), u_ShadowAtlas (13)
//   - sampler2DArray u_ShadowMapCSMRaw (33), u_ShadowAtlasRaw (34) — PCSS blocker search
//   - sampler2D u_ReSTIRDIRadiance (73) — ReSTIR DI's resolved direct lighting
//     (issue #1140), plus u_MSAAParams.y as the "the tier is live" lane
//   - sampler2D u_ReSTIRGIRadiance (74) — ReSTIR GI's resolved indirect diffuse
//     (issue #1169), plus u_MSAAParams.z as its "the tier is live" lane
//   - sampler2D u_RayTracedShadowMask (72) — hybrid ray-traced visibility mask,
//     plus the ShadowData block's u_RayTracedShadowLightIndices /
//     u_RayTracedShadowParams routing lanes (issue #1056)
//   - include/PBRCommon.glsl, include/LightProbeSampling.glsl,
//     include/ForwardPlusCommon.glsl (with FPLUS_ATLAS_SHADOWS defined, AFTER
//     the ShadowData block + atlas samplers)
//   - the DeferredLightingControls block's u_LeafProfileTint /
//     u_LeafProfileLobe arrays (issue #1234) — the leaf-profile table, indexed
//     by the same three-bit G-Buffer slot field skin's profile uses
//   - include/ReflectionProbes.glsl with OLO_REFLECTION_PROBE_SAMPLERS
//     defined (distance-impostor probe arrays at bindings 14/15, probe UBO
//     58, probe grid SSBO 53 — issue #705)
//
// G-Buffer sampling is done by the caller (sampler2D vs sampler2DMS); this
// file consumes only the already-unpacked per-pixel values.
// =============================================================================

#ifndef DEFERRED_LIGHTING_SHARED_GLSL
#define DEFERRED_LIGHTING_SHARED_GLSL

// Surface weather response (wetness + cloud shadow, issue #633) — self-
// contained (UBO 53 + sampler 59), safe to pull in from an include.
#include "AtmosphereShading.glsl"

// Virtual Shadow Maps (issue #702) — self-contained too (UBO 79/80, page-table
// SSBO 54, sampler 65), so the directional-shadow branch below can choose
// between VSM and CSM at runtime instead of needing a second shader variant.
// VirtualShadowMap::BindForSampling publishes a DISABLED globals block when the
// system is off, which is what makes the always-compiled branch free.
#include "VirtualShadowSampling.glsl"

// The vegetation material's LOBE half (issue #1234) — no resources of its own,
// so it compiles into this fullscreen pass as easily as into the foliage
// shaders. This is what makes "forward and deferred evaluate the same
// transmission" a fact about the build rather than a claim: both call
// oloFoliageTransmissionDirect / ...Ambient from THIS file. The SURFACE half
// (OLO_FOLIAGE_SURFACE_SAMPLING) is deliberately NOT compiled here — it needs
// the foliage UBO and the leaf samplers, which a fullscreen pass has neither of,
// and its output already reached us through the G-Buffer.
#include "FoliageSurface.glsl"

vec3 OctDecodeGB(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

vec3 ReconstructWorldPosGB(vec2 uv, float depthNDC)
{
    float zNDC = depthNDC * 2.0 - 1.0;
    vec4 clipPos = vec4(uv * 2.0 - 1.0, zNDC, 1.0);
    vec4 worldPos = u_InverseViewProjection * clipPos;
    return worldPos.xyz / worldPos.w;
}

// -----------------------------------------------------------------------------
// The shadow-technique seam (issue #1056).
//
// This file used to decide the directional light's shadow with a literal
// two-way GLSL branch. Ray tracing is a THIRD source of the same number, and
// bolting on a third `if` selected by yet another unrelated flag is what the
// seam exists to avoid — so the question asked here is "does a ray-traced mask
// answer for this light?", once, for every light type, and the answer is a
// routing lookup rather than a technique flag.
//
// Returns true and fills `visibility` when light `lightIndex` reads a mask
// channel this frame. False means: take whichever raster path this light type
// already had. The CPU side (OloEngine::SelectShadowTechnique) is what decided
// that, counted the reason, and left u_RayTracedShadowParams.x at 0 when the
// answer was no — so there is no way for this branch to be on while the mask
// is absent.
//
// texelFetch, not texture(): the mask is declared at the scene band, the same
// resolution this pass shades at, so there is exactly one texel per shaded
// pixel and any filtering would only smear the denoiser's own edge-aware
// result across a silhouette it deliberately kept.
bool oloRayTracedShadowFactor(int lightIndex, out float visibility)
{
    visibility = 1.0;
    if (u_RayTracedShadowParams.x < 0.5 || lightIndex < 0)
        return false;

    // Four comparisons against the channel routing rather than a per-light
    // lane in the block: there are at most four ray-traced lights and up to
    // MAX_LIGHTS lights, so this is the cheap direction to index.
    int channel = -1;
    for (int c = 0; c < 4; ++c)
    {
        if (u_RayTracedShadowLightIndices[c] == lightIndex)
        {
            channel = c;
            break;
        }
    }
    if (channel < 0)
        return false;

    vec4 mask = texelFetch(u_RayTracedShadowMask, ivec2(gl_FragCoord.xy), 0);
    visibility = clamp(mask[channel], 0.0, 1.0);
    return true;
}

// -----------------------------------------------------------------------------
// The direct-lighting tier seam (issue #1140).
//
// ReSTIR DI answers for every punctual light, every sphere-area light and all
// emissive geometry when it is live, so the loop below is TRUNCATED to the
// directional lights rather than added to. IBL, light probes, baked GI and
// emissive are untouched: those are different terms, and adding a resampled
// direct estimate on top of a clustered one would double-count every light in
// the scene.
//
// Directional lights are deliberately NOT in it — see ComputeDeferredLit.
//
// Two conditions, and both are load-bearing. u_MSAAParams.y is only raised when
// OloEngine::SelectReSTIRDITechnique returned ReSTIRDI, so the CPU has already
// counted the reason if it did not. The ALPHA test is the second half: the
// resolve writes alpha 1 only where it produced a value, so a sky pixel, an
// unlit pixel and a pixel the tier stood down on all fall through to the
// clustered loop per-pixel rather than going black.
//
// texelFetch, not texture(): the radiance target is declared at the scene band,
// the resolution this pass shades at, so there is exactly one texel per shaded
// pixel and filtering would only smear the estimator's own edges.
bool oloReSTIRDIDirectLighting(out vec3 radiance)
{
    radiance = vec3(0.0);
    if (u_MSAAParams.y < 0.5)
        return false;
    vec4 resolved = texelFetch(u_ReSTIRDIRadiance, ivec2(gl_FragCoord.xy), 0);
    if (resolved.a < 0.5)
        return false;
    if (any(isnan(resolved.rgb)) || any(isinf(resolved.rgb)))
        return false;
    radiance = max(resolved.rgb, vec3(0.0));
    return true;
}

// -----------------------------------------------------------------------------
// The indirect-diffuse tier seam (issue #1169).
//
// ReSTIR GI answers for the WHOLE diffuse ambient when it is live — not just
// the probe rung — and that is forced rather than chosen: a bounce ray that
// escapes collects the environment, so the sky's diffuse contribution is
// already inside the resampled estimate, and leaving the ladder's
// sky-irradiance rung on underneath would count it twice. A baked lightmap is
// itself a complete diffuse-GI solution and goes the same way.
//
// The SPECULAR half of the ambient — the prefiltered IBL and the reflection
// probes — is untouched. This is a diffuse tier and says so; a tier that
// quietly ate the specular ambient would darken every metal in the scene by an
// amount an exposure tweak hides.
//
// AND THE RESULT IS NOT MULTIPLIED BY AO. ComputeDeferredLit computes
// `ambient * ao`; this estimate's occlusion is already exact, because it traced
// the rays. Multiplying a ray-traced visibility estimate by a screen-space
// approximation of the same visibility darkens every corner twice, by a factor
// nobody can attribute because both halves look plausible. AO keeps multiplying
// the specular half.
//
// Two conditions, and both are load-bearing, exactly as in the DI seam above.
// u_MSAAParams.z is only raised when OloEngine::SelectReSTIRGITechnique returned
// ReSTIRGI, so the CPU has already counted the reason if it did not. The ALPHA
// test is the second half: the resolve writes alpha 1 only where it produced a
// value, so a sky pixel, an unlit pixel and a pixel the tier stood down on all
// fall through to the ambient ladder PER PIXEL rather than going dark.
//
// texelFetch, not texture(): the radiance target is declared at the scene band,
// the resolution this pass shades at, so there is exactly one texel per shaded
// pixel and filtering would only smear the estimator's own edges.
bool oloReSTIRGIIndirectDiffuse(out vec3 radiance)
{
    radiance = vec3(0.0);
    if (u_MSAAParams.z < 0.5)
        return false;
    vec4 resolved = texelFetch(u_ReSTIRGIRadiance, ivec2(gl_FragCoord.xy), 0);
    if (resolved.a < 0.5)
        return false;
    if (any(isnan(resolved.rgb)) || any(isinf(resolved.rgb)))
        return false;
    radiance = max(resolved.rgb, vec3(0.0));
    return true;
}

// Apply cascade-debug tint on top of the lit color. Shared between variants.
vec3 ApplyCascadeDebug(vec3 color, vec3 worldPos)
{
    vec4 viewSpacePos = u_View * vec4(worldPos, 1.0);
    float viewDepth = -viewSpacePos.z;
    vec3 cascadeColors[4] = vec3[4](
        vec3(1.0, 0.2, 0.2),
        vec3(0.2, 1.0, 0.2),
        vec3(0.2, 0.2, 1.0),
        vec3(1.0, 1.0, 0.2));
    int cascadeIdx = 3;
    for (int c = 0; c < 4; ++c)
    {
        if (viewDepth < u_CascadePlaneDistances[c])
        {
            cascadeIdx = c;
            break;
        }
    }
    return mix(color, cascadeColors[cascadeIdx], 0.3);
}

// Full deferred lighting evaluation for a single shaded point. Returns the
// linear HDR color (caller writes it to the output attachment).
//
// The `emissiveFlags` parameter carries the emissive colour in .rgb and a
// material-flags packed float in .a — a small integer, exact in RGBA16F:
//
//   bit 0 — **unlit**: skip all direct + ambient + probe + IBL work and
//           return the raw emissive colour. This is the mechanism G-Buffer
//           overlay variants (Skybox_GBuffer, InfiniteGrid_GBuffer,
//           LightCube_GBuffer) use to opt out of PBR shading while still
//           participating in motion-vector + depth writes. They write a=1.0.
//   bits 1.. — **PBR closure model** (issues #975, #996): the whole model
//           index shifted up past the unlit bit, written by every PBR
//           G-Buffer shader through oloEncodeGBufferPbrFlags (PBRCommon.glsl)
//           — the ONE executable home for this layout. The field has no width
//           and the decode below has no mask, so appending a model to
//           PBRModel.h cannot truncate it to Legacy on this path while Forward
//           shades it correctly. Legacy materials still write a=0.0.
//
//           The lane is a bitfield and is never averaged: GBuffer::Resolve()
//           overwrites RT2's alpha with one real sample's flags after the blit
//           (GBufferFlagsResolve.glsl, issue #996), so both the resolved and
//           the per-sample reader see a value a real sample wrote, and the
//           int(round()) below is exact rather than a tie-break.
//
// `bakedGI` is G-Buffer RT5 (issue #865): baked lightmap irradiance E in .rgb
// and COVERAGE in .a, already un-premultiplied and intensity-scaled by
// sampleLightmapIrradiance() back in the G-Buffer pass — the last stage that
// still had UV2 and the per-draw atlas region. It is the same vec4 the forward
// path hands evaluateAmbientLadder(), and it enters the ladder at the same
// (top) rung, so the two paths pick the same ambient source for the same pixel.
// vec4(0.0) means "no baked GI here" and drops straight through to probes/IBL.
// The `skinDiffuse` out-parameter is the DIFFUSION HAND-OFF (issue #1241): the
// diffuse half of a skin pixel's lighting plus the identity of the profile that
// should blur it, for SkinDiffusion.glsl. See include/PBRCommon.glsl for the
// encoding and for why scene colour still carries the whole composite.
//
// It is cleared FIRST, so every early return below -- the unlit pass-through and
// the four material debug views -- leaves it at "no diffusion here". That is the
// intended answer in each case: an unlit pixel has no skin transport, and the
// debug views show what the CLOSURE produced, which is the thing being
// inspected. A blurred debug view would be a different question.
vec3 ComputeDeferredLitSplit(
    vec3 albedo, float metallic,
    vec3 N, float roughness, float ao,
    vec4 emissiveFlags, vec3 worldPos, vec4 bakedGI,
    out vec4 skinDiffuse)
{
    skinDiffuse = vec4(0.0);
    vec3 emissive = emissiveFlags.rgb;
    int gbFlags = oloDecodeGBufferFlags(emissiveFlags.a);
    if (oloGBufferFlagsAreUnlit(gbFlags))
    {
        // Unlit pass-through — skybox, editor grid, light-cube billboards
        // etc. sit inside the G-Buffer but do not want PBR shading applied.
        return emissive;
    }
    // PBR closure model selector (issues #975, #996) — see the flag layout
    // above. No mask: the field is the whole rest of the lane, so a model
    // appended to PBRModel.h arrives here un-truncated.
    int pbrModel = oloGBufferFlagsPbrModel(gbFlags);

    // Material kind + skin profile (issue #1231). The kind is what the surface
    // IS; the slot names the authored profile whose parameters this pass reads
    // out of u_SkinProfileParams. The slot is ONLY consulted under the Skin
    // test: the G-Buffer writers that do not go through
    // oloEncodeGBufferPbrFlagsEx (Terrain, Foliage, Water) leave the lane at
    // 0.0, whose slot bits read as 0, and treating that as "profile 0" would
    // tint every terrain highlight with somebody's skin.
    int materialKind = oloGBufferFlagsMaterialKind(gbFlags);
    int skinProfileSlot = (materialKind == OLO_MATERIAL_KIND_SKIN)
                              ? oloGBufferFlagsSkinProfileSlot(gbFlags)
                              : OLO_SKIN_PROFILE_SLOT_NONE;

    // ── The LEAF material (issue #1234) ─────────────────────────────────────
    // The SECOND tenant of that same three-bit slot field, read under the
    // Foliage test exactly as skin's is read under the Skin test. A pixel has
    // one kind, so the two can never both claim these bits — and the
    // slot-is-only-meaningful-under-its-kind discipline is why a Terrain pixel's
    // zero lane is not mistaken for "leaf profile 0".
    bool isFoliage = (materialKind == OLO_MATERIAL_KIND_FOLIAGE);
    int leafProfileSlot = isFoliage ? oloGBufferFlagsSkinProfileSlot(gbFlags)
                                    : OLO_SKIN_PROFILE_SLOT_NONE;
    bool hasLeafProfile = leafProfileSlot < OLO_SKIN_PROFILE_SLOT_NONE;
    vec3 leafTint = vec3(0.0);
    vec4 leafLobe = vec4(0.0);
    if (hasLeafProfile)
    {
        // .rgb is tint PRE-MULTIPLIED by strength, packed by
        // FoliageLeafProfileTintLane — the same product the forward path's
        // u_LeafTransmit.rgb carries, in the same order.
        leafTint = u_LeafProfileTint[leafProfileSlot].rgb;
        leafLobe = u_LeafProfileLobe[leafProfileSlot];
    }
    // THE THICKNESS LANE. Foliage's G-Buffer writer parks the pixel's thickness
    // in RT5's RED channel with coverage 0 — see Foliage_Instance_GBuffer.glsl
    // for why that is free.
    //
    // GATED ON THE PROFILE SLOT, NOT MERELY ON THE KIND, and the difference is
    // a real surface. A Material's kind is authorable in the editor, so a
    // MESH material can be set to Foliage — and a mesh goes through
    // PBR_GBuffer, which writes RT5 as BAKED LIGHTMAP IRRADIANCE and names no
    // leaf profile. Keying on the kind alone would read that irradiance as a
    // thickness. Only a pixel a foliage shader wrote carries a leaf slot, so
    // the slot is the honest test for "RT5 is a thickness here".
    // AND A COVERAGE TEST, which is about RESOLVED MSAA. GBuffer::Resolve
    // average-blits RT5 but overwrites RT2's flags with ONE REAL SAMPLE
    // (GBufferFlagsResolve.glsl), so a silhouette pixel that is part foliage and
    // part lightmapped receiver can resolve its FLAGS to Foliage while its RT5
    // red channel is an average that includes the neighbour's baked irradiance
    // — which is in physical units and routinely exceeds 1. Read as a thickness
    // that is a bright transmission fringe along every such edge.
    //
    // Foliage writes coverage 0 and a lightmapped receiver writes 1, so the
    // averaged alpha IS the mix fraction: requiring it below 0.5 drops the
    // pixels where the irradiance dominates, and the clamp bounds what is left.
    // This does not make a mixed pixel exact — nothing short of resolving RT5
    // the way the flags lane is resolved would — it bounds the error to the
    // side where foliage is the majority and the contamination is small.
    float leafThickness = (hasLeafProfile && bakedGI.a < 0.5) ? clamp(bakedGI.r, 0.0, 1.0) : 0.0;
    // The transmission term, accumulated beside Lo and composited at the end.
    // Kept OUT of the diffuse/specular split on purpose: it is a third
    // transport, and folding it into the diffuse half would make the
    // `materialdiffuse` debug view answer a question nobody asked and leave
    // #1234's fourth criterion (inspect transmission separately) unanswerable.
    vec3 transmitted = vec3(0.0);
    vec3 skinSpecularTint = vec3(1.0);
    int skinEvaluationModel = OLO_SKIN_MODEL_DIFFUSE_SPECULAR_SPLIT;
    // All-zero until a slot claims them, and an all-zero scatter lane makes the
    // transmittance black — so an unclaimed or stale slot loses the effect
    // rather than acquiring someone else's lobe.
    vec4 skinTransmitScatter = vec4(0.0);
    vec4 skinTransmitScaling = vec4(0.0);
    // The layered specular lane (issue #1243). vec2(0.0, 1.0) — one lobe — until
    // a slot claims it, so an unclaimed or stale slot loses the effect rather
    // than acquiring someone else's.
    vec2 skinLobe = vec2(0.0, 1.0);
    // The oral surface lane (issue #1245). All-zero until a slot claims it,
    // which is dry with the transmitted term untouched — so an unclaimed or
    // stale slot loses the effect rather than acquiring someone else's wetness.
    vec4 skinOralLane = vec4(0.0);
    if (skinProfileSlot < OLO_SKIN_PROFILE_SLOT_NONE)
    {
        skinSpecularTint = u_SkinProfileParams[skinProfileSlot].rgb;
        // The .w lane is the profile's transport VERSION, carried as a float
        // because it is a small exact integer. It is what makes the forward and
        // deferred paths take the same arm of oloApplySkinProfile's version
        // branch for the same profile.
        skinEvaluationModel = int(u_SkinProfileParams[skinProfileSlot].w + 0.5);
        // The thin-region transmission lanes (issue #1242) — the same two vec4s
        // the forward path reads out of the material UBO, packed by the same
        // SkinTransmissionScatterLane / SkinTransmissionScalingLane, so the two
        // paths cannot differ about the order the factors multiply in.
        skinTransmitScatter = u_SkinTransmitScatter[skinProfileSlot];
        skinTransmitScaling = u_SkinTransmitScaling[skinProfileSlot];
        // Through oloSkinLobeFor and not read raw, so the VERSION TEST is the
        // same one the forward paths apply — a profile below transport version 3
        // gets one lobe here exactly as it does there, decided by one function.
        skinLobe = oloSkinLobeFor(materialKind, skinEvaluationModel, u_SkinSpecularLobe[skinProfileSlot]);
        // Through oloSkinEvaluatesOralSurface and not read raw, for the reason the lobe
        // above goes through oloSkinLobeFor: the VERSION TEST is then the same
        // one function the forward and clustered paths apply, so a profile below
        // transport version 4 is dry here exactly as it is there.
        skinOralLane = oloSkinEvaluatesOralSurface(materialKind, skinEvaluationModel)
                           ? u_SkinOralLane[skinProfileSlot] : vec4(0.0);
    }

    // THE SKIN THICKNESS LANE (issue #1242). The second tenant of RT5's red
    // channel — see oloSkinPackGBufferThickness in include/SkinTransmission.glsl
    // for the tenancy rules and for why the coverage test inside the unpack is
    // about resolved MSAA rather than about authoring.
    //
    // Gated on the PROFILE SLOT, not on the kind: a mesh material can be set to
    // Skin in the editor, and only a pixel that also names a profile went
    // through the writer. Identical discipline to the leaf thickness above.
    float skinThicknessMM = oloSkinUnpackGBufferThickness(bakedGI, skinProfileSlot < OLO_SKIN_PROFILE_SLOT_NONE);

    // THE VERSION TEST, HERE AND NOT IN THE G-BUFFER. The writer publishes a
    // thickness for every skin pixel that names a profile, because it has no
    // SkinEvaluationModel to test; the version lives in the slot table, which
    // is here. So this is the ONE place on this path that decides whether the
    // profile's author asked for transmission — and a version this shader has
    // no arm for transmits NOTHING rather than guessing.
    // BOTH TRANSMITTING VERSIONS. The versions are CUMULATIVE — version 3 is
    // "everything version 2 does, plus the layered specular" — so omitting it
    // here would silently stop a version-3 head transmitting through its ears
    // the moment its author turned the lobes on.
    bool isSkinTransmitting = ((skinEvaluationModel == OLO_SKIN_MODEL_THICKNESS_TRANSMISSION) ||
                               (skinEvaluationModel == OLO_SKIN_MODEL_LAYERED_SPECULAR) ||
                               (skinEvaluationModel == OLO_SKIN_MODEL_ORAL_SURFACE)) &&
                              (skinThicknessMM > 0.0);

    vec3 V = normalize(u_CameraPosition - worldPos);

    // Weather response + cloud shadow (issue #633) — identical placement to
    // the forward path (PBR_MultiLight.glsl) so the two paths stay in
    // photometric parity: wetness before any lighting reads albedo/roughness,
    // cloud shadow applied per directional light inside the loop.
    atmosphereApplyWetness(albedo, roughness, N);
    float cloudShadow = atmosphereCloudShadow(worldPos);

    bool enableIBL     = u_DeferredControls.x > 0.5;
    bool enableProbes  = u_DeferredControls.y > 0.5;
    float iblIntensity = u_DeferredControls.z;
    bool cascadeDebug  = u_DeferredControls.w > 0.5;

    // Direct lighting, diffuse and specular kept apart to the composite (issue
    // #1231) — the same seam PBR_MultiLight.glsl cuts, cut here so the two
    // paths mean the same thing by construction rather than by review.
    OloSurfaceLighting Lo = oloSurfaceLightingZero();

    // Terms that arrive ALREADY COMBINED and cannot be split here. The ReSTIR DI
    // estimate is one resampled radiance covering both lobes; splitting it would
    // mean re-deriving the estimator, which is that tier's own work and not
    // #1231's. They are summed into the composite exactly as before, and the
    // diffuse/specular debug views say so by excluding them — an honest gap is
    // better than a plausible-looking half.
    vec3 unsplitDirect = vec3(0.0);

    // The ReSTIR DI tier owns the direct term for every light EXCEPT the
    // directional ones (issue #1140).
    //
    // WHY DIRECTIONAL LIGHTS STAY ON THE LOOP. A directional light is one delta
    // light: there is no variance for resampling to remove, and routing it
    // through the reservoir would silently drop three things the loop below
    // applies and a reservoir cannot — the CSM / VSM cascades, the ray-traced
    // shadow mask channel, and the cloud shadow. Each would go missing as "the
    // sun looks flat", which is precisely the silent regression this tier must
    // not cause. So the split is the one the Forward+ arm already uses: the loop
    // runs over the directional lights, the tier owns the rest.
    //
    // Everything after the loop — IBL, probes, baked GI, emissive — runs either
    // way, because those are different terms.
    vec3 restirDirect;
    bool restirActive = oloReSTIRDIDirectLighting(restirDirect);
    if (restirActive)
    {
        unsplitDirect += restirDirect;
    }

    bool fplusActive = !restirActive && (fplus_Params.z != 0u);
    if (fplusActive)
    {
        float fplusViewDepth = -(u_View * vec4(worldPos, 1.0)).z;
        Lo = oloSurfaceLightingAdd(Lo, fplusEvaluateTileLightsSplit(N, V, worldPos, albedo, metallic,
                                                                    roughness, fplusViewDepth, pbrModel,
                                                                    skinLobe, skinOralLane));
    }

    // DIRECTIONAL-ONLY when either ReSTIR DI or Forward+ answered for the rest:
    // the multi-light array is ordered directional-first, so the truncated count
    // walks exactly the lights the other mechanism did NOT cover. Walking all of
    // them would double-count every punctual and area light.
    // AND THE SAME TRUNCATION BOUNDS THE TRANSMISSION LOBE, WHICH IS A STATED
    // GAP RATHER THAN AN OVERSIGHT (issue #1234). When ReSTIR DI or Forward+
    // owns the punctual lights, this loop walks only the directional ones, so
    // foliage transmission from a torch or a lamp is not evaluated on those
    // tiers — the reflected lobe for those lights arrives already combined
    // (ReSTIR) or through a tile evaluator with no transmission arm (Forward+),
    // and splitting either to recover a per-light L is that tier's own work.
    //
    // The demonstrating case is unaffected: transmission is a SUN effect, and
    // the sun is a directional light, which is exactly what stays on this loop.
    int loopCount = (restirActive || fplusActive) ? min(u_DirectionalLightCount, MAX_LIGHTS)
                                                  : min(u_LightCount, MAX_LIGHTS);
    for (int i = 0; i < loopCount; ++i)
    {
        int lightType = int(u_Lights[i].position.w);
        // THE LAYERED CLOSURE (issue #1243), the same call the forward paths
        // make, so a skin pixel shades identically whichever path drew it. For
        // a lobe mix of 0 — every non-skin pixel and every profile below
        // transport version 3 — it is the identical single-lobe result and
        // evaluates the closure once.
        OloSurfaceLighting lightContrib = oloSkinLightContributionSplit(
            u_Lights[i], N, V, albedo, metallic, roughness, worldPos, pbrModel, skinLobe);

        // THE VISIBILITY FACTOR, ACCUMULATED RATHER THAN APPLIED (issue #1234).
        //
        // Every branch below used to call oloSurfaceLightingScale on
        // lightContrib in place. They now multiply into ONE float that is
        // applied once, at the bottom of the loop.
        //
        // EXACTLY EQUIVALENT for the reflected lobe: those were a chain of
        // multiplies by a scalar broadcast to vec3, and multiplication is
        // associative, so folding the cloud shadow and the shadow-map factor
        // into one scalar first produces the same product. What it BUYS is that
        // the transmission lobe can be gated by the same number — which is what
        // #1234's second criterion demands, and what a per-branch in-place scale
        // made impossible without evaluating the shadow a second time.
        float lightVisibility = 1.0;

        // THE NORMAL THE SHADOW IS BIASED ALONG. For a foliage pixel this is the
        // LIT-SIDE normal, not the shading normal: a backlit leaf's shading
        // normal points away from the light, so the receiver normal-offset would
        // push the sample point into the leaf's own shadow-map depth and collapse
        // the very term it is gating. oloFoliageShadowNormal returns N unchanged
        // wherever dot(N, L) > 0 — i.e. wherever the reflected lobe is non-zero —
        // so ONE lookup serves both lobes, and this is identity for every
        // non-foliage surface.
        vec3 shadowN = N;
        vec3 lightL;
        vec3 lightRadiance;
        bool lightHasDirection = oloLightSample(u_Lights[i], worldPos, lightL, lightRadiance);
        if (isFoliage && lightHasDirection)
            shadowN = oloFoliageShadowNormal(N, lightL);
        // A BACKLIT SKIN pixel needs the same lit-side bias, for the same
        // reason (issue #1242): its shading normal points away from the light,
        // so the receiver normal-offset would push the shadow sample into the
        // head's own depth and collapse the term it is gating.
        //
        // `else if`, not a second `if`: the two are mutually exclusive because a
        // pixel has ONE material kind, and writing them as independent tests
        // would suggest a pixel could be both and leave the order mattering.
        else if (isSkinTransmitting && lightHasDirection)
            shadowN = oloSkinShadowNormal(N, lightL);

        if (lightType == DIRECTIONAL_LIGHT)
        {
            lightVisibility *= cloudShadow;
        }
        // The ray-traced branch is tested BEFORE u_DirectionalShadowEnabled,
        // not inside it. That flag belongs to the CSM: only the FIRST
        // directional light sets it, because only one light gets the cascades.
        // The ray-traced tier has no such limit and Scene.cpp will hand a mask
        // channel to a second sun — gating on the flag would trace that
        // channel, count it as a success, and never read it.
        float rayTracedDirectional;
        if (lightType == DIRECTIONAL_LIGHT && oloRayTracedShadowFactor(i, rayTracedDirectional))
        {
            lightVisibility *= rayTracedDirectional;
        }
        else if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
            // Virtual Shadow Maps own the directional light when active (issue
            // #702); the CSM cascades are not even rendered in that case, so this
            // is an either/or, never a blend. VSM_ENABLED is uploaded by
            // VirtualShadowMap::BindForSampling, which publishes a DISABLED block
            // when the system is off — so this branch is safe to compile in
            // unconditionally and needs no second shader variant.
            float shadow;
            if (VSM_ENABLED != 0)
            {
                shadow = vsmShadowFactor(worldPos, shadowN);
            }
            else
            {
                vec4 viewSpacePos = u_View * vec4(worldPos, 1.0);
                float viewDepth = viewSpacePos.z;
                shadow = calculateCascadedShadowFactorCSM(
                    u_ShadowMapCSM,
                    u_ShadowMapCSMRaw,
                    worldPos,
                    shadowN,
                    viewDepth,
                    u_DirectionalLightSpaceMatrices,
                    u_CascadePlaneDistances,
                    u_ShadowParams,
                    u_ShadowMapResolution,
                    u_SoftShadowMode);
            }
            lightVisibility *= shadow;
        }
        else if (lightType == SPOT_LIGHT)
        {
            // Spot light shadows come from the light's shadow-atlas entry
            // (issue #435), or from its VSM layer (issue #703); direction.w
            // carries whichever index is live (-1 = none), and vsmLocalShadow is
            // what decides which — see its comment for why the caller must not
            // fall through to the atlas arrays on the VSM branch.
            int atlasEntry = int(u_Lights[i].direction.w);
            float localShadow;
            if (oloRayTracedShadowFactor(i, localShadow))
            {
                lightVisibility *= localShadow;
            }
            else if (vsmLocalShadow(worldPos, shadowN, atlasEntry, false, localShadow))
            {
                lightVisibility *= localShadow;
            }
            else if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
            {
                float shadow = calculateAtlasEntryShadow(
                    worldPos,
                    u_AtlasEntryMatrices[atlasEntry],
                    u_AtlasEntryScaleOffset[atlasEntry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_AtlasDepthBias,
                    u_AtlasResolution,
                    u_SoftShadowMode,
                    u_ShadowParams.z);
                lightVisibility *= shadow;
            }
        }
        else if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
        {
            // Sphere area lights shadow from their centre (the representative
            // point), so both types share the point path: direction.w carries
            // the BASE atlas entry of the 6 cube-face tiles (issue #435).
            int baseEntry = int(u_Lights[i].direction.w);
            float localShadow;
            if (oloRayTracedShadowFactor(i, localShadow))
            {
                lightVisibility *= localShadow;
            }
            else if (vsmLocalShadow(worldPos, shadowN, baseEntry, true, localShadow))
            {
                lightVisibility *= localShadow;
            }
            else if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
            {
                vec3 lightPos = u_Lights[i].position.xyz;
                int entry = baseEntry + atlasCubeFace(worldPos - lightPos);
                float shadow = calculateAtlasEntryShadow(
                    worldPos,
                    u_AtlasEntryMatrices[entry],
                    u_AtlasEntryScaleOffset[entry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_AtlasDepthBias,
                    u_AtlasResolution,
                    0, // PCF only on cube faces (matches the old cubemap path)
                    u_ShadowParams.z);
                lightVisibility *= shadow;
            }
        }

        // THE WET COAT (issue #1245) — same placement and same argument as the
        // forward path: AFTER the surface closure, because the coat attenuates
        // what that produced; BEFORE the visibility, so the film's own highlight
        // is gated by the same shadow factor the surface and transmitted lobes
        // are. The cosine is applied to the radiance here because oloLightSample
        // returns it without one while the closure folds it in.
        if (skinOralLane.x > 0.0 && lightHasDirection)
        {
            vec3 coatRadiance = lightRadiance * max(dot(N, lightL), 0.0);
            lightContrib = oloSkinOralApplyCoat(lightContrib, N, V, lightL, coatRadiance, skinOralLane);
        }

        Lo = oloSurfaceLightingAdd(Lo, oloSurfaceLightingScale(lightContrib, vec3(lightVisibility)));

        // The TRANSMITTED lobe, gated by the SAME visibility the reflected lobe
        // just was. That single shared factor is the whole of #1234's second
        // criterion: a leaf behind a trunk stops glowing, because whatever
        // darkens its lit face also darkens what comes through it.
        //
        // Skipped for a sphere area light, which oloLightSample declines to give
        // a direction for — that evaluator's representative point depends on N
        // and V, so there is no single L to transmit along. Declined in the
        // helper rather than approximated here.
        if (lightHasDirection && leafThickness > 0.0)
        {
            transmitted += oloFoliageTransmissionDirect(N, V, lightL, lightRadiance, lightVisibility,
                                                        leafThickness, leafTint, leafLobe);
        }

        // THE SKIN TRANSMITTED LOBE (issue #1242), gated by the SAME
        // `lightVisibility` — which is the whole of that issue's second
        // criterion, and it is only expressible because #1234 had already
        // refactored this loop to accumulate one visibility factor instead of
        // scaling in place.
        //
        // The two transmission terms share the `transmitted` accumulator and can
        // never both fire: a pixel has one material kind, so isFoliage and
        // isSkinTransmitting are mutually exclusive by construction.
        if (lightHasDirection && isSkinTransmitting)
        {
            // THE CAVITY WEIGHT (issue #1245) multiplies HERE and nowhere else,
            // for the reason the forward path states: the transmitted lobe is
            // the only term not already occluded, and applying an occlusion
            // twice is as wrong as applying it never. A zero CavityOcclusion
            // returns exactly 1.
            transmitted += oloSkinTransmissionDirect(N, V, lightL, lightRadiance, lightVisibility,
                                                     albedo, skinThicknessMM,
                                                     skinTransmitScatter, skinTransmitScaling)
                           * oloSkinOralCavityWeight(ao, skinOralLane.w);
        }
    }

    // Specular reflection source (issue #705): the global prefilter map at
    // the mirror direction, parallax-corrected per pixel by the distance-
    // impostor probes wherever one covers the shading point. SSR composites
    // LATER over the lit colour (PostProcess_SSR lerps by its confidence),
    // so this term is exactly what SSR misses fall back to — the intended
    // ladder is SSR (on-screen) -> probe raymarch -> global sky prefilter.
    //
    // Guarded: this shared body is also compiled into consumers that never
    // declare the probe arrays (DDGI_Relight, the G-Buffer overlay shaders)
    // — those keep the plain global-prefilter source.
    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = textureLod(u_PrefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
#ifdef OLO_REFLECTION_PROBE_SAMPLERS
    if (enableIBL)
    {
        float probeViewDepth = -(u_View * vec4(worldPos, 1.0)).z;
        vec4 probeSpecular = oloSampleReflectionProbes(worldPos, N, R,
                                                       roughness * MAX_REFLECTION_LOD, probeViewDepth);
        prefilteredColor = mix(prefilteredColor, probeSpecular.rgb, probeSpecular.a);
    }
#endif

    // THE INDIRECT-DIFFUSE TIER, decided once and applied in two places: it
    // suppresses the ladder's diffuse rungs below, and it supplies the term
    // itself at the composite. Asking twice would let the two answers drift.
    vec3 restirIndirect;
    bool restirGIActive = oloReSTIRGIIndirectDiffuse(restirIndirect);
    bool restirPTActive = restirGIActive && u_MSAAParams.z > 1.5;

    OloSurfaceLighting ambient = oloSurfaceLightingZero();
    if (restirGIActive)
    {
        // The SPECULAR half only, and only when IBL is on at all.
        // calculateCombinedAmbientPrefiltered's DIFFUSE half is what ReSTIR GI
        // replaced, so passing it a zero irradiance keeps the specular IBL /
        // probe term while dropping the diffuse one — rather than skipping the
        // call, which would drop both.
        //
        // With IBL off there is no specular ambient to keep, so the whole term
        // is zero: calculateSimpleAmbient would have put a flat DIFFUSE fill
        // back, which is the one thing this tier has just replaced.
        if (enableIBL && !restirPTActive)
        {
            ambient = calculateCombinedAmbientPrefilteredSplit(vec3(0.0), N, V, albedo, metallic, roughness,
                                                               u_BRDFLutMap, prefilteredColor);
            ambient = oloSurfaceLightingScale(ambient, vec3(iblIntensity));
        }
    }
    else if (bakedGI.a > 0.5)
    {
        // Rung 1 — baked lightmap, mirroring include/AmbientLadder.glsl's first
        // branch (the forward path's definition of this rung).
        //
        // NO FOLIAGE GATE HERE, AND THAT IS THE POINT OF PUTTING THE THICKNESS
        // IN RT5 (issue #1234). Every foliage G-Buffer writer writes coverage
        // 0, and this rung is gated on coverage — so a foliage pixel never
        // reached it before #1234 and still does not, with no new test. The one
        // RESOLVED MSAA leaks BOTH WAYS across a foliage/lightmapped
        // silhouette, and both are bounded. Into this rung: a mixed pixel
        // averages in `thickness / sampleCount` on red where it used to average
        // in 0. Out of it: the thickness read above can pick up averaged
        // irradiance, which is why that read carries its own coverage test.
        // Silhouette pixels only, in resolved-MSAA mode only.
        //
        // The gate is COVERAGE, never the colour: a validly baked pure-black texel is an
        // enclosed surface no indirect light reaches and must keep its darkness
        // instead of falling through and glowing with sky IBL — the exact leak
        // the bake exists to kill. Deliberately not gated on enableProbes: baked
        // GI is its own source, and its scene kill switch is u_LightmapEnabled,
        // which the G-Buffer pass already applied before writing RT5.
        if (enableIBL)
        {
            ambient = calculateCombinedAmbientPrefilteredSplit(bakedGI.rgb, N, V, albedo, metallic, roughness,
                                                               u_BRDFLutMap, prefilteredColor);
            ambient = oloSurfaceLightingScale(ambient, vec3(iblIntensity));
        }
        else
        {
            ambient = calculateLightProbeAmbientSplit(bakedGI.rgb, albedo, metallic, roughness, N, V);
        }
    }
    else if (enableProbes && enableIBL)
    {
        // Issue #632: unified probe sampling — realtime DDGI atlases when a
        // Realtime/Hybrid volume is bound, baked SH otherwise.
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(worldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
        {
            ambient = calculateCombinedAmbientPrefilteredSplit(probeIrradiance, N, V, albedo, metallic, roughness,
                                                               u_BRDFLutMap, prefilteredColor);
            ambient = oloSurfaceLightingScale(ambient, vec3(iblIntensity));
        }
        else
        {
            ambient = calculateIBLPrefilteredSplit(N, V, albedo, metallic, roughness,
                                                   u_IrradianceMap, u_BRDFLutMap, prefilteredColor);
            ambient = oloSurfaceLightingScale(ambient, vec3(iblIntensity));
        }
    }
    else if (enableProbes)
    {
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(worldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
            ambient = calculateLightProbeAmbientSplit(probeIrradiance, albedo, metallic, roughness, N, V);
        else
            ambient = calculateSimpleAmbientSplit(albedo, metallic, ao);
    }
    else if (enableIBL)
    {
        ambient = calculateIBLPrefilteredSplit(N, V, albedo, metallic, roughness,
                                               u_IrradianceMap, u_BRDFLutMap, prefilteredColor);
        ambient = oloSurfaceLightingScale(ambient, vec3(iblIntensity));
    }
    else
    {
        ambient = calculateSimpleAmbientSplit(albedo, metallic, ao);
    }

    // ambient * ao, then the resampled indirect diffuse UNMULTIPLIED — see
    // oloReSTIRGIIndirectDiffuse for why the AO term must not touch it. It joins
    // the DIFFUSE half because that is exactly what that tier estimates; its own
    // comment above says so, and putting it anywhere else would make the diffuse
    // debug view disagree with the tier's documented contract.
    OloSurfaceLighting lighting = oloSurfaceLightingAdd(oloSurfaceLightingScale(ambient, vec3(ao)), Lo);
    if (restirGIActive)
        lighting.Diffuse += restirIndirect;

    // The INDIRECT half of the leaf transmission (issue #1234) — the
    // environment arriving on the FAR face, added ONCE rather than per light.
    //
    // The irradiance is sampled along -N from the same cubemap the ambient
    // ladder's IBL rungs read, which is exactly what the forward path does with
    // its own copy of that map. So this term responds to the sky and the time of
    // day, and is the reason the transmission is not the "unshadowed ambient
    // constant" the issue rules out: the SHADOWED direct half above carries the
    // sun, and this half carries the sky.
    // Gated on enableIBL for the same reason the FORWARD path gates on
    // u_LeafIds.y: "there is no environment" and "the environment is black"
    // must not be told apart by reading a typed-null sampler and hoping. With
    // IBL off this term is zero on BOTH paths by the same test, rather than by
    // two different accidents.
    if (leafThickness > 0.0 && enableIBL)
    {
        vec3 backEnvIrradiance = texture(u_IrradianceMap, -N).rgb * iblIntensity;
        transmitted += oloFoliageTransmissionAmbient(leafThickness, leafTint, backEnvIrradiance, leafLobe);
    }

    // The skin profile, applied to the SPECULAR half alone and at the last
    // moment the two halves are still separable (issue #1231) — the same place
    // and the same order as PBR_MultiLight.glsl. A non-skin pixel reads a
    // neutral tint, so this is a multiply by one.
    lighting = oloApplySkinProfile(lighting, materialKind, skinEvaluationModel, skinSpecularTint);

    // The diffusion hand-off, at the same seam and in the same order as
    // PBR_MultiLight.glsl -- which is what keeps the forward and deferred paths
    // handing the diffusion pass the same number for the same pixel.
    skinDiffuse = oloSkinDiffusionOutput(lighting, materialKind, skinEvaluationModel,
                                         skinProfileSlot,
                                         oloSkinScatteringMask(materialKind, metallic));

    // The four separated outputs, exposed (issue #1231). Returned BEFORE the
    // debug tints below because those composite over a finished frame and would
    // otherwise paint over the thing being inspected.
    int materialDebug = int(u_MSAAParams.w + 0.5);
    if (materialDebug == OLO_MATERIAL_DEBUG_DIFFUSE)
        return lighting.Diffuse;   // linear HDR radiance, Rec.709
    if (materialDebug == OLO_MATERIAL_DEBUG_SPECULAR)
        return lighting.Specular;  // linear HDR radiance, Rec.709
    if (materialDebug == OLO_MATERIAL_DEBUG_PROFILE_ID)
    {
        // Profile identity. Black where the pixel names no profile — which is
        // every non-skin surface — and a distinct hue per slot otherwise. The
        // hue is derived from the slot rather than looked up so adding a slot
        // needs no table: slot 0 is red, and each further slot rotates.
        if (skinProfileSlot >= OLO_SKIN_PROFILE_SLOT_NONE)
            return vec3(0.0);
        float hue = float(skinProfileSlot) / float(OLO_SKIN_PROFILE_SLOT_NONE);
        return clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    }
    if (materialDebug == OLO_MATERIAL_DEBUG_SCATTERING_MASK)
        return vec3(oloSkinScatteringMask(materialKind, metallic)); // unitless [0,1]
    if (materialDebug == OLO_MATERIAL_DEBUG_TRANSMISSION)
    {
        // The transmission term ALONE, linear HDR radiance — the LEAF lobe
        // (issue #1234) or the SKIN lobe (issue #1242), whichever this pixel's
        // material kind selected, since the two share the accumulator and are
        // mutually exclusive.
        //
        // Black on every pixel that is neither Foliage nor transmitting Skin,
        // which makes this view a direct test of whether the kind, the profile
        // slot and the thickness lane reached the G-Buffer at all — not just of
        // the lobe's shape. It is the first thing to look at when a backlit ear
        // does not glow: black here means the DATA did not arrive, non-black
        // here with a dark composite means the compositing did.
        return transmitted;
    }

    // Ordered `lighting + unsplitDirect + emissive` so the sum stays as close to
    // the pre-#1231 `ambient * ao + Lo + emissive` as the regrouping allows: the
    // resampled direct term used to live inside Lo, which is where it lands
    // again here, just outside the split.
    // `transmitted` joins OUTSIDE the diffuse/specular split, beside
    // unsplitDirect and for the same reason: it is a third transport, not a
    // half of the BRDF. Folding it into the diffuse term would have made
    // `materialdiffuse` show something that is not the diffuse lobe and left
    // #1234's fourth criterion — inspect transmission separately — with nowhere
    // to look.
    vec3 color = oloSurfaceLightingSum(lighting) + unsplitDirect + transmitted + emissive;

    if (cascadeDebug && u_DirectionalShadowEnabled != 0)
        color = ApplyCascadeDebug(color, worldPos);

    // Virtual Shadow Map debug views (issue #702), driven by
    // VirtualShadowMapSettings::DebugMode. Applied AFTER the cascade tint because
    // the two are alternatives — VSM replaces the cascades, so they can never be
    // meaningful in the same frame.
    //
    // Deliberately gated only on VSM_DEBUG_MODE and not on
    // u_DirectionalShadowEnabled: the whole point of the residency view is to be
    // usable when the shadow term is coming out wrong, and gating it on the same
    // flag the broken path reads would hide exactly the case it exists for.
    if (VSM_DEBUG_MODE != 0)
        color = mix(color, vsmDebugTint(worldPos, N), 0.85);

    return color;
}

// The summing spelling, for callers with no aux target to write -- the same
// wrapper-over-the-split shape every term in PBRCommon.glsl uses, so the two can
// never disagree.
vec3 ComputeDeferredLit(
    vec3 albedo, float metallic,
    vec3 N, float roughness, float ao,
    vec4 emissiveFlags, vec3 worldPos, vec4 bakedGI)
{
    vec4 ignoredSkinDiffuse;
    return ComputeDeferredLitSplit(albedo, metallic, N, roughness, ao, emissiveFlags,
                                   worldPos, bakedGI, ignoredSkinDiffuse);
}

#endif // DEFERRED_LIGHTING_SHARED_GLSL
