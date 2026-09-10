// =============================================================================
// ReSTIRReservoirParityProbe.glsl
//
// Dumps include/Reservoir.glsl's PACKING — OloPackReservoirIdentity and
// OloPackReservoirNormal, exactly as the driver compiles them — into an RGBA32F
// target, so ReSTIRDIReservoirGpuParityTest can compare it against the C++
// twins in Renderer/ReSTIR/ReservoirDI.h (issue #1140).
//
// WHY THIS HAS TO GO THROUGH A REAL RENDER TARGET
// -----------------------------------------------
// The failure this test exists for is not an arithmetic disagreement. Reservoir
// layout v1 stored each integer as a FLOAT BIT PATTERN, and a small integer
// reinterpreted as a float is a DENORMAL — `kind | index << 3` = 401 becomes
// 5.6e-43. GPUs are permitted to flush denormals to zero and this one does, so
// the value was destroyed by the STORE, not by the maths. Every reservoir read
// back as kind None, the resolve wrote black, and every counter still said the
// tier was active. Nothing in the log.
//
// A test that packed and unpacked inside one shader invocation would have
// passed that. So this probe WRITES the packed lanes and the C++ side reads
// them back off the texture: the comparison spans the storage round-trip, which
// is the only place the bug lived.
//
// Like the other parity probes here this computes NO estimator and asserts
// nothing. It is a pure function dump; the C++ side reproduces the same grid and
// diffs. Keep it that way — cleverness added here has to be mirrored exactly on
// the C++ side, which defeats the purpose.
//
// Parameterisation (pixel-centre sampling):
//   uv.x -> the light index, 0 .. INDEX_STEPS-1, spanning the encodable range
//           including its top end, where a bit-pattern encoding overflows
//   uv.y -> a packed (sample kind, normal direction) pair: the y axis splits
//           into KIND_STEPS horizontal bands, one per LightSampleKind, each
//           sweeping the emitter normal over the full sphere so both octahedral
//           hemispheres are covered — the z < 0 fold is a separate code path
//           and the one most likely to be transcribed wrongly.
//
// Output: .r = OloPackReservoirIdentity(kind, lightIndex)
//         .g = OloPackReservoirNormal(normal)
//         .b = the normal's polar angle that produced .g, so a failure names the
//              direction rather than only the encoded number
//         .a = float(OLO_RESERVOIR_LAYOUT_VERSION), which pins the GLSL
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

#include "include/Reservoir.glsl"

// Must match kIndexSteps / kKindSteps in ReSTIRDIReservoirGpuParityTest.cpp.
const int INDEX_STEPS = 256;
const int KIND_STEPS = 5;

void main()
{
    // The light index, spread over the encodable range rather than 0..255: the
    // interesting end is the top, where `lightIndex << 3` is largest and a
    // bit-pattern encoding stops being representable.
    float indexT = clamp(v_TexCoord.x, 0.0, 1.0);
    uint lightIndex = uint(min(floor(indexT * float(INDEX_STEPS)), float(INDEX_STEPS - 1))) * 8191u;

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

    o_Color = vec4(OloPackReservoirIdentity(kind, lightIndex), OloPackReservoirNormal(normal), theta,
                   float(OLO_RESERVOIR_LAYOUT_VERSION));
}
