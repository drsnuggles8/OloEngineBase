// =============================================================================
// BRDFLutGeneration.glsl - BRDF LUT Generation for IBL
// Part of OloEngine PBR System
// Generates BRDF lookup table for split-sum approximation
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 8 (ADR 0011 §5): V1 vertex pull. On the Vulkan route the
// pipeline has no vertex-input state, so attributes are READ from binding 57
// (the engine-wide vertex-pull binding; the root struct carries this buffer's
// device address). This pass draws MeshPrimitives::GetFullscreenTriangle(),
// a 20-byte {vec3 position @0, vec2 uv @12} interleave, so the stride is
// 5 floats. The GL attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
#endif

layout(location = 0) out vec2 v_TexCoord;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 5;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4]);
#endif
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

// Hammersley / ImportanceSampleGGX / bitwise radical inverse / branchless basis.
#include "include/MathCommon.glsl"

float geometrySchlickGGX(float NdotV, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0;

    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec2 IntegrateBRDF(float NdotV, float roughness)
{
    vec3 V;
    V.x = sqrt(1.0 - NdotV * NdotV);
    V.y = 0.0;
    V.z = NdotV;

    float A = 0.0;
    float B = 0.0;

    vec3 N = vec3(0.0, 0.0, 1.0);

    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0)
        {
            float G = geometrySmith(N, V, L, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV);
            float Fc = Pow5(1.0 - VdotH);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);

    return vec2(A, B);
}

void main()
{
    vec2 integratedBRDF = IntegrateBRDF(v_TexCoord.x, v_TexCoord.y);
    o_Color = vec4(integratedBRDF, 0.0, 1.0);
}
