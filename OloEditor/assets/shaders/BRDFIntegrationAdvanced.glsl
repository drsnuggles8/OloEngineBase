// =============================================================================
// BRDFIntegrationAdvanced.glsl - Quality-configurable BRDF LUT generation
// Part of OloEngine PBR System
//
// The "advanced" split-sum BRDF integrator selected by
// IBLPrecompute::GenerateBRDFLutAdvanced. It is identical in form to the
// baseline BRDFLutGeneration.glsl but takes its Monte-Carlo sample count from
// the IBLAdvancedParams UBO (256/512/1024/2048 across the quality presets)
// instead of hard-coding 1024, so Ultra spends more samples to drive down the
// residual noise in the scale/bias terms while Low stays cheap.
//
// Output is the standard RG split-sum pair (scale, bias) consumed by the PBR
// shaders' `F0 * brdf.x + brdf.y` term; the LUT texture is RG32F.
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

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

// IBLAdvancedParamsUBO at UBO_USER_0 (binding 7). Only u_SampleCount is used
// here; the remaining members are shared with the prefilter/irradiance paths.
layout(std140, binding = 7) uniform IBLAdvancedParams {
    float u_Roughness;
    float u_QualityMultiplier;
    int u_SampleCount;
    int u_UseImportanceSampling;
    int u_SourceResolution;
};

// Hammersley / ImportanceSampleGGX / bitwise radical inverse / branchless basis.
#include "include/MathCommon.glsl"

// Smith geometry term with Schlick-GGX, using the IBL remap k = a^2 / 2.
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec2 IntegrateBRDF(float NdotV, float roughness, uint sampleCount)
{
    vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);

    float A = 0.0;
    float B = 0.0;

    vec3 N = vec3(0.0, 0.0, 1.0);

    for (uint i = 0u; i < sampleCount; ++i)
    {
        vec2 Xi = Hammersley(i, sampleCount);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0)
        {
            float G = GeometrySmith(N, V, L, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV + 1e-7);
            float Fc = Pow5(1.0 - VdotH);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    return vec2(A, B) / float(sampleCount);
}

void main()
{
    // Quality-driven sample count, clamped to a safe window.
    uint sampleCount = uint(clamp(float(u_SampleCount), 64.0, 4096.0));

    vec2 integratedBRDF = IntegrateBRDF(v_TexCoord.x, v_TexCoord.y, sampleCount);
    o_Color = vec4(integratedBRDF, 0.0, 1.0);
}
