// =============================================================================
// ShaderUnit_SkinOralSurface.glsl
//
// Pins the contracts issue #1245 added to the shading language, by CALLING the
// production functions in include/SkinOralSurface.glsl rather than transcribing
// them — the same discipline ShaderUnit_SkinLayeredSpecular.glsl and
// ShaderUnit_SkinTransmission.glsl use, and for the same reason: a
// transcription tests the copy.
//
// The other callers of these exact functions are PBR_MultiLight{,_Skinned}.glsl
// (forward), include/ForwardPlusCommon.glsl (clustered) and
// include/DeferredLightingShared.glsl (deferred). If this probe agrees with the
// CPU, all four do, because there is one implementation.
//
// WHAT THIS ADDS OVER SkinOralSurfaceTest. That test pins the CPU maths and its
// energy argument; this one pins that the GLSL is the SAME maths.
// Renderer/SkinOralSurface.h calls itself the specification, and a
// specification nothing is compared against is a comment.
//
// ONE COLUMN PER NAMED CLAIM. Each texel's x coordinate selects a case; the CPU
// side asserts the claim the case is named for. The probe is 1 pixel tall.
//
//   0  F0 PARITY          — oloSkinOralCoatF0 against the CPU's SkinOralCoatF0
//                           for saliva's 1.33. The EQUALITY case: every other
//                           case below is a property, and a property can hold
//                           while the number is wrong by a factor of ten.
//   1  F0, ENAMEL         — and again at enamel's 1.63, because ONE agreement
//                           can be a coincidence of the formula's fixed point.
//   2  F0 AT IOR 1 IS ZERO— a coat of the index of air reflects nothing. The
//                           second, redundant spelling of "dry", and the reason
//                           the bound's floor is 1 rather than above it.
//   3  FRESNEL PARITY     — oloSkinOralCoatFresnel against SkinOralCoatFresnel
//                           at an oblique angle. Schlick is four multiplies on
//                           both sides, so this is an exact comparison.
//   4  FRESNEL AT GRAZING — the Fresnel goes to 1 as cos goes to 0, which is
//                           what makes a wet surface rim-bright.
//   5  THE PARTITION      — attenuation + strength * fresnel == 1, EXACTLY, for
//                           an arbitrary legal pair. This is the energy
//                           statement the whole feature rests on, checked as an
//                           identity rather than as a bound.
//   6  COAT LOBE PARITY   — oloSkinOralCoatSpecular against
//                           SkinOralCoatSpecular for one oblique, non-
//                           degenerate configuration. The GGX D and the Smith
//                           visibility are shared with PBRCommon.glsl, so this
//                           also pins that the CPU transcribed THOSE correctly.
//   7  COAT LOBE, DEGENERATE — a view and light that cancel have no half-vector
//                           and the lobe is 0 rather than a NaN. The guard that
//                           keeps a NaN out of a whole frame's specular.
//   8  ZERO STRENGTH IS EXACT — oloSkinOralApplyCoat with strength 0 returns
//                           its input UNCHANGED, bit for bit. The A/B control
//                           arm the wet-vs-dry evidence is measured against, and
//                           "almost unchanged" would make the control a third
//                           variant rather than a baseline.
//   9  THE COAT DARKENS THE DIFFUSE — a non-zero strength returns a diffuse
//                           STRICTLY BELOW the input's. The coat takes energy
//                           from the tissue; a coat that left the diffuse alone
//                           would be a highlight painted on top, which is the
//                           thing this file exists not to be.
//  10  AND NEVER INTO THE BLUR — the returned diffuse is also non-negative. The
//                           diffuse half is what oloSkinDiffusionOutput hands
//                           the diffusion pass, so a negative one would be
//                           blurred and re-added as a dark halo.
//  11  CAVITY, ZERO IS EXACTLY ONE — a CavityOcclusion of 0 returns 1.0, bit for
//                           bit, for an arbitrary AO. This is what makes
//                           "cavity off" the identity frame a golden image can
//                           assert against.
//  12  CAVITY, ONE IS THE AO — a CavityOcclusion of 1 returns the AO itself, so
//                           case 11 is not passing by the function being inert.
//  13  CAVITY IS MONOTONE — more occlusion never transmits more. The direction
//                           of the whole "no glowing interiors" claim, as an
//                           inequality.
//  14  LANE GATE, WRONG VERSION — oloSkinEvaluatesOralSurface is false for a
//                           profile below transport version 4, so a stale
//                           deferred slot loses the effect rather than
//                           acquiring someone else's wetness.
//  15  LANE GATE, RIGHT VERSION — and returns the lane for a version-4 skin
//                           pixel, so case 14 is not passing by being broken.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

void main()
{
    gl_Position = vec4(a_Position.xy, 0.0, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Result;

#include "include/PBRCommon.glsl"

// THE FIXTURE, spelled here and mirrored on the CPU side. Deliberately NOT
// derived from a uniform: a probe that took its inputs from a UBO would also be
// testing the upload, and a failure could not tell a bad coat from a bad bind.
const float kSalivaIor = 1.33;
const float kEnamelIor = 1.63;

const float kCoatStrength = 0.45;
const float kCoatRoughness = 0.12;
const float kCavityOcclusion = 0.6;

// An oblique cosine — not 0, not 1, and not the half-way point where a wrong
// exponent would still agree.
const float kObliqueCos = 0.37;

const float kAo = 0.25;

void main()
{
    int caseIndex = int(gl_FragCoord.x);
    vec4 result = vec4(0.0);

    // Shared shading configuration. Oblique on purpose, for the reason the
    // cosine above is: an axis-aligned frame passes with terms dropped.
    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 V = normalize(vec3(0.15, -0.10, 1.0));
    vec3 L = normalize(vec3(0.45, 0.30, 0.84));

    // The lane a version-4 mucosa profile packs, mirroring SkinOralLane.
    vec4 kLane = vec4(kCoatStrength, kCoatRoughness, oloSkinOralCoatF0(kSalivaIor), kCavityOcclusion);

    if (caseIndex == 0) // F0 PARITY — the equality case
    {
        result = vec4(oloSkinOralCoatF0(kSalivaIor), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 1) // F0, ENAMEL
    {
        result = vec4(oloSkinOralCoatF0(kEnamelIor), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 2) // F0 AT IOR 1 IS ZERO
    {
        result = vec4(oloSkinOralCoatF0(1.0), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 3) // FRESNEL PARITY
    {
        result = vec4(oloSkinOralCoatFresnel(kLane.z, kObliqueCos), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 4) // FRESNEL AT GRAZING
    {
        result = vec4(oloSkinOralCoatFresnel(kLane.z, 0.0), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 5) // THE PARTITION
    {
        float fresnel = oloSkinOralCoatFresnel(kLane.z, kObliqueCos);
        float attenuation = oloSkinOralCoatAttenuation(kCoatStrength, fresnel);
        // The sum, so the CPU asserts against 1.0 rather than against two
        // numbers that might both be wrong in the same direction.
        result = vec4(attenuation + kCoatStrength * fresnel, attenuation, fresnel, 1.0);
    }
    else if (caseIndex == 6) // COAT LOBE PARITY
    {
        result = vec4(oloSkinOralCoatSpecular(N, V, L, kCoatRoughness, kLane.z), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 7) // COAT LOBE, DEGENERATE
    {
        result = vec4(oloSkinOralCoatSpecular(N, V, -V, kCoatRoughness, kLane.z), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 8) // ZERO STRENGTH IS EXACT
    {
        OloSurfaceLighting base = OloSurfaceLighting(vec3(0.31, 0.22, 0.18), vec3(0.09, 0.09, 0.09));
        vec4 dryLane = vec4(0.0, kCoatRoughness, kLane.z, kCavityOcclusion);
        OloSurfaceLighting dry = oloSkinOralApplyCoat(base, N, V, L, vec3(2.0), dryLane);
        // The DIFFERENCE from the input, so the CPU asserts against an exact
        // zero rather than against a value it would have to recompute.
        result = vec4(dry.Diffuse - base.Diffuse, 1.0);
        result.z = length(dry.Specular - base.Specular);
    }
    else if (caseIndex == 9) // THE COAT DARKENS THE DIFFUSE
    {
        OloSurfaceLighting base = OloSurfaceLighting(vec3(0.31, 0.22, 0.18), vec3(0.09, 0.09, 0.09));
        OloSurfaceLighting wet = oloSkinOralApplyCoat(base, N, V, L, vec3(2.0), kLane);
        result = vec4(wet.Diffuse.x, base.Diffuse.x, wet.Specular.x, 1.0);
    }
    else if (caseIndex == 10) // AND NEVER INTO THE BLUR
    {
        // The strongest legal coat, so the lower bound is tested where it is
        // nearest to being violated.
        vec4 fullLane = vec4(1.0, kCoatRoughness, oloSkinOralCoatF0(kEnamelIor), kCavityOcclusion);
        OloSurfaceLighting base = OloSurfaceLighting(vec3(0.31, 0.22, 0.18), vec3(0.09, 0.09, 0.09));
        OloSurfaceLighting wet = oloSkinOralApplyCoat(base, N, V, L, vec3(2.0), fullLane);
        result = vec4(min(min(wet.Diffuse.x, wet.Diffuse.y), wet.Diffuse.z), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 11) // CAVITY, ZERO IS EXACTLY ONE
    {
        result = vec4(oloSkinOralCavityWeight(kAo, 0.0), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 12) // CAVITY, ONE IS THE AO
    {
        result = vec4(oloSkinOralCavityWeight(kAo, 1.0), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 13) // CAVITY IS MONOTONE
    {
        float low = oloSkinOralCavityWeight(kAo, 0.25);
        float high = oloSkinOralCavityWeight(kAo, 0.75);
        result = vec4(low, high, 0.0, 1.0);
    }
    else if (caseIndex == 14) // LANE GATE, WRONG VERSION
    {
        vec4 stale = oloSkinEvaluatesOralSurface(OLO_MATERIAL_KIND_SKIN, OLO_SKIN_MODEL_LAYERED_SPECULAR)
                         ? kLane : vec4(0.0);
        vec4 notSkin = oloSkinEvaluatesOralSurface(OLO_MATERIAL_KIND_GENERIC, OLO_SKIN_MODEL_ORAL_SURFACE)
                           ? kLane : vec4(0.0);
        result = vec4(length(stale), length(notSkin), 0.0, 1.0);
    }
    else if (caseIndex == 15) // LANE GATE, RIGHT VERSION
    {
        vec4 live = oloSkinEvaluatesOralSurface(OLO_MATERIAL_KIND_SKIN, OLO_SKIN_MODEL_ORAL_SURFACE)
                        ? kLane : vec4(0.0);
        result = vec4(live.x, live.y, live.z, live.w);
    }

    o_Result = result;
}
