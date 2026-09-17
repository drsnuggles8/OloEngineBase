#ifndef SKIN_TRANSMISSION_GLSL
#define SKIN_TRANSMISSION_GLSL

// =============================================================================
// SkinTransmission.glsl — the thin-region transmission term, issue #1242.
//
// Small and dependency-free ON PURPOSE, exactly as SkinDiffusionCommon.glsl is:
// it is included by the FORWARD paths (PBR_MultiLight*.glsl, through
// PBRCommon.glsl), by the DEFERRED lighting pass
// (include/DeferredLightingShared.glsl) and by the shader-unit probe that pins
// it against the CPU. Nothing in here declares a uniform, samples a texture or
// knows what a light is — the two vec4 lanes and a thickness are the whole of
// its input, so there is exactly one definition of the arithmetic and all three
// callers get it.
//
// NO PHYSICS IS DECIDED HERE. `d` (the Burley scaling, millimetres) and the
// pre-multiplied transport albedo arrive precomputed from
// SkinTransmissionScatterLane / SkinTransmissionScalingLane in
// Renderer/SkinTransmission.h. That file's opening rule is that every physical
// decision is made on the CPU where a test can look at it; this file is the
// transcription, and SkinTransmissionParityEvidenceTest drives
// EvaluateSkinTransmissionLanes against oloSkinTransmissionDirect below to
// prove the transcription is faithful.
//
// THE LANES.
//   scatter: xyz = ScatterColor * Strength   linear Rec.709, unitless [0,1]
//            w   = Anisotropy `g`            unitless [0,1]
//   scaling: xyz = Burley scaling `d`        MILLIMETRES, strictly positive
//            w   = Power `P`                 unitless [1,64]
//
// `thicknessMM` is the authored thickness in MILLIMETRES — already through the
// material's thickness factor, the thickness map and the profile's
// ThicknessScale. Zero means "no thickness authored here" and transmits
// NOTHING: the other reading of a zero thickness is "infinitely thin, therefore
// exp(0) = 1, therefore fully transparent", which renders the uniformly
// emissive head issue #1242's second criterion forbids. See
// docs/guides/skin-transmission.md.
// =============================================================================

// The floor the scaling lane is clamped against before the divide. Mirrors
// kMinSkinTransmissionScalingMM in Renderer/SkinTransmission.h. A sanitized
// profile cannot produce a zero here, so this never binds in production — it is
// what makes a ZERO-FILLED slot (one nobody claimed, or a stale frame) divide by
// something instead of producing an infinity that becomes a NaN pixel.
#define OLO_SKIN_TRANSMISSION_MIN_SCALING_MM 1.0e-6

// The thickness ceiling, mirroring kMaxSkinThicknessMM. Bounds what a corrupt
// thickness map can ask for; past it the transmittance is below the
// quantisation of an RGBA16F target anyway.
#define OLO_SKIN_MAX_THICKNESS_MM 2000.0

// The per-channel fraction of light entering the far face that reaches the near
// one. Unitless [0,1]; `thicknessMM` and `scaling.xyz` both in millimetres.
//
// Red survives a trip a blue photon does not — that is the whole reason a
// backlit ear is red — and it is the authored profile's per-channel mean free
// path that says so, not a tint.
vec3 oloSkinTransmittance(float thicknessMM, vec4 scatter, vec4 scaling)
{
    vec3 opticalDepth = vec3(min(thicknessMM, OLO_SKIN_MAX_THICKNESS_MM)) /
                        max(scaling.xyz, vec3(OLO_SKIN_TRANSMISSION_MIN_SCALING_MM));
    return scatter.xyz * exp(-opticalDepth);
}

// The bounded exit lobe weight, unitless [0,1].
//
// EXACTLY ZERO FOR dot(N, L) >= 0, and that is not a threshold — it is what
// clamp(-dot(N,L), 0, 1) is over the whole near hemisphere. It is premise 1 of
// the energy argument in Renderer/SkinTransmission.h (the reflected diffuse
// lobe carries +dot(N,L), so the two terms can never both be non-zero for one
// light) and it is the "vanishes toward front lighting" half of the issue's
// second acceptance criterion.
//
// NO WRAP PARAMETER, deliberately, and this is where this term differs from
// oloFoliageTransmission in include/FoliageSurface.glsl. A leaf is a thin sheet
// whose two faces are the same surface, so foliage wraps its lobe into
// dot(N,L) > 0 on purpose. A cheek has a near face and an ear has a far one:
// wrapping here would lay transmitted energy on top of the reflected diffuse
// lobe over a whole hemisphere, which is the double-count the fourth criterion
// forbids.
//
// BOUNDED, NOT NORMALISED. `mix` of two values in [0,1] cannot leave [0,1] for
// any inputs, so "the head cannot glow brighter than the light behind it" is a
// fact about the arithmetic rather than about the parameter values. A normalised
// phase function would exceed 1 at its peak and break that.
float oloSkinTransmissionLobe(vec3 N, vec3 V, vec3 L, vec4 scatter, vec4 scaling)
{
    float backFacing = clamp(-dot(N, L), 0.0, 1.0);
    if (backFacing <= 0.0)
        return 0.0;

    // `L` points FROM the surface TOWARD the light (the oloLightSample
    // convention), so -L is the direction transmitted light continues in.
    float forward = clamp(dot(V, -L), 0.0, 1.0);
    return backFacing * mix(1.0, pow(forward, scaling.w), scatter.w);
}

// =============================================================================
// THE DEFERRED THICKNESS LANE — G-Buffer RT5's RED channel
// =============================================================================
//
// The deferred path needs this pixel's thickness, and thickness is PER PIXEL —
// it is the whole point, because an ear is thin where a cheek is not. So it
// cannot ride the profile slot table, which is per PROFILE.
//
// THERE IS NO FREE G-BUFFER CHANNEL. RT0 is albedo + metallic (RGBA8), RT1 is
// the octahedral normal + roughness + AO, RT2 is emissive + the flags lane, RT3
// is velocity, RT4 is the entity id. The flags lane cannot carry it either: it
// is an exact small integer in an RGBA16F alpha, with 11 bits of mantissa, and
// the closure model already occupies everything above bit 6.
//
// SO THIS IS THE SECOND TENANT OF RT5's RED CHANNEL. Foliage got there first
// (issue #1234) and the argument is identical, which is why the two coexist:
// RT5 is baked lightmap irradiance in .rgb with COVERAGE in .a, and a surface
// with no lightmap writes coverage 0 and leaves the channel doing nothing. A
// pixel has ONE material kind, so a foliage thickness and a skin thickness can
// never both claim this channel.
//
// TWO DIFFERENCES FROM THE FOLIAGE TENANCY, both deliberate:
//
//   * THE RANGE. Foliage's thickness is unitless [0,1]; this one is
//     MILLIMETRES, up to OLO_SKIN_MAX_THICKNESS_MM. An RGBA16F half carries
//     2000 exactly and a few hundred with ~11 bits of mantissa, which is finer
//     than any authored thickness map.
//   * THE WRITER DEFERS TO THE LIGHTMAP. Foliage never reaches the baked rung,
//     so it can write the channel unconditionally. A SKIN material CAN be
//     lightmapped — it goes through PBR_GBuffer, which writes real irradiance
//     here — so this writer keeps the irradiance and drops the thickness in
//     that case, rather than corrupting the pixel's ambient light for a term
//     that is an enhancement. The consequence (a uniform thickness instead of a
//     per-pixel one) is COUNTED on the CPU as
//     SkinTransmissionFallbackReason::DeferredThicknessLaneUnavailable, so the
//     degradation is loud rather than silent.
//
// Both helpers live here, next to each other, so the write and the read cannot
// drift apart — the same reason oloEncodeGBufferPbrFlags and its decode share a
// file.

// WRITER. Returns what RT5 should hold: the baked irradiance untouched wherever
// there is any, and otherwise the thickness in .r with coverage 0.
//
// `isSkinTransmitting` is the caller's full gate — kind is Skin, transport is
// version 2, thickness is non-zero — passed as a bool rather than as the kind
// and model ints so this file stays free of PBRCommon.glsl's defines.
vec4 oloSkinPackGBufferThickness(vec4 bakedGI, bool isSkinTransmitting, float thicknessMM)
{
    // COVERAGE FIRST, and it is not merely a priority call. The ambient ladder
    // gates the baked rung on this same coverage, so overwriting a covered
    // texel would take a lightmapped surface's indirect light away and replace
    // it with sky IBL — a visible lighting regression in exchange for a subtle
    // transmission gain, which is the wrong trade in every scene.
    if (!isSkinTransmitting || bakedGI.a > 0.5)
        return bakedGI;

    // Coverage stays 0, so the ambient ladder still falls through to
    // probes/IBL exactly as it did before this channel had a second tenant.
    return vec4(clamp(thicknessMM, 0.0, OLO_SKIN_MAX_THICKNESS_MM), 0.0, 0.0, 0.0);
}

// READER. The thickness a deferred skin pixel carries, MILLIMETRES, or 0.
//
// GATED ON THE PROFILE SLOT, NOT MERELY ON THE KIND, and the difference is a
// real surface — the same one FoliageSurface's reader documents. A Material's
// kind is authorable in the editor, so a mesh material can be set to Skin; only
// a pixel that also NAMES A PROFILE went through the writer above, and only
// those pixels have a thickness here.
//
// AND A COVERAGE TEST, which is about RESOLVED MSAA. GBuffer::Resolve
// average-blits RT5 but overwrites RT2's flags with ONE REAL SAMPLE
// (GBufferFlagsResolve.glsl), so a silhouette pixel that is part skin and part
// lightmapped receiver can resolve its FLAGS to Skin while its RT5 red channel
// is an average including the neighbour's baked irradiance — which is in
// physical units, routinely well under 2000, and would therefore read as a
// perfectly plausible thickness. Requiring coverage below 0.5 drops the pixels
// where the irradiance dominates, and the clamp bounds what is left.
//
// This does not make a mixed pixel exact — nothing short of resolving RT5 the
// way the flags lane is resolved would — it bounds the error to the side where
// skin is the majority and the contamination is small. Silhouette pixels only,
// resolved-MSAA mode only.
float oloSkinUnpackGBufferThickness(vec4 bakedGI, bool hasSkinProfile)
{
    if (!hasSkinProfile || bakedGI.a > 0.5)
        return 0.0;
    return clamp(bakedGI.r, 0.0, OLO_SKIN_MAX_THICKNESS_MM);
}

// The normal a SKIN pixel's shadow lookup must be offset along, for light
// arriving from direction L.
//
// THIS IS THE ONE THAT BITES, and it bit foliage first — see
// oloFoliageShadowNormal in include/FoliageSurface.glsl, whose comment is the
// long version of this one. Cascaded-shadow sampling offsets the receiver along
// its normal to escape self-shadowing acne. For a BACKLIT ear the shading
// normal points at the viewer and therefore AWAY from the light, so offsetting
// along it walks the sample point deeper behind the head's own shadow-map depth:
// the head shadows itself, the shared visibility factor collapses to zero, and
// the transmission term it was supposed to gate goes black. The symptom is
// "backlit ears do not glow", which reads as the lobe being wrong and is
// actually the bias having the wrong sign.
//
// IDENTITY WHEREVER THE REFLECTED LOBE IS NON-ZERO. Where dot(N, L) > 0 this
// returns N unchanged, so ONE shadow lookup serves both lobes and they can never
// disagree about whether this pixel is lit — which is what lets the transmission
// term be gated by the SAME `lightVisibility` the reflected lobe is scaled by,
// as the issue's second criterion requires. Front-lit skin and every non-skin
// surface are bit-identical to before.
//
// KEPT SEPARATE FROM THE FOLIAGE SPELLING although the expression is the same,
// for the reason FoliageLeafProfile.h gives about the slot constants: the two
// are tenants of one problem, not one concept. A leaf is a thin sheet whose two
// faces are the same surface and flipping the normal names its other face; a
// head is solid and flipping the normal names the direction the light came from.
// Aliasing them would let a future change to the leaf rule silently retune skin.
//
// ITS LIMIT, STATED: a region THICKER than the shadow-map normal offset can
// still self-shadow, so the term weakens on genuinely thick backlit geometry.
// That fails in the conservative direction — a thick region should not transmit
// much anyway — but it fails for the wrong reason, and
// docs/guides/skin-transmission.md says so rather than leaving it to be
// rediscovered.
vec3 oloSkinShadowNormal(vec3 N, vec3 L)
{
    return (dot(N, L) < 0.0) ? -N : N;
}

// The transmitted radiance for ONE light, linear HDR, Rec.709.
//
// `visibility` MUST be the same shadow/occlusion factor the reflected lobe was
// scaled by. That shared factor is the whole of the issue's second criterion:
// an ear behind a raised hand stops glowing, because whatever darkens its lit
// face darkens what comes through it. Passing 1.0 here "because transmission is
// backlighting" is the bug this comment exists to prevent.
//
// The multiply order is EvaluateSkinTransmissionLanes' order, operation for
// operation. Do not reassociate: the parity test compares last bits.
vec3 oloSkinTransmissionDirect(vec3 N, vec3 V, vec3 L, vec3 radiance, float visibility,
                               vec3 albedo, float thicknessMM, vec4 scatter, vec4 scaling)
{
    if (thicknessMM <= 0.0)
        return vec3(0.0);

    float lobe = oloSkinTransmissionLobe(N, V, L, scatter, scaling);
    if (lobe <= 0.0)
        return vec3(0.0);

    vec3 transmittance = oloSkinTransmittance(thicknessMM, scatter, scaling);
    return radiance * transmittance * albedo * lobe * visibility;
}

#endif // SKIN_TRANSMISSION_GLSL
