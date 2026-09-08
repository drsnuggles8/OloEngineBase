// =============================================================================
// PbrClosureV2SampleProbe.glsl
//
// Draws PBRCommon.glsl's `closureV2SampleBRDF` — the v2 closure's SAMPLE, the
// third of the Evaluate / Sample / Pdf triple — over a deterministic parameter
// grid, so the C++ twin (PBRClosureBSDF.h's BSDF::Sample with Model ==
// ClosureV2) can be compared against the REAL COMPILED SHADER texel for texel
// (issue #1055).
//
// PbrClosureV2ParityProbe.glsl pins Evaluate and Pdf; until #1055 the sampler
// was compile-covered only, and PBRCommon.glsl's own note asked the first
// consumer to "extend the parity probe with a sampled-tuple channel". This is
// that channel, as its own probe so a failure names the function that drifted.
// The GPU path tracer draws every scatter direction through this function; an
// unpinned sampler is exactly the wrong-Sample-twin failure that converges
// beautifully to the wrong image.
//
// Like the other probes this computes NO estimator and asserts no invariant.
// It is a pure function dump; the C++ side reproduces the grid and diffs.
//
// Parameterisation (pixel-centre sampling, so no sample lands exactly on a
// degenerate endpoint):
//   uv.x -> roughness in (0, 1)
//   uv.y -> a packed (lobe / shape sample, metallic) pair — the y axis is
//           split into METALLIC_STEPS bands, each sweeping the lobe-selection
//           scalar across the band. The 2D shape sample is derived from BOTH
//           axes so every texel draws a distinct (lobeXi, Xi) triple.
// Fixed:  N = (0, 0, 1), V = normalize(0.4, 0.3, 1) — an OFF-AXIS view on
//         purpose: with N == V the VNDF sampler's stretch is the identity and a
//         transposed basis would be invisible. albedo = (0.9, 0.6, 0.3), non-grey
//         so the lobe probability's per-channel luminance weighting is exercised.
//
// Two draws, selected by the DRAW define the harness sets:
//   Output 0: .rgb = sampled direction L, .a = the mixture Pdf
//   Output 1: .rgb = the sampled Value (closureV2Evaluate at L), .a = Pdf again
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

layout(location = 0) out vec4 o_Direction;
layout(location = 1) out vec4 o_Value;

layout(location = 0) in vec2 v_TexCoord;

#include "include/PBRCommon.glsl"

// Must match kMetallicSteps in ClosureV2SampleGpuParityTest.cpp.
const int METALLIC_STEPS = 4;

void main()
{
    float roughness = clamp(v_TexCoord.x, 0.0, 1.0);

    // Decode the packed y axis: which metallic band, and where inside it.
    float scaled = clamp(v_TexCoord.y, 0.0, 1.0) * float(METALLIC_STEPS);
    float band = min(floor(scaled), float(METALLIC_STEPS - 1));
    float withinBand = scaled - band;

    float metallic = band / float(METALLIC_STEPS - 1);

    // The lobe-selection scalar sweeps the band; the 2D shape sample is a
    // deterministic function of both axes so neighbouring texels differ in
    // every draw. Kept strictly inside (0, 1): the endpoints are where the
    // samplers' sqrt / acos edges live, and they are not what parity is about.
    float lobeXi = withinBand;
    vec2 Xi = vec2(fract(v_TexCoord.x * 7.0 + v_TexCoord.y * 3.0) * 0.98 + 0.01,
                   fract(v_TexCoord.y * 11.0 + v_TexCoord.x * 5.0) * 0.98 + 0.01);

    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 V = normalize(vec3(0.4, 0.3, 1.0));
    vec3 albedo = vec3(0.9, 0.6, 0.3);

    ClosureV2Sample s = closureV2SampleBRDF(N, V, albedo, metallic, roughness, lobeXi, Xi);

    o_Direction = vec4(s.L, s.Pdf);
    o_Value = vec4(s.Value, s.Pdf);
}
