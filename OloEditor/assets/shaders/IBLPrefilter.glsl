// =============================================================================
// IBLPrefilter.glsl - Environment Map Prefiltering for IBL
// Part of OloEngine PBR System
// Generates prefiltered environment map for specular IBL
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(location = 0) out vec3 v_LocalPos;

void main()
{
    v_LocalPos = a_Position;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_LocalPos;
layout(location = 0) out vec4 o_Color;

layout(binding = 9) uniform samplerCube u_EnvironmentMap;

// UBO_USER_0 (binding 7) — must match ShaderBindingLayout::UBO_USER_0
// and IBLPrecompute.cpp GeneratePrefilterMap() which creates the UBO at this binding
layout(std140, binding = 7) uniform IBLParameters {
    float u_Roughness;
    float u_ExposureAdjustment;
    float u_IBLIntensity;
    float u_IBLRotation;
};

// Hammersley / ImportanceSampleGGX / bitwise radical inverse / branchless basis.
#include "include/MathCommon.glsl"

void main()
{
    vec3 N = normalize(v_LocalPos);
    vec3 R = N;
    vec3 V = R;

    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    vec3 prefilteredColor = vec3(0.0);

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleGGX(Xi, N, u_Roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            prefilteredColor += texture(u_EnvironmentMap, L).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = prefilteredColor / totalWeight;

    o_Color = vec4(prefilteredColor, 1.0);
}
