// =============================================================================
// ShaderUnit_FoliageTransmission.glsl
//
// Pins the two contracts issue #1234 added to the shading language, by CALLING
// the production functions in include/FoliageSurface.glsl rather than
// transcribing them — the same discipline ShaderUnit_GBufferFlagsLane.glsl uses
// for the flags lane, and for the same reason: a transcription tests the copy.
//
// The other callers of these exact functions are Foliage_Instance.glsl,
// Foliage_Impostor.glsl (forward) and DeferredLightingShared.glsl (deferred).
// If this probe agrees with the CPU expectations, all three do, because there
// is one implementation.
//
// ONE COLUMN PER NAMED CLAIM. Each texel's x coordinate selects a case; the
// CPU side asserts the claim the case is named for. A row index is unused, so
// the probe is 1 pixel tall and every row would repeat — the harness draws it
// at height 1.
//
//   0  BACKLIT, LIT       — a backlit leaf transmits: the direct lobe is > 0.
//   1  BACKLIT, SHADOWED  — the SAME geometry with shadow 0 transmits NOTHING.
//                           This is #1234's second acceptance criterion stated
//                           as a number: transmission is not an unshadowed
//                           constant, so removing the light removes the term.
//   2  AMBIENT HALF       — the environment half is > 0 and is NOT a function
//                           of `shadow` (it takes no shadow argument at all;
//                           the claim here is that it is driven by the supplied
//                           irradiance, so zero irradiance gives zero).
//   3  FRONT-LIT          — the same leaf lit from the FRONT transmits far less
//                           than the backlit case. Forward scattering is what
//                           this lobe models; a term that was equally bright
//                           front and back would be an ambient fill wearing a
//                           lobe's name.
//   4  ZERO THICKNESS     — thickness 0 transmits nothing, in both halves. This
//                           is the "layer is not a leaf material" path and the
//                           pre-#1234 behaviour.
//   5  FACE NORMAL, FRONT — oloFoliageFaceNormal(N, V) with V on the +N side
//                           returns N unchanged: dot(result, N) == +1.
//   6  FACE NORMAL, BACK  — with V on the -N side it returns -N:
//                           dot(result, N) == -1, and dot(result, V) > 0. The
//                           second half is the actual contract — "faces the
//                           viewer" — and the first is how it achieves it.
//   7  SHADOW NORMAL      — oloFoliageShadowNormal(N, L) for a BACKLIT leaf
//                           (dot(N, L) < 0) returns the LIT-SIDE normal:
//                           dot(result, L) > 0. This is the one that decides
//                           whether a backlit leaf shadow-acnes itself to black.
//   8  SHADOW NORMAL, LIT — with dot(N, L) > 0 it returns N UNCHANGED, which is
//                           what makes one shadow lookup serve both lobes
//                           instead of the reflected lobe silently changing its
//                           bias on foliage.
//
// Output channel usage (per case, meanings differ by column — the CPU side
// names them):
//   .r = the primary quantity the claim is about
//   .g = a secondary quantity (a comparison arm, or dot(result, V))
//   .b = a third quantity where the claim needs one
//   .a = unused, written 0 so no channel is undefined
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// The LOBE half only — no OLO_FOLIAGE_SURFACE_SAMPLING, because the sampling
// half needs the foliage UBO and the leaf samplers, which this probe has
// neither of and does not need: it is testing the maths, not the plumbing.
#include "include/FoliageSurface.glsl"

float luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    int caseIndex = int(gl_FragCoord.x);

    // A leaf facing the camera, the sun directly behind it. The canonical
    // backlit configuration this material exists for.
    vec3 N = vec3(0.0, 0.0, 1.0);  // shading normal, towards the eye
    vec3 V = vec3(0.0, 0.0, 1.0);  // surface -> eye
    vec3 Lback = vec3(0.0, 0.0, -1.0); // surface -> light, BEHIND the leaf
    vec3 Lfront = vec3(0.0, 0.0, 1.0); // surface -> light, in front

    vec3 radiance = vec3(1.0);
    vec3 tint = vec3(1.0);
    float thickness = 0.75;
    // distortion, power, wrap, environment scale
    vec4 lobe = vec4(0.35, 4.0, 0.5, 0.5);

    vec4 result = vec4(0.0);

    if (caseIndex == 0)
    {
        result.r = luma(oloFoliageTransmissionDirect(N, V, Lback, radiance, 1.0, thickness, tint, lobe));
    }
    else if (caseIndex == 1)
    {
        result.r = luma(oloFoliageTransmissionDirect(N, V, Lback, radiance, 0.0, thickness, tint, lobe));
        // The lit arm beside it, so the CPU can assert the pair rather than a
        // bare zero that a broken probe would also produce.
        result.g = luma(oloFoliageTransmissionDirect(N, V, Lback, radiance, 1.0, thickness, tint, lobe));
    }
    else if (caseIndex == 2)
    {
        result.r = luma(oloFoliageTransmissionAmbient(thickness, tint, vec3(1.0), lobe));
        result.g = luma(oloFoliageTransmissionAmbient(thickness, tint, vec3(0.0), lobe));
    }
    else if (caseIndex == 3)
    {
        result.r = luma(oloFoliageTransmissionDirect(N, V, Lfront, radiance, 1.0, thickness, tint, lobe));
        result.g = luma(oloFoliageTransmissionDirect(N, V, Lback, radiance, 1.0, thickness, tint, lobe));
    }
    else if (caseIndex == 4)
    {
        result.r = luma(oloFoliageTransmissionDirect(N, V, Lback, radiance, 1.0, 0.0, tint, lobe));
        result.g = luma(oloFoliageTransmissionAmbient(0.0, tint, vec3(1.0), lobe));
    }
    else if (caseIndex == 5)
    {
        vec3 n = oloFoliageFaceNormal(N, V);
        result.r = dot(n, N);
        result.g = dot(n, V);
    }
    else if (caseIndex == 6)
    {
        // The viewer is on the OTHER side of the lamina.
        vec3 vBack = vec3(0.0, 0.0, -1.0);
        vec3 n = oloFoliageFaceNormal(N, vBack);
        result.r = dot(n, N);
        result.g = dot(n, vBack);
    }
    else if (caseIndex == 7)
    {
        vec3 n = oloFoliageShadowNormal(N, Lback);
        result.r = dot(n, Lback);
        result.g = dot(n, N);
    }
    else if (caseIndex == 8)
    {
        vec3 n = oloFoliageShadowNormal(N, Lfront);
        result.r = dot(n, Lfront);
        result.g = dot(n, N);
    }

    o_Color = result;
}
