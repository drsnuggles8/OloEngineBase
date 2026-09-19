// =============================================================================
// ShaderUnit_SkinOcularSurface.glsl
//
// Pins the contracts issue #1244 added to the shading language, by CALLING the
// production functions in include/SkinOcularSurface.glsl rather than
// transcribing them — the same discipline ShaderUnit_SkinOralSurface.glsl uses,
// and for the same reason: a transcription tests the copy.
//
// The other callers of these exact functions are PBR_MultiLight{,_Skinned}.glsl
// and PBR_GBuffer{,_Skinned}.glsl. If this probe agrees with the CPU, all four
// do, because there is one implementation — and note that the list is four
// MATERIAL stages rather than three lighting paths, which is the structural
// point of this feature.
//
// ONE COLUMN PER NAMED CLAIM. Each texel's x coordinate selects a case; the CPU
// side asserts the claim the case is named for. The probe is 1 pixel tall.
//
//   0  REFRACT PARITY      — oloSkinOcularRefract against SkinOcularRefract for
//                            one oblique configuration. The EQUALITY case:
//                            every other case is a property, and a property can
//                            hold while the ray bends by the wrong angle.
//   1  REFRACT BENDS TOWARD THE NORMAL — entering a denser medium, the
//                            refracted ray is CLOSER to the surface normal than
//                            the incident one. The sign of the whole feature,
//                            and the one thing an inverted eta gets wrong while
//                            still producing a plausible direction.
//   2  REFRACT REPORTS TIR — an eta above 1 with an oblique incidence returns
//                            false rather than a zeroed direction. A zero
//                            direction marches nowhere and lands on the entry
//                            point, which shades as a painted iris — the silent
//                            failure the whole file is written against.
//   3  CORNEAL NORMAL PARITY — oloSkinCornealNormal against SkinCornealNormal
//                            at an oblique point. The bend that recovers a fifth
//                            of the error a sphere mesh would otherwise carry.
//   4  CORNEAL NORMAL, RATIO ONE IS EXACT — a ratio of 1 returns the input bit
//                            for bit. How a mesh with real corneal geometry
//                            opts out, and it is only worth opting out if it is
//                            exact.
//   5  CORNEAL NORMAL IS STEEPER — the bent normal is FURTHER from the optical
//                            axis than the globe normal it came from, so case 3
//                            is not passing by the bend being inert.
//   6  IRIS PLANE HIT PARITY — oloSkinIrisPlaneHit against SkinIrisPlaneHit.
//                            The march, in eye radii.
//   7  IRIS PLANE, OUTWARD RAY REFUSED — a ray travelling away from the iris
//                            returns false rather than solving for a negative t
//                            and placing the iris in front of the eye.
//   8  LIMBAL RING PARITY  — oloSkinIrisLimbalRing against the CPU at a radial
//                            inside the band.
//   9  LIMBAL RING, ZERO IS EXACTLY ONE — a strength of 0 returns 1.0 bit for
//                            bit. The A/B control arm.
//  10  PUPIL PARITY        — oloSkinIrisPupilMask against the CPU just inside
//                            the pupil's soft edge, where a hard step and a
//                            soft one disagree most.
//  11  PUPIL CENTRE IS BLACK — and the iris edge is untouched, in one texel, so
//                            case 10 cannot pass on a mask that is constant.
//  12  DISH TILT VANISHES AT THE EDGE — oloSkinIrisShadingNormal returns the
//                            input at the iris edge. The continuity that stops
//                            a hard ring appearing at the limbus.
//  13  THE WHOLE APPLY, PARITY — oloSkinOcularApply's albedo and iris radial
//                            against ApplySkinOcularSurface for a full
//                            version-5 profile at an oblique view. The
//                            end-to-end case, which is the one that fails if any
//                            of the twelve lane components is in the wrong slot.
//  14  ZERO MASTER IS EXACT — oloSkinOcularApply with OcularStrength 0 returns
//                            its albedo UNCHANGED, bit for bit, and a NEGATIVE
//                            iris radial. The neutral-identity arm every
//                            evidence A/B is measured against.
//  15  SCLERA IS UNTOUCHED — a normal outside the limbus returns the input
//                            albedo and a negative radial, so an eye's white
//                            stays a skin term.
//  16  REFRACTION MOVES THE IRIS — the same pixel with RefractionStrength 1 and
//                            0 lands at DIFFERENT iris radials, and the
//                            refracted one is nearer the axis (the corneal
//                            magnification). The feature, in one texel, at the
//                            only kind of view where it is visible.
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
// testing the upload, and a failure could not tell a bad eye from a bad bind.
//
// These are the lane values the SHIPPED default profile packs with
// OcularStrength 1 and a limbal ring authored — see
// TheProbeFixtureMatchesTheProfileItClaimsToBe on the CPU side, which pins them
// to a real profile so the mirror cannot drift silently.
const float kEta = 1.0 / 1.336;          // SkinCorneaEta(1.336)
const float kCurvatureRatio = 12.0 / 7.8;
const float kIrisPlaneDepth = 2.48 / 12.0;
const float kLimbusCos = 0.8731230;      // sqrt(1 - (5.85/12)^2)

const float kIrisRadius = 5.85 / 12.0;
const float kPupilRadial = 2.0 / 5.85;
const float kRingWidth = 0.7 / 5.85;

const float kRingStrength = 0.7;
const float kPupilDarkening = 1.0;
const float kConcavity = 0.3;
// A mid blue-grey iris, and the derived edge band beside it.
const vec3 kIrisColor = vec3(0.32, 0.46, 0.55);
const float kIrisEdgeBand = 0.7 / 5.85;

// An oblique surface point: 18 degrees off the optical axis, comfortably inside
// the limbus (29.2 degrees) and nowhere near the apex, where the bend and the
// refraction both vanish by symmetry and every case would pass with terms
// dropped.
const vec3 kAxis = vec3(0.0, 0.0, 1.0);
const vec3 kObliqueN = vec3(0.309017, 0.0, 0.951057);

void main()
{
    int caseIndex = int(gl_FragCoord.x);
    vec4 result = vec4(0.0);

    // The view, deliberately NOT along the normal and NOT along the axis: an
    // axis-aligned frame passes with the tangential term dropped.
    vec3 V = normalize(vec3(0.12, -0.08, 1.0));

    vec4 corneaLane = vec4(kEta, kCurvatureRatio, kIrisPlaneDepth, kLimbusCos);
    vec4 irisLane = vec4(kIrisRadius, kPupilRadial, kRingWidth, 1.0);
    vec4 responseLane = vec4(kRingStrength, kPupilDarkening, kConcavity, 1.0);
    vec4 tintLane = vec4(kIrisColor, kIrisEdgeBand);

    if (caseIndex == 0) // REFRACT PARITY — the equality case
    {
        vec3 refracted;
        bool ok = oloSkinOcularRefract(-V, kObliqueN, kEta, refracted);
        result = vec4(refracted, ok ? 1.0 : 0.0);
    }
    else if (caseIndex == 1) // REFRACT BENDS TOWARD THE NORMAL
    {
        vec3 refracted;
        oloSkinOcularRefract(-V, kObliqueN, kEta, refracted);
        // Both cosines against the normal, measured on the INCOMING side so
        // they are directly comparable: a denser medium must increase it.
        float before = dot(normalize(V), kObliqueN);
        float after = dot(-refracted, kObliqueN);
        result = vec4(after, before, after > before ? 1.0 : 0.0, 1.0);
    }
    else if (caseIndex == 2) // REFRACT REPORTS TIR
    {
        vec3 refracted;
        // eta 2.0 at a 45-degree incidence: sin(theta_t) would exceed 1.
        bool ok = oloSkinOcularRefract(normalize(vec3(1.0, 0.0, -1.0)), kAxis, 2.0, refracted);
        result = vec4(ok ? 1.0 : 0.0, refracted.x, refracted.z, 1.0);
    }
    else if (caseIndex == 3) // CORNEAL NORMAL PARITY
    {
        result = vec4(oloSkinCornealNormal(kObliqueN, kAxis, kCurvatureRatio), 1.0);
    }
    else if (caseIndex == 4) // CORNEAL NORMAL, RATIO ONE IS EXACT
    {
        vec3 same = oloSkinCornealNormal(kObliqueN, kAxis, 1.0);
        // The DIFFERENCE, so the CPU asserts against zero rather than against a
        // vector that might be right for the wrong reason.
        result = vec4(same - kObliqueN, 1.0);
    }
    else if (caseIndex == 5) // CORNEAL NORMAL IS STEEPER
    {
        vec3 bent = oloSkinCornealNormal(kObliqueN, kAxis, kCurvatureRatio);
        result = vec4(dot(bent, kAxis), dot(kObliqueN, kAxis), 0.0, 1.0);
    }
    else if (caseIndex == 6) // IRIS PLANE HIT PARITY
    {
        vec3 refracted;
        oloSkinOcularRefract(-V, oloSkinCornealNormal(kObliqueN, kAxis, kCurvatureRatio), kEta, refracted);
        vec3 hit;
        bool ok = oloSkinIrisPlaneHit(kObliqueN, kAxis, refracted, kIrisPlaneDepth, hit);
        result = vec4(hit, ok ? 1.0 : 0.0);
    }
    else if (caseIndex == 7) // IRIS PLANE, OUTWARD RAY REFUSED
    {
        vec3 hit;
        bool ok = oloSkinIrisPlaneHit(kObliqueN, kAxis, kAxis, kIrisPlaneDepth, hit);
        result = vec4(ok ? 1.0 : 0.0, hit.x, hit.z, 1.0);
    }
    else if (caseIndex == 8) // LIMBAL RING PARITY
    {
        result = vec4(oloSkinIrisLimbalRing(0.95, kRingWidth, kRingStrength), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 9) // LIMBAL RING, ZERO IS EXACTLY ONE
    {
        result = vec4(oloSkinIrisLimbalRing(0.95, kRingWidth, 0.0), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 10) // PUPIL PARITY
    {
        result = vec4(oloSkinIrisPupilMask(kPupilRadial * 0.98, kPupilRadial, kPupilDarkening), 0.0, 0.0, 1.0);
    }
    else if (caseIndex == 11) // PUPIL CENTRE IS BLACK, IRIS EDGE IS NOT
    {
        result = vec4(oloSkinIrisPupilMask(0.0, kPupilRadial, kPupilDarkening),
                      oloSkinIrisPupilMask(1.0, kPupilRadial, kPupilDarkening), 0.0, 1.0);
    }
    else if (caseIndex == 12) // DISH TILT VANISHES AT THE EDGE
    {
        vec3 atEdge = oloSkinIrisShadingNormal(kObliqueN, kAxis, vec3(kIrisRadius, 0.0, 0.8),
                                               kIrisRadius, kConcavity);
        // The DIFFERENCE again, so the assertion is against zero.
        result = vec4(atEdge - kObliqueN, 1.0);
    }
    else if (caseIndex == 13) // THE WHOLE APPLY, PARITY
    {
        OloSkinOcular o = oloSkinOcularApply(vec3(0.42, 0.33, 0.27), kObliqueN, V, kAxis,
                                             corneaLane, irisLane, responseLane, tintLane);
        result = vec4(o.Albedo, o.IrisRadial);
    }
    else if (caseIndex == 14) // ZERO MASTER IS EXACT
    {
        vec3 albedo = vec3(0.42, 0.33, 0.27);
        OloSkinOcular o = oloSkinOcularApply(albedo, kObliqueN, V, kAxis, corneaLane,
                                             vec4(kIrisRadius, kPupilRadial, kRingWidth, 0.0), responseLane, tintLane);
        // The albedo DIFFERENCE and the radial, so the CPU asserts an exact
        // zero rather than an approximate equality.
        result = vec4(o.Albedo - albedo, o.IrisRadial);
    }
    else if (caseIndex == 15) // SCLERA IS UNTOUCHED
    {
        vec3 albedo = vec3(0.42, 0.33, 0.27);
        // 60 degrees off the axis — well outside the 29.2-degree limbus.
        vec3 scleraN = vec3(0.8660254, 0.0, 0.5);
        OloSkinOcular o = oloSkinOcularApply(albedo, scleraN, V, kAxis, corneaLane, irisLane, responseLane, tintLane);
        result = vec4(o.Albedo - albedo, o.IrisRadial);
    }
    else if (caseIndex == 16) // REFRACTION MOVES THE IRIS
    {
        vec3 albedo = vec3(0.42, 0.33, 0.27);
        OloSkinOcular refractedArm =
            oloSkinOcularApply(albedo, kObliqueN, kAxis, kAxis, corneaLane, irisLane,
                               vec4(kRingStrength, kPupilDarkening, kConcavity, 1.0), tintLane);
        OloSkinOcular paintedArm =
            oloSkinOcularApply(albedo, kObliqueN, kAxis, kAxis, corneaLane, irisLane,
                               vec4(kRingStrength, kPupilDarkening, kConcavity, 0.0), tintLane);
        result = vec4(refractedArm.IrisRadial, paintedArm.IrisRadial,
                      paintedArm.IrisRadial - refractedArm.IrisRadial, 1.0);
    }

    o_Result = result;
}
