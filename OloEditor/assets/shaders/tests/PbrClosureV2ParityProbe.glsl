// =============================================================================
// PbrClosureV2ParityProbe.glsl
//
// Evaluates PBRCommon.glsl's `closureV2Evaluate` and `closureV2Pdf` — the v2
// closure's Evaluate and density, exactly as the driver compiles them — over a
// deterministic parameter grid, so the C++ twins
// (OloEngine/Renderer/PathTracing/ReferenceBRDF.h's ClosureV2Evaluate and
// PBRClosureBSDF.h's BSDF::Pdf with Model == ClosureV2) can be compared
// against the REAL COMPILED SHADER texel for texel (issue #975).
//
// This is the v2 sibling of PbrBrdfParityProbe.glsl and the anti-drift guard
// for the versioned closure contract. The v2 promise — one D serving Evaluate,
// Sample and Pdf, mirrored function-for-function in C++ — only holds while the
// two languages agree; if the PBR CLOSURE V2 section is edited and the C++
// twins are not, the reference tracer silently integrates a closure the raster
// path no longer shades. ClosureV2GpuParityTest fails the moment that happens.
// Unlike the Legacy probe, this one can pin the DENSITY too: Legacy GLSL has
// no PDF, but closureV2Pdf exists precisely so a GPU sampler and the CPU
// reference draw from the same mixture.
//
// Like the Legacy probe this computes NO estimator and asserts no invariant.
// It is a pure function dump: the C++ side reproduces the same grid and diffs.
// Keep it that way — any cleverness added here has to be mirrored exactly on
// the C++ side, which defeats the purpose.
//
// Parameterisation (pixel-centre sampling, so no sample lands exactly on a
// degenerate endpoint):
//   uv.x -> roughness in (0, 1)
//   uv.y -> a packed (NdotL angle, metallic) pair — the y axis is split into
//           kMetallicSteps horizontal bands, each sweeping the light's polar
//           angle across the band. This gets three parameters onto a 2D target
//           without a second draw, and the C++ side decodes it identically.
// Fixed:  N = (0, 0, 1), V = (0, 0, 1), albedo = (0.9, 0.6, 0.3)
//
// With N == V both closureV2Evaluate and closureV2Pdf are azimuthally
// symmetric — only the angle between L and N matters — so a 2D grid covers
// the domain and a varying azimuth would add nothing but C++/GLSL trig drift.
// A non-grey albedo is deliberate: it makes a channel swap or a luminance
// collapse in either implementation visible, which a white albedo would hide
// (and it exercises closureV2SpecularProbability's per-channel luminance
// weighting inside the Pdf).
//
// Output: .rgb = closureV2Evaluate(...)
//         .a   = closureV2Pdf(...)
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

#include "include/PBRCommon.glsl"

// Must match kMetallicSteps in ClosureV2GpuParityTest.cpp.
const int METALLIC_STEPS = 4;

void main()
{
    float roughness = clamp(v_TexCoord.x, 0.0, 1.0);

    // Decode the packed y axis: which metallic band, and where inside it.
    float scaled = clamp(v_TexCoord.y, 0.0, 1.0) * float(METALLIC_STEPS);
    float band = min(floor(scaled), float(METALLIC_STEPS - 1));
    float withinBand = scaled - band;

    float metallic = band / float(METALLIC_STEPS - 1);

    // Light polar angle sweeps (0, pi/2): cosTheta in (0, 1).
    float theta = withinBand * HALF_PI;
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);

    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 V = vec3(0.0, 0.0, 1.0);
    // Azimuth fixed at 0 — with N == V the closure is azimuthally symmetric,
    // so a varying azimuth would add nothing but a source of C++/GLSL drift.
    vec3 L = vec3(sinTheta, 0.0, cosTheta);

    vec3 albedo = vec3(0.9, 0.6, 0.3);

    o_Color = vec4(closureV2Evaluate(N, V, L, albedo, metallic, roughness),
                   closureV2Pdf(N, V, L, albedo, metallic, roughness));
}
