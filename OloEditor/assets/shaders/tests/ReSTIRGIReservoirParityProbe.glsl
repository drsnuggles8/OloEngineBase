// =============================================================================
// ReSTIRGIReservoirParityProbe.glsl — the GI reservoir's PACKING, driven through
// a real RGBA32F render target. Issue #1169. Test-only; never loaded by a
// production path.
//
// WHY A PROBE SHADER AND NOT A HEADLESS COMPARISON. #1140's failure was not an
// arithmetic disagreement between the C++ and the GLSL — both sides were right.
// It was the STORE: `kind | index << 3` reinterpreted as a float is a DENORMAL,
// the GPU flushed it to zero, every reservoir read back as kind None, the
// resolve wrote black, and every counter still said the tier was active. No
// headless test could see that, and neither could a same-invocation round-trip,
// because the value has to LEAVE the shader and come back.
//
// WHAT IS GI-SPECIFIC HERE, and why this file exists alongside the DI probe: the
// ENCODING is shared (include/ReservoirCore.glsl) and DI's probe already covers
// it. What GI adds is the identity lane's PAYLOAD — an AGE rather than a light
// index — and the kind range it shares that lane with. An age that saturated
// wrongly, or a kind that decoded out of range, would make the OLDEST samples
// look the freshest, which is the one failure the age cap exists to prevent.
//
// Output: .r = OloPackReservoirIdentity(kind, age)
//         .g = OloPackReservoirNormal(normal)
//         .b = the normal's polar angle that produced .g, so a failure names the
//              direction rather than only the encoded number
//         .a = float(OLO_GI_RESERVOIR_LAYOUT_VERSION), which pins the GLSL
//              constant against the C++ one THROUGH the same lane the reservoir
//              planes use
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

#include "include/ReservoirGI.glsl"

// Must match kAgeSteps / kKindSteps in ReSTIRGIReservoirGpuParityTest.cpp.
const int AGE_STEPS = 256;
const int KIND_STEPS = 3;

void main()
{
    // The age, spread over the WHOLE cap rather than 0..255: the interesting end
    // is the top, where `age << 3` is largest and where a saturation that was
    // written as a wrap would show.
    float ageT = clamp(v_TexCoord.x, 0.0, 1.0);
    uint age = uint(min(floor(ageT * float(AGE_STEPS)), float(AGE_STEPS - 1))) *
               (OLO_GI_MAX_SAMPLE_AGE / uint(AGE_STEPS - 1));

    // Decode the packed y axis: which kind band, and where inside it.
    float scaled = clamp(v_TexCoord.y, 0.0, 1.0) * float(KIND_STEPS);
    float band = min(floor(scaled), float(KIND_STEPS - 1));
    float withinBand = scaled - band;
    uint kind = uint(band);

    // A full sphere sweep, so the octahedral z < 0 fold is exercised. Azimuth is
    // tied to the polar angle rather than held fixed, so the grid does not walk
    // one meridian and miss a swapped x/y.
    float theta = withinBand * 3.14159265358979;
    float phi = withinBand * 6.28318530717959 * 3.0;
    vec3 normal = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));

    o_Color = vec4(OloPackReservoirIdentity(kind, age), OloPackReservoirNormal(normal), theta,
                   float(OLO_GI_RESERVOIR_LAYOUT_VERSION));
}
