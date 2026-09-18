// =============================================================================
// ShaderUnit_SkinTransmission.glsl
//
// Pins the contracts issue #1242 added to the shading language, by CALLING the
// production functions in include/SkinTransmission.glsl rather than
// transcribing them — the same discipline ShaderUnit_FoliageTransmission.glsl
// and ShaderUnit_GBufferFlagsLane.glsl use, and for the same reason: a
// transcription tests the copy.
//
// The other callers of these exact functions are PBR_MultiLight.glsl,
// PBR_MultiLight_Skinned.glsl (forward), PBR_GBuffer{,_Skinned}.glsl (the
// thickness lane) and DeferredLightingShared.glsl (deferred). If this probe
// agrees with the CPU, all five do, because there is one implementation.
//
// WHAT THIS ADDS OVER SkinTransmissionTest. That test pins the CPU maths; this
// one pins that the GLSL is the SAME maths. Between them, a unit slip or a
// reassociation in the shader shows up as a number rather than as a screenshot
// somebody has to interpret — which matters here because the CPU side is
// explicitly called "the specification" and a specification nothing is compared
// against is a comment.
//
// ONE COLUMN PER NAMED CLAIM. Each texel's x coordinate selects a case; the CPU
// side asserts the claim the case is named for. The probe is 1 pixel tall.
//
//   0  BACKLIT, LIT        — a backlit thin region transmits: the term is > 0.
//   1  BACKLIT, SHADOWED   — the SAME geometry with visibility 0 transmits
//                            NOTHING. #1242's second criterion as a number: the
//                            term is gated by the shared visibility factor, so
//                            an occluded ear stops glowing.
//   2  FRONT-LIT           — the same surface lit from the FRONT transmits
//                            EXACTLY zero. Not "less" — zero, because that
//                            exactness is premise 1 of the energy argument and
//                            the thing a wrap term would quietly break.
//   3  TANGENT             — dot(N, L) == 0 transmits exactly zero as well. The
//                            ray of measure zero between the two hemispheres; a
//                            term that fired here would rim every silhouette.
//   4  ZERO THICKNESS      — thickness 0 transmits nothing. The conservative
//                            fallback, and NOT the other reading of a zero
//                            thickness (exp(0) = 1, fully transparent), which is
//                            the uniformly emissive head.
//   5  THICKER TRANSMITS LESS — the same geometry at 4x the thickness is
//                            strictly dimmer. Catches a sign slip in the
//                            exponent, which would make the THICKEST parts glow.
//   6  RED OUTLIVES BLUE   — red's channel exceeds blue's through one thickness.
//                            Carried entirely by the authored per-channel mean
//                            free paths; a grey glow is wax, not skin.
//   7  SHADOW NORMAL, BACKLIT — oloSkinShadowNormal(N, L) with dot(N, L) < 0
//                            returns the LIT-SIDE normal: dot(result, L) > 0.
//                            This is the one that decides whether a backlit ear
//                            shadow-acnes itself to black.
//   8  SHADOW NORMAL, LIT  — with dot(N, L) > 0 it returns N UNCHANGED, which is
//                            what lets ONE shadow lookup serve both lobes
//                            instead of the reflected lobe silently changing its
//                            bias on skin.
//   9  RT5 LANE ROUND-TRIP — oloSkinPackGBufferThickness then
//                            oloSkinUnpackGBufferThickness returns the thickness
//                            it was handed, in MILLIMETRES. The deferred path's
//                            whole per-pixel thickness depends on this pair, and
//                            a range or clamp slip here is invisible in a frame.
//  10  RT5 LANE DEFERS TO THE LIGHTMAP — with coverage 1 the packer returns the
//                            IRRADIANCE untouched and the unpacker reads no
//                            thickness. The priority that keeps a lightmapped
//                            surface's indirect light.
//  11  CPU PARITY           — the exact term the CPU's EvaluateSkinTransmissionLanes
//                            computes for the same inputs, returned raw so the
//                            CPU can compare it against its own answer. This is
//                            the transcription test proper; every case above is
//                            a property, this one is an equality.
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

#include "include/SkinTransmission.glsl"

layout(location = 0) out vec4 o_Result;

// THE FIXTURE, spelled here and mirrored on the CPU side. Deliberately NOT
// derived from a uniform: a probe that took its inputs from a UBO would also be
// testing the upload, and a failure could not tell a bad lobe from a bad bind.
//
// The lanes are what SkinTransmissionScatterLane / SkinTransmissionScalingLane
// produce for a profile with ScatterColor (0.90, 0.55, 0.40), Strength 1.0,
// Anisotropy 0.7, Power 4.0 and a Burley scaling of (2.0, 1.1, 0.8) mm. The CPU
// test constructs that profile and asserts the lanes match these literals
// before it compares anything, so the two cannot drift apart silently.
const vec4 kScatter = vec4(0.90, 0.55, 0.40, 0.7);
const vec4 kScaling = vec4(2.0, 1.1, 0.8, 4.0);

const vec3 kNormal  = vec3(0.0, 0.0, 1.0);
const vec3 kView    = vec3(0.0, 0.0, 1.0);
// `L` points FROM the surface TOWARD the light, so a light BEHIND the surface
// points away from the eye.
const vec3 kLightBehind  = vec3(0.0, 0.0, -1.0);
const vec3 kLightInFront = vec3(0.0, 0.0,  1.0);
const vec3 kLightTangent = vec3(1.0, 0.0,  0.0);

const vec3 kAlbedo   = vec3(0.62, 0.48, 0.42);
const vec3 kRadiance = vec3(3.0, 3.0, 3.0);
const float kThicknessMM = 2.0;

void main()
{
    int caseIndex = int(gl_FragCoord.x);
    vec4 result = vec4(0.0);

    if (caseIndex == 0) // BACKLIT, LIT
    {
        vec3 t = oloSkinTransmissionDirect(kNormal, kView, kLightBehind, kRadiance, 1.0,
                                           kAlbedo, kThicknessMM, kScatter, kScaling);
        result = vec4(t, 1.0);
    }
    else if (caseIndex == 1) // BACKLIT, SHADOWED — red must be 0, green is the lit control
    {
        vec3 shadowed = oloSkinTransmissionDirect(kNormal, kView, kLightBehind, kRadiance, 0.0,
                                                  kAlbedo, kThicknessMM, kScatter, kScaling);
        vec3 lit = oloSkinTransmissionDirect(kNormal, kView, kLightBehind, kRadiance, 1.0,
                                             kAlbedo, kThicknessMM, kScatter, kScaling);
        // The lit arm rides in GREEN so a failure can say "and the lit arm
        // measured X" rather than leaving the reader unable to tell a working
        // gate from a dead lobe.
        result = vec4(shadowed.r, lit.r, 0.0, 1.0);
    }
    else if (caseIndex == 2) // FRONT-LIT — exactly zero
    {
        vec3 t = oloSkinTransmissionDirect(kNormal, kView, kLightInFront, kRadiance, 1.0,
                                           kAlbedo, kThicknessMM, kScatter, kScaling);
        result = vec4(t, 1.0);
    }
    else if (caseIndex == 3) // TANGENT — exactly zero
    {
        vec3 t = oloSkinTransmissionDirect(kNormal, kView, kLightTangent, kRadiance, 1.0,
                                           kAlbedo, kThicknessMM, kScatter, kScaling);
        result = vec4(t, 1.0);
    }
    else if (caseIndex == 4) // ZERO THICKNESS — nothing, not everything
    {
        vec3 t = oloSkinTransmissionDirect(kNormal, kView, kLightBehind, kRadiance, 1.0,
                                           kAlbedo, 0.0, kScatter, kScaling);
        result = vec4(t, 1.0);
    }
    else if (caseIndex == 5) // THICKER TRANSMITS LESS — thin in red, thick in green
    {
        vec3 thin = oloSkinTransmissionDirect(kNormal, kView, kLightBehind, kRadiance, 1.0,
                                              kAlbedo, kThicknessMM, kScatter, kScaling);
        vec3 thick = oloSkinTransmissionDirect(kNormal, kView, kLightBehind, kRadiance, 1.0,
                                               kAlbedo, kThicknessMM * 4.0, kScatter, kScaling);
        result = vec4(thin.r, thick.r, 0.0, 1.0);
    }
    else if (caseIndex == 6) // RED OUTLIVES BLUE — the transmittance itself, unlit by geometry
    {
        vec3 transmittance = oloSkinTransmittance(kThicknessMM, kScatter, kScaling);
        result = vec4(transmittance, 1.0);
    }
    else if (caseIndex == 7) // SHADOW NORMAL, BACKLIT — must point at the light
    {
        vec3 shadowN = oloSkinShadowNormal(kNormal, kLightBehind);
        result = vec4(dot(shadowN, kLightBehind), dot(shadowN, kNormal), 0.0, 1.0);
    }
    else if (caseIndex == 8) // SHADOW NORMAL, LIT — must be N unchanged
    {
        vec3 shadowN = oloSkinShadowNormal(kNormal, kLightInFront);
        result = vec4(dot(shadowN, kNormal), dot(shadowN, kLightInFront), 0.0, 1.0);
    }
    else if (caseIndex == 9) // RT5 LANE ROUND-TRIP, no lightmap
    {
        vec4 packed = oloSkinPackGBufferThickness(vec4(0.0), true, kThicknessMM);
        float unpacked = oloSkinUnpackGBufferThickness(packed, true);
        // red = what came back, green = the coverage the packer wrote (must be
        // 0, or the ambient ladder would read this pixel as lightmapped).
        result = vec4(unpacked, packed.a, 0.0, 1.0);
    }
    else if (caseIndex == 10) // RT5 LANE DEFERS TO THE LIGHTMAP
    {
        vec4 irradiance = vec4(0.25, 0.5, 0.75, 1.0);
        vec4 packed = oloSkinPackGBufferThickness(irradiance, true, kThicknessMM);
        float unpacked = oloSkinUnpackGBufferThickness(packed, true);
        // red = the thickness a reader would get (must be 0), green/blue/alpha =
        // the irradiance, which must be byte-for-byte what went in.
        result = vec4(unpacked, packed.r, packed.g, packed.a);
    }
    else if (caseIndex == 11) // CPU PARITY — an oblique, partly shadowed configuration
    {
        // DELIBERATELY NOT AXIS-ALIGNED and DELIBERATELY NOT FULLY VISIBLE. An
        // axis-aligned case makes the lobe's `pow` exactly 1 or 0 and would pass
        // with the anisotropy term missing entirely; a visibility of 1 would
        // pass with the multiply dropped.
        vec3 lightDir = normalize(vec3(0.35, 0.20, -0.85));
        vec3 view = normalize(vec3(0.10, -0.15, 1.0));
        vec3 t = oloSkinTransmissionDirect(kNormal, view, lightDir, kRadiance, 0.6,
                                           kAlbedo, kThicknessMM, kScatter, kScaling);
        result = vec4(t, 1.0);
    }

    o_Result = result;
}
