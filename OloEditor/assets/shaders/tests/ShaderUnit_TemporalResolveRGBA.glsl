#version 460 core

// =============================================================================
// ShaderUnit_TemporalResolveRGBA.glsl
//
// L2 probe for the RGBA half of include/TemporalResolve.glsl (issue #903) —
// the entry point the cloudscape resolve adopted, whose alpha carries
// TRANSMITTANCE and therefore gets its own 1-D box instead of a slot in the
// YCoCg one.
//
// The probe exists because that half's risk is not in the math, it is in the
// GATHER: OLO_TEMPORAL_GATHER_3X3_RGBA fills two statistics structs from one
// nine-tap loop, and the way that goes wrong is a copy-paste — alpha's moments
// written from the colour accumulator, or vice versa — which every CPU mirror
// of the *functions* would still pass.
//
// So each invocation reads the centre texel of a 3x3 RGBA signal and emits
// three vec4s:
//
//   [3i + 0] the RGBA resolve
//   [3i + 1] the RGB-ONLY resolve, via the original OLO_TEMPORAL_GATHER_3X3.
//            The rgb halves of these two must agree exactly, which is what
//            pins that the new macro did not disturb the colour path.
//   [3i + 2] the raw alpha statistics (mean, stddev, min, max), so the test can
//            check them against hand-computed values without duplicating a
//            mirror of the kernel.
// =============================================================================

layout(local_size_x = 1, local_size_y = 1) in;

layout(binding = 0) uniform sampler2D u_Signal;

#include "../include/TemporalResolve.glsl"

// Two vec4s per case: the history RGBA, then (gamma, feedback, confidence, _).
layout(std430, binding = 2) readonly buffer Inputs
{
    vec4 u_Inputs[];
};

layout(std430, binding = 1) writeonly buffer Outputs
{
    vec4 u_Outputs[];
};

void main()
{
    uint caseIndex = gl_GlobalInvocationID.x;
    vec4 history = u_Inputs[2u * caseIndex];
    vec4 params = u_Inputs[2u * caseIndex + 1u];
    float gamma = params.x;
    float feedback = params.y;
    float confidence = params.z;

    // The signal is exactly 3x3, so the centre of the texture is the centre of
    // texel (1,1) and the eight +/-1 texel offsets land on the other eight
    // centres. Nothing samples outside, so the wrap mode cannot matter.
    vec2 texel = 1.0 / vec2(textureSize(u_Signal, 0));
    vec2 uv = vec2(0.5);
    vec4 current = texture(u_Signal, uv);

    OloTemporalStats stats;
    OloTemporalScalarStats alphaStats;
    OLO_TEMPORAL_GATHER_3X3_RGBA(u_Signal, uv, texel, stats, alphaStats);
    u_Outputs[3u * caseIndex] =
        OloTemporalResolveRGBA(current, history, stats, alphaStats, gamma, feedback, confidence);

    OloTemporalStats rgbOnlyStats;
    OLO_TEMPORAL_GATHER_3X3(u_Signal, uv, texel, rgbOnlyStats);
    u_Outputs[3u * caseIndex + 1u] =
        vec4(OloTemporalResolve(current.rgb, history.rgb, rgbOnlyStats, gamma, feedback, confidence), 0.0);

    u_Outputs[3u * caseIndex + 2u] =
        vec4(alphaStats.Mean, alphaStats.StdDev, alphaStats.MinC, alphaStats.MaxC);
}
