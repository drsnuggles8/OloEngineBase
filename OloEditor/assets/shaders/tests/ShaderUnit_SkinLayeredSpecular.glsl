// =============================================================================
// ShaderUnit_SkinLayeredSpecular.glsl
//
// Pins the contracts issue #1243 added to the shading language, by CALLING the
// production functions in include/SkinLayeredSpecular.glsl rather than
// transcribing them — the same discipline ShaderUnit_SkinTransmission.glsl and
// ShaderUnit_FoliageTransmission.glsl use, and for the same reason: a
// transcription tests the copy.
//
// The other callers of these exact functions are PBR_MultiLight{,_Skinned}.glsl
// and include/ForwardPlusCommon.glsl (forward and clustered),
// PBR_GBuffer{,_Skinned}.glsl (the filtered roughness written into RT1) and
// include/DeferredLightingShared.glsl (deferred). If this probe agrees with the
// CPU, all six do, because there is one implementation.
//
// WHAT THIS ADDS OVER SkinLayeredSpecularTest. That test pins the CPU maths and
// its convexity argument; this one pins that the GLSL is the SAME maths.
// Renderer/SkinLayeredSpecular.h calls itself "the specification", and a
// specification nothing is compared against is a comment.
//
// ONE COLUMN PER NAMED CLAIM. Each texel's x coordinate selects a case; the CPU
// side asserts the claim the case is named for. The probe is 1 pixel tall.
//
//   0  FILTER PARITY        — oloSkinFilteredAlpha against the CPU's
//                             SkinFilteredAlpha for one oblique, non-degenerate
//                             derivative pair. The EQUALITY case: every other
//                             filter case below is a property, and a property
//                             can hold while the number is wrong by a factor of
//                             a thousand.
//   1  FILTER NEVER SHARPENS— the filtered alpha is >= the input alpha. The
//                             other direction is a sharpening filter, and a
//                             sharpening filter manufactures aliasing rather
//                             than removing it.
//   2  ZERO STRENGTH IS EXACT— a variance strength of 0 returns the roughness
//                             UNCHANGED, bit for bit. This is the A/B control
//                             arm the sparkle evidence is measured against, and
//                             "almost unchanged" would make the control a third
//                             variant rather than a baseline.
//   3  THE KERNEL IS CLAMPED— an absurd derivative pair stops at the published
//                             clamp instead of running to fully rough. Without
//                             it every silhouette in the scene grows a bright
//                             halo, which is the failure mode that looks like a
//                             bloom bug.
//   4  MIX IS CONVEX        — the mixture lies BETWEEN its two lobes for a
//                             mid-range weight. Rules out the additive spelling,
//                             which is the one that quietly brightens every head.
//   5  ZERO MIX IS EXACT    — LobeMix 0 returns the narrow lobe bit for bit, so
//                             "layering off" is the pre-#1243 frame and a golden
//                             image can say so.
//   6  FULL MIX IS THE BROAD LOBE — the other endpoint, which catches a swapped
//                             pair of arguments that case 5 alone would pass.
//   7  BROAD IS NEVER NARROWER — oloSkinBroadRoughness only ever widens, and
//                             clamps at 1 rather than leaving the NDF's domain.
//   8  LAYERED CLOSURE, DIFFUSE UNTOUCHED — the layered closure's DIFFUSE half
//                             equals the unlayered closure's. This is the first
//                             acceptance criterion as a number: the second lobe
//                             must not erase the diffusion underneath, and the
//                             diffusion is fed from this exact value.
//   9  LAYERED CLOSURE, SPECULAR MOVES — and the specular half does change, so
//                             case 8 is not passing because the whole thing is
//                             inert.
//  10  DETAIL, ZERO IS IDENTITY — a detail strength of 0 returns the fine
//                             tangent normal untouched. The neutral default, and
//                             what makes the feature cost nothing until asked.
//  11  DETAIL, MINUS ONE IS THE COARSE NORMAL — the exact "detail off" arm the
//                             fourth acceptance criterion's AOV comparison is
//                             built on. Not "smoother" — the coarse normal.
//  12  DETAIL, POSITIVE DEEPENS — a positive strength pushes the normal FURTHER
//                             from the coarse one than the fine one is, which is
//                             what "deepen the pores" means as an inequality.
//  13  LOBE GATE, WRONG VERSION — oloSkinLobeFor returns the neutral (0, 1) for
//                             a profile below transport version 3, so a stale
//                             deferred slot loses the effect rather than
//                             acquiring someone else's lobe.
//  14  LOBE GATE, RIGHT VERSION — and returns the lane's xy for a version-3 skin
//                             pixel, so case 13 is not passing by being broken.
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
// testing the upload, and a failure could not tell a bad lobe from a bad bind.
const float kAlpha = 0.16;          // GGX alpha == roughness^2 for roughness 0.4
const float kRoughness = 0.4;
const float kVarianceStrength = 0.5;

// An OBLIQUE, non-degenerate derivative pair. Axis-aligned or equal-length
// derivatives would pass with one of the two dot products dropped entirely.
const vec3 kdNdx = vec3(0.12, -0.05, 0.03);
const vec3 kdNdy = vec3(-0.04, 0.09, -0.07);

const float kLobeMix = 0.35;
const float kLobeRoughnessScale = 2.0;

const vec3 kAlbedo = vec3(0.62, 0.48, 0.42);
const float kMetallic = 0.0;

// Two tangent-space normals a couple of mips apart: the fine one tilted, the
// coarse one nearer flat, as a mip chain would produce over a pore.
const vec3 kFineTangent = vec3(0.30, -0.18, 0.9366);
const vec3 kCoarseTangent = vec3(0.08, -0.05, 0.9955);

// The lane a version-3 profile packs, mirroring SkinSpecularLane.
const vec4 kLane = vec4(kLobeMix, kLobeRoughnessScale, kVarianceStrength, 0.0);

void main()
{
    int caseIndex = int(gl_FragCoord.x);
    vec4 result = vec4(0.0);

    // Shared shading configuration for the closure cases. Oblique on purpose,
    // for the reason the derivatives are.
    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 V = normalize(vec3(0.15, -0.10, 1.0));
    vec3 L = normalize(vec3(0.45, 0.30, 0.84));

    if (caseIndex == 0) // FILTER PARITY — the equality case
    {
        float filtered = oloSkinFilteredAlpha(kAlpha, kdNdx, kdNdy, kVarianceStrength);
        result = vec4(filtered, 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 1) // FILTER NEVER SHARPENS
    {
        float filtered = oloSkinFilteredAlpha(kAlpha, kdNdx, kdNdy, kVarianceStrength);
        // red = the filtered alpha, green = the input it must not go below.
        result = vec4(filtered, kAlpha, 0.0, 1.0);
    }
    else if (caseIndex == 2) // ZERO STRENGTH IS EXACT
    {
        float filtered = oloSkinFilteredAlpha(kAlpha, kdNdx, kdNdy, 0.0);
        result = vec4(filtered, kAlpha, 0.0, 1.0);
    }
    else if (caseIndex == 3) // THE KERNEL IS CLAMPED
    {
        float filtered = oloSkinFilteredAlpha(kAlpha, vec3(1.0e3), vec3(1.0e3), 4.0);
        float ceiling = sqrt(kAlpha * kAlpha + OLO_SKIN_VARIANCE_KERNEL_CLAMP);
        result = vec4(filtered, ceiling, 0.0, 1.0);
    }
    else if (caseIndex == 4) // MIX IS CONVEX
    {
        vec3 narrow = vec3(2.0, 3.0, 4.0);
        vec3 broad = vec3(8.0, 1.0, 4.0);
        result = vec4(oloSkinSpecularMix(narrow, broad, 0.25), 1.0);
    }
    else if (caseIndex == 5) // ZERO MIX IS EXACT
    {
        vec3 narrow = vec3(2.0, 3.0, 4.0);
        vec3 broad = vec3(8.0, 1.0, 4.0);
        vec3 mixed = oloSkinSpecularMix(narrow, broad, 0.0);
        // red = the mixed red, green = the narrow red it must equal exactly.
        result = vec4(mixed.r, narrow.r, mixed.g - narrow.g, 1.0);
    }
    else if (caseIndex == 6) // FULL MIX IS THE BROAD LOBE
    {
        vec3 narrow = vec3(2.0, 3.0, 4.0);
        vec3 broad = vec3(8.0, 1.0, 4.0);
        vec3 mixed = oloSkinSpecularMix(narrow, broad, 1.0);
        result = vec4(mixed.r, broad.r, 0.0, 1.0);
    }
    else if (caseIndex == 7) // BROAD IS NEVER NARROWER, AND STAYS IN DOMAIN
    {
        float broad = oloSkinBroadRoughness(kRoughness, kLobeRoughnessScale);
        float saturated = oloSkinBroadRoughness(0.8, 4.0);
        // red = the broad roughness, green = the narrow one it must exceed,
        // blue = a case that would leave the domain without the clamp.
        result = vec4(broad, kRoughness, saturated, 1.0);
    }
    else if (caseIndex == 8) // LAYERED CLOSURE, DIFFUSE UNTOUCHED
    {
        OloSurfaceLighting plain =
            evaluatePBRClosureSplit(OLO_PBR_MODEL_LEGACY, N, V, L, kAlbedo, kMetallic, kRoughness);
        OloSurfaceLighting layered =
            oloSkinLayeredClosureSplit(OLO_PBR_MODEL_LEGACY, N, V, L, kAlbedo, kMetallic, kRoughness,
                                       vec2(kLobeMix, kLobeRoughnessScale));
        // red = the layered diffuse, green = the unlayered one it must equal.
        result = vec4(layered.Diffuse.r, plain.Diffuse.r, 0.0, 1.0);
    }
    else if (caseIndex == 9) // LAYERED CLOSURE, SPECULAR MOVES
    {
        OloSurfaceLighting plain =
            evaluatePBRClosureSplit(OLO_PBR_MODEL_LEGACY, N, V, L, kAlbedo, kMetallic, kRoughness);
        OloSurfaceLighting layered =
            oloSkinLayeredClosureSplit(OLO_PBR_MODEL_LEGACY, N, V, L, kAlbedo, kMetallic, kRoughness,
                                       vec2(kLobeMix, kLobeRoughnessScale));
        // red = the layered specular, green = the unlayered one, blue = the
        // BROAD lobe alone so the CPU can check the mixture really lies between.
        OloSurfaceLighting broadOnly =
            evaluatePBRClosureSplit(OLO_PBR_MODEL_LEGACY, N, V, L, kAlbedo, kMetallic,
                                    oloSkinBroadRoughness(kRoughness, kLobeRoughnessScale));
        result = vec4(layered.Specular.r, plain.Specular.r, broadOnly.Specular.r, 1.0);
    }
    else if (caseIndex == 10) // DETAIL, ZERO IS IDENTITY
    {
        vec3 detailed = oloSkinDetailTangentNormal(kFineTangent, kCoarseTangent, 0.0);
        result = vec4(detailed - kFineTangent, 1.0);
    }
    else if (caseIndex == 11) // DETAIL, MINUS ONE IS THE COARSE NORMAL
    {
        vec3 detailed = oloSkinDetailTangentNormal(kFineTangent, kCoarseTangent, -1.0);
        // The coarse normal is unit length already, so the difference must be
        // zero in every channel — not merely "closer to coarse than fine".
        result = vec4(detailed - normalize(kCoarseTangent), 1.0);
    }
    else if (caseIndex == 12) // DETAIL, POSITIVE DEEPENS
    {
        vec3 detailed = oloSkinDetailTangentNormal(kFineTangent, kCoarseTangent, 1.0);
        vec3 coarse = normalize(kCoarseTangent);
        // red = how far the deepened normal is from the coarse one, green = how
        // far the fine one is. red must exceed green.
        result = vec4(length(detailed - coarse), length(kFineTangent - coarse), 0.0, 1.0);
    }
    else if (caseIndex == 13) // LOBE GATE, WRONG VERSION
    {
        vec2 lobe = oloSkinLobeFor(OLO_MATERIAL_KIND_SKIN, OLO_SKIN_MODEL_THICKNESS_TRANSMISSION, kLane);
        vec2 notSkin = oloSkinLobeFor(OLO_MATERIAL_KIND_GENERIC, OLO_SKIN_MODEL_LAYERED_SPECULAR, kLane);
        result = vec4(lobe, notSkin);
    }
    else if (caseIndex == 14) // LOBE GATE, RIGHT VERSION
    {
        vec2 lobe = oloSkinLobeFor(OLO_MATERIAL_KIND_SKIN, OLO_SKIN_MODEL_LAYERED_SPECULAR, kLane);
        result = vec4(lobe, 0.0, 1.0);
    }

    o_Result = result;
}
