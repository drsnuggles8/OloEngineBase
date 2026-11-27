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

    o_Color = vec4(closureV2Evaluate(N, V, L, albedo, metallic, roughness),
                   visibilitySmithGGXCorrelated(N, V, L, roughness));
}
