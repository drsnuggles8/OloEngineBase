// =============================================================================
// SkinLayeredSpecular.glsl — the layered surface response, issue #1243.
//
// THE MATHS LIVES ON THE CPU, in Renderer/SkinLayeredSpecular.h, and that file
// is where the physical decisions, the energy argument and the measured
// evidence are. Everything here is a transcription of it, in the same order,
// operation for operation, and SkinLayeredSpecularParityTest drives this file
// against that one so the two cannot drift.
//
// INCLUDED BY include/PBRCommon.glsl, after evaluatePBRClosureSplit, so every
// shader that can shade skin gets the same arithmetic: the forward paths
// (PBR_MultiLight{,_Skinned}.glsl), the G-Buffer writers
// (PBR_GBuffer{,_Skinned}.glsl) and the deferred lighting pass
// (include/DeferredLightingShared.glsl, which includes PBRCommon).
//
// -----------------------------------------------------------------------------
// WHERE EACH HALF OF THE FEATURE IS SPENT, which is not the same place
// -----------------------------------------------------------------------------
//
//   THE VARIANCE FILTER runs where the NORMAL IS BUILT — in the forward shaders
//   and in the G-Buffer writers. It produces a roughness, and on the deferred
//   path that roughness is written into the G-Buffer, so the lighting pass
//   inherits it with no knowledge that anything was filtered. That placement is
//   forced rather than chosen: the filter needs dFdx of the shading normal, and
//   a FULLSCREEN pass cannot take a derivative across a G-Buffer without
//   straddling object edges, where the normal discontinuity is not sub-pixel
//   variance but two different surfaces. Filtering there would draw a rough halo
//   around every silhouette in the scene.
//
//   THE LOBE MIXTURE runs at LIGHTING time, because it is a property of the
//   BRDF and not of the surface. It therefore has to reach the deferred pass as
//   data, which is what the lane in the per-frame profile table is for.
//
// -----------------------------------------------------------------------------
// WHAT THE MIXTURE IS APPLIED TO, AND WHAT IT IS NOT
// -----------------------------------------------------------------------------
//
// PUNCTUAL AND AREA LIGHTS: yes. IBL, light probes and the ambient ladder: NO,
// and deliberately.
//
// The reason is a cost/benefit the measurement settles rather than a limitation
// nobody got to. Layering the image-based specular means a SECOND prefiltered
// cubemap lookup per pixel — the expensive kind, a different mip of a cubemap —
// and the measured gain from the second lobe is one to two percentage points of
// RMS on top of the variance filter, against the filter's own thirty-plus. The
// filtered roughness DOES flow into the IBL path, because it is just the
// roughness by the time anything image-based sees it, so the large win is
// already there. Spending a cubemap fetch to chase the small one is not a trade
// this feature can justify, and saying so here is better than leaving the next
// reader to wonder whether it was forgotten.
// =============================================================================

// The upper bound on how much variance one pixel may add to the lobe, in the
// alpha-squared units the variance is added in. Mirrors
// kSkinVarianceKernelClamp in Renderer/SkinLayeredSpecular.h.
//
// NOT A TASTE BOUND: without it a silhouette pixel, where the shading normal
// swings most of a hemisphere between neighbours, adds an unbounded amount and
// the surface goes fully rough along every outline — a bright halo rather than a
// soft one.
#define OLO_SKIN_VARIANCE_KERNEL_CLAMP 0.18

// GGX alpha widened by the screen-space variance of the shading normal.
//
//   kernel = min(2 * sigma^2 * (|dN/dx|^2 + |dN/dy|^2), clamp)
//   alpha' = sqrt(alpha^2 + kernel)
//
// THE VARIANCES ADD. GGX's alpha^2 IS a slope variance and the pixel's normal
// spread is another one, so the sum is the physical composition — which is why
// this cooperates with an authored roughness map instead of competing with it,
// and degrades to each of them when the other is zero.
//
// ONLY EVER ROUGHENS. The other direction is a sharpening filter, and a
// sharpening filter cannot remove aliasing; it manufactures it.
float oloSkinFilteredAlpha(float alpha, vec3 dNdx, vec3 dNdy, float varianceStrength)
{
    float variance = varianceStrength * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
    float kernel = min(2.0 * variance, OLO_SKIN_VARIANCE_KERNEL_CLAMP);
    return clamp(sqrt(alpha * alpha + kernel), 0.0, 1.0);
}

// The same filter expressed in the PERCEPTUAL roughness every material authors
// and every closure in this file takes, with the derivatives read off `N` here
// so no caller has to remember to take them of the FINAL shading normal.
//
// THE FINAL normal is load-bearing and is the easy thing to get wrong: taking
// the derivative of the interpolated VERTEX normal measures the mesh's curvature
// and misses the normal map entirely, which is the whole of the pore detail this
// filter exists to tame. Call this AFTER applyNormalMapTBN, never before.
//
// A varianceStrength of exactly 0 returns `roughness` unchanged — not
// approximately, exactly, since sqrt(alpha^2 + 0) is alpha and sqrt(alpha)
// inverts the square below it. That exactness is what makes "filtering off" an
// A/B control rather than a similar-looking frame.
float oloSkinFilteredRoughness(float roughness, vec3 N, float varianceStrength)
{
    if (varianceStrength <= 0.0)
        return roughness;
    float alpha = roughness * roughness;
    float filtered = oloSkinFilteredAlpha(alpha, dFdx(N), dFdy(N), varianceStrength);
    return sqrt(filtered);
}

// The BROAD lobe's roughness.
//
// THE SCALE IS ON ROUGHNESS, NOT ON ALPHA, and the two differ by a square, so
// the number here is not the number the reference experiment fitted: its fitted
// alpha ratios of 1.3 to 4.0 are roughness ratios of 1.14 to 2.0. Roughness is
// the quantity a material authors and the quantity every closure in this file
// takes, and converting in the shader would have put a sqrt between the author's
// slider and its effect for no gain.
float oloSkinBroadRoughness(float roughness, float lobeRoughnessScale)
{
    return clamp(roughness * lobeRoughnessScale, 0.0, 1.0);
}

// The convex mixture, and the ONE place it is written on this side.
//
// `narrow + w * (broad - narrow)` rather than `(1 - w) * narrow + w * broad`:
// at w == 0 the first returns `narrow` EXACTLY for every finite input, which is
// what makes LobeMix 0 the bit-identical single-lobe frame a golden image can
// assert against.
vec3 oloSkinSpecularMix(vec3 narrow, vec3 broad, float broadWeight)
{
    return narrow + broadWeight * (broad - narrow);
}

// The layered closure: the versioned PBR closure evaluated at two roughnesses
// and mixed, DIFFUSE TAKEN FROM THE NARROW ARM.
//
// Taking the diffuse from one arm is exact for Legacy and a stated
// approximation for ClosureV2. Legacy's diffuse half is `kD * albedo / PI` with
// kD built from the half-vector Fresnel, which does not depend on roughness, so
// both arms' diffuse values are identical. ClosureV2's Lambert is weighted by
// the energy-conserving coupling of issue #1479, which subtracts the specular
// lobe's directional albedo E_spec — and that DOES depend on roughness, so the
// broad arm's diffuse differs from the narrow arm's.
//
// The narrow arm's diffuse is kept anyway, deliberately: #1243's contract is
// that the lobe mixture never touches the diffusion (SkinLayeredSpecular-
// EvidenceTest's first claim, measured on the diffuse AOV), and the mixture of
// a narrow-arm diffuse with a mixed specular departs from the fully mixed
// closure by w (D_broad - D_narrow), which in energy is w (E_spec,narrow -
// E_spec,broad) x albedo: small head-on, where a dielectric's E_spec is ~F0 at
// any roughness, and largest at grazing view, where a narrow lobe's E_spec
// climbs toward 1 faster than a broad one's. Mixing the diffuse halves too
// would make the mixture exactly energy conserving and break that contract;
// THIS is the line that changes if that trade is ever reversed.
//
// `skinLobe` is the profile lane's xy: x = LobeMix (w), y = LobeRoughnessScale.
// x <= 0 returns the single-lobe result without evaluating anything twice, so a
// non-skin surface and a skin surface whose author left the mixture at zero both
// cost exactly what they cost before this feature existed.
OloSurfaceLighting oloSkinLayeredClosureSplit(int pbrModel, vec3 N, vec3 V, vec3 L,
                                              vec3 albedo, float metallic, float roughness,
                                              vec2 skinLobe)
{
    OloSurfaceLighting narrow = evaluatePBRClosureSplit(pbrModel, N, V, L, albedo, metallic, roughness);
    if (skinLobe.x <= 0.0)
        return narrow;

    float broadRoughness = oloSkinBroadRoughness(roughness, skinLobe.y);
    OloSurfaceLighting broad = evaluatePBRClosureSplit(pbrModel, N, V, L, albedo, metallic, broadRoughness);
    return OloSurfaceLighting(narrow.Diffuse,
                              oloSkinSpecularMix(narrow.Specular, broad.Specular, skinLobe.x));
}

// One light's contribution with the layered closure — the skin twin of
// calculateLightContributionSplit, and a near-copy of it on purpose.
//
// A COPY RATHER THAN AN EXTRA ARGUMENT ON THE ORIGINAL, because the original has
// eight callers across the foliage, terrain and voxel shaders as well as these,
// and every one of them would have had to grow a `vec2(0.0, 1.0)` that means
// "not skin". A parameter whose only legal value at seven of eight call sites is
// a magic constant is a parameter that will eventually be passed wrong.
//
// The sphere-area branch delegates to the shared evaluator UNLAYERED. That
// evaluator uses the representative-point approximation, which splits the BRDF
// differently — it perturbs the light direction and re-normalizes the specular
// by a roughness-dependent factor — so evaluating it twice at two roughnesses
// and mixing does not give the mixture of two area-light responses, it gives an
// expression with no physical reading at all. Skin under a sphere-area light
// therefore gets the filtered roughness (which is most of the benefit) and one
// lobe. Stated here because the alternative is someone "fixing" it later.
OloSurfaceLighting oloSkinLightContributionSplit(LightData light, vec3 N, vec3 V, vec3 albedo,
                                                 float metallic, float roughness, vec3 worldPos,
                                                 int pbrModel, vec2 skinLobe)
{
    int lightType = int(light.position.w);

    if (lightType == SPHERE_AREA_LIGHT)
    {
        return calculateLightContributionSplit(light, N, V, albedo, metallic, roughness, worldPos, pbrModel);
    }

    vec3 L;
    vec3 radiance;
    if (!oloLightSample(light, worldPos, L, radiance))
        return oloSurfaceLightingZero();

    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= EPSILON)
        return oloSurfaceLightingZero();

    OloSurfaceLighting brdf = oloSkinLayeredClosureSplit(pbrModel, N, V, L, albedo, metallic, roughness, skinLobe);
    return oloSurfaceLightingScale(brdf, radiance * NdotL);
}

// -----------------------------------------------------------------------------
// The expression-driven detail normal
// -----------------------------------------------------------------------------

// Scale the HIGH-FREQUENCY half of an already-sampled tangent normal.
//
//   result = normalize(fine + (fine - coarse) * extraStrength)
//
// `fine` is the normal map at the fragment's own mip; `coarse` is the SAME map
// a couple of mips up. Their difference is, by construction, the band the
// coarser mip threw away — which for a skin normal map is the pores and the fine
// furrows, and is exactly the band an expression is supposed to deepen.
//
// WHY THERE IS NO SEPARATE DETAIL MAP. A second texture would need a tenth
// material texture slot, a tenth heap offset, an asset field, an importer path
// and an authoring convention, and it would introduce a way for the detail to
// DESYNC from the base normal — a wrinkle map authored against one UV layout and
// a normal map against another agree everywhere except where it matters. Taking
// the band out of the map that is already there cannot desync, costs one extra
// sample of a texture that is already resident, and modulates the detail the
// artist actually authored rather than a second opinion about it.
//
// ITS LIMIT, PLAINLY: this deepens the pores the map already has, EVERYWHERE on
// the surface, in proportion to how far from neutral the face is. It does not
// know that a brow furrows while a cheek does not. Per-region wrinkle response
// needs a mask per morph target, which is a facial-rig authoring tool — issue
// #1243's scope boundary excludes it in those words and #1245 owns it.
//
// `extraStrength` 0 returns `fine` unchanged (it is already unit length), which
// is the neutral default; -1 returns the coarse normal, which is the exact
// "detail off" arm the acceptance criteria are demonstrated against.
vec3 oloSkinDetailTangentNormal(vec3 fineTangentNormal, vec3 coarseTangentNormal, float extraStrength)
{
    if (extraStrength == 0.0)
        return fineTangentNormal;

    vec3 detail = fineTangentNormal - coarseTangentNormal;
    vec3 combined = fineTangentNormal + detail * extraStrength;

    // A fully cancelled result (extraStrength == -1 on a map whose two mips
    // agree, i.e. a flat region) normalizes to NaN. Guard with the same
    // `!(x > eps)` form PBRCommon.glsl's degeneracy guards use, so a NaN input
    // also takes the fallback instead of being renormalized into one.
    float lenSq = dot(combined, combined);
    if (!(lenSq > kDegenerateEpsilon))
        return fineTangentNormal;

    return combined * inversesqrt(lenSq);
}

// How many mips up the "coarse" sample is taken. Two, not one: one mip is a
// 2x2 box filter, and the difference between a texel and its own 2x2 average is
// dominated by the sampling grid rather than by anything on the skin, so the
// "detail" it isolates is half noise. Two mips is a 4x4 footprint, which at the
// texel densities a head is authored at is about the size of a pore.
//
// A CONSTANT AND NOT AN AUTHORED FIELD, because it is a property of what a pore
// IS relative to a texel, not a taste: an author who wants more detail turns
// the strength up, and an author whose map is at a wildly different density has
// a mip-density problem this knob would only disguise.
#define OLO_SKIN_DETAIL_LOD_OFFSET 2.0

// The non-heap spelling of OLO_SKIN_MAT_NORMAL (include/PBRCommon.glsl), which
// is where the two are documented. A real function on this path for the reason
// getNormalFromMap is one: a function body is readable and a macro body is not,
// and only the Vulkan heap reader actually needs the inlining.
//
// `detailStrength` 0 takes the single-sample path and is bit-identical to
// getNormalFromMap, so a skin material with no detail authored costs what it
// always cost.
vec3 oloSkinNormalFromMap(sampler2D normalMap, vec2 texCoords, vec3 worldPos, vec3 normal,
                          float normalScale, float detailStrength)
{
    vec3 fine = decodeTangentNormal(texture(normalMap, texCoords).xy, normalScale);

    if (detailStrength != 0.0)
    {
        float coarseLod = textureQueryLod(normalMap, texCoords).y + OLO_SKIN_DETAIL_LOD_OFFSET;
        vec3 coarse = decodeTangentNormal(textureLod(normalMap, texCoords, coarseLod).xy, normalScale);
        fine = oloSkinDetailTangentNormal(fine, coarse, detailStrength);
    }

    return applyNormalMapTBN(fine,
                             dFdx(worldPos), dFdy(worldPos),
                             dFdx(texCoords), dFdy(texCoords),
                             normal);
}

// The lobe pair for a pixel, or the neutral single-lobe answer.
//
// THE ONE PLACE THE VERSION TEST IS SPELLED on this side, mirroring
// SkinEvaluatesLayeredSpecular in Renderer/SkinLayeredSpecular.h, so the forward
// paths, the clustered path and the deferred pass cannot disagree about which
// pixels are layered.
//
// AN EXPLICIT LIST OF THE VERSIONS THAT LAYER, not an `==` and not a `>=`,
// matching every version branch in this engine and for their reason: a version
// this shader has no arm for applies NOTHING rather than guessing that a later
// transport meant the same thing by these fields.
//
// BOTH LAYERING VERSIONS. The versions are CUMULATIVE — version 4 is
// "everything version 3 does, plus the oral terms" — so a version-4 profile
// must still get its lobe pair. This function was the ONE list #1245 missed
// when it appended version 4, and the symptom was exact: a neutral version-4
// profile rendered 11 levels away from the version-3 frame across 38 221
// pixels, identically on all three raster paths, because the CPU packed the
// lane (SkinEvaluatesLayeredSpecular had been updated) and the shader then
// threw it away. Caught by SkinOralSurfaceEvidenceTest's neutral-identity A/B,
// which is the third time that shape of assertion has paid for itself — see
// the comment in Renderer/SkinDiffusion.cpp for the first two.
//
// BELT AND BRACES over the CPU, which already zeroes the lane for every
// non-layering profile — but the DEFERRED table is indexed by a slot that can
// be stale by a frame after a scene change, and a stale slot must lose the
// effect rather than acquire someone else's lobe.
vec2 oloSkinLobeFor(int materialKind, int evaluationModel, vec4 skinSpecularLane)
{
    if (materialKind != OLO_MATERIAL_KIND_SKIN)
        return vec2(0.0, 1.0);
    // ALL THREE LAYERING VERSIONS. Version 5 (issue #1244) is "everything
    // version 4 does, plus the cornea", so omitting it here silently returned a
    // version-5 eye to ONE lobe — the fourth time this shape of omission has
    // been caught, and the first time it was caught by a neutral-identity A/B
    // rather than by somebody looking at a frame. It was worth 5 levels out of
    // 255 on a sclera: far too little to notice, far too much to be nothing.
    if (evaluationModel != OLO_SKIN_MODEL_LAYERED_SPECULAR &&
        evaluationModel != OLO_SKIN_MODEL_ORAL_SURFACE &&
        evaluationModel != OLO_SKIN_MODEL_OCULAR_SURFACE &&
        evaluationModel != OLO_SKIN_MODEL_ISOTROPIC_GATHER)
        return vec2(0.0, 1.0);
    return skinSpecularLane.xy;
}
