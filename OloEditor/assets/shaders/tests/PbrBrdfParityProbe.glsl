// =============================================================================
// PbrBrdfParityProbe.glsl
//
// Evaluates PBRCommon.glsl's `cookTorranceBRDF` — the exact function every lit
// pass calls — over a deterministic parameter grid, so the C++ mirror of it
// (OloEngine/Renderer/PathTracing/ReferenceBRDF.h) can be compared against the
// REAL COMPILED SHADER texel for texel (issue #709).
//
// This is the anti-drift guard for the offline reference path tracer. The
// tracer's whole claim — "a divergence between raster and reference IS the
// bug" — only holds while the two BRDFs agree; if PBRCommon's BRDF is edited
// and the C++ port is not, the reference silently starts blessing whatever the
// old formula said. ReferenceBRDFGpuParityTest fails the moment that happens.
//
// Unlike the other probes here this one computes NO estimator and asserts no
// invariant. It is a pure function dump: the C++ side reproduces the same grid
// and diffs. Keep it that way — any cleverness added here has to be mirrored
// exactly on the C++ side, which defeats the purpose.
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
// A non-grey albedo is deliberate: it makes a channel swap or a luminance
// collapse in either implementation visible, which a white albedo would hide.
//
// Output: .rgb = cookTorranceBRDF(...)
//         .a   = visibilitySmithGGXCorrelated(...)
//
// The alpha channel rides along for issue #904. The height-correlated
// visibility term is not on the shipping lit path, but it IS a PBRCommon
// function with a C++ mirror, and an unmirrored-or-unpinned function is one
// nothing can detect drift in. Reusing this probe's grid rather than adding a
// second one keeps both sides decoding identical parameters by construction.
// Note N == V here, so NdotV is pinned at 1 and only the NdotL half of the
// term's (NdotV, NdotL) symmetry is swept — which is enough to expose an
// alpha-convention error, since V varies strongly with alpha at low NdotL.
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

// Must match kMetallicSteps in ReferenceBRDFGpuParityTest.cpp.
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
    // Azimuth fixed at 0 — with N == V the BRDF is azimuthally symmetric, so a
    // varying azimuth would add nothing but a source of C++/GLSL trig drift.
    vec3 L = vec3(sinTheta, 0.0, cosTheta);

    vec3 albedo = vec3(0.9, 0.6, 0.3);

    o_Color = vec4(cookTorranceBRDF(N, V, L, albedo, metallic, roughness),
                   visibilitySmithGGXCorrelated(N, V, L, roughness));
}
