// =============================================================================
// IBLPrefilterImportance.glsl - Quality-configurable specular IBL prefilter
// Part of OloEngine PBR System
//
// The "advanced" prefilter selected by IBLPrecompute::GeneratePrefilterMapAdvanced
// when IBLConfiguration::UseImportanceSampling is enabled. Over the baseline
// IBLPrefilter.glsl it adds:
//   * a configurable, quality-scaled sample count (driven by a UBO, not a
//     fixed 1024) so Low/Medium/High/Ultra trade speed for noise;
//   * Karis/Krivanek mip-biased environment sampling: each GGX sample reads a
//     source mip chosen from its solid angle vs the source texel solid angle,
//     which suppresses the firefly sparkles bright HDR pixels otherwise leave
//     in rough reflections;
//   * a roughness == 0 fast path that copies the mirror reflection directly.
// Requires the source environment cubemap to carry a mip chain for the bias to
// have anything to read; with a single-level source textureLod() simply clamps
// to mip 0 (same result as the baseline, just without the firefly reduction).
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

// IBLAdvancedParamsUBO at UBO_USER_0 (binding 7). See ShaderBindingLayout.h.
layout(std140, binding = 7) uniform IBLAdvancedParams {
    float u_Roughness;
    float u_QualityMultiplier;
    int u_SampleCount;
    int u_UseImportanceSampling;
    int u_SourceResolution;
};

// Hammersley / ImportanceSampleGGX / bitwise radical inverse / branchless basis.
#include "include/MathCommon.glsl"

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom + 1e-7);
}

void main()
{
    vec3 N = normalize(v_LocalPos);
    // Epic's split-sum assumption: the view and reflection directions equal the
    // normal, which lets the lobe be precomputed independent of view angle.
    vec3 R = N;
    vec3 V = R;

    // Mirror reflection: the GGX lobe collapses to a single direction, so skip
    // the (degenerate) importance loop and copy the environment exactly.
    if (u_Roughness <= 0.0)
    {
        o_Color = vec4(texture(u_EnvironmentMap, N).rgb, 1.0);
        return;
    }

    // Effective sample count: configured count scaled by quality, clamped to a
    // safe window so a stale/garbage UBO value can never spin the GPU.
    int effective = int(clamp(float(u_SampleCount) * max(u_QualityMultiplier, 0.0625), 32.0, 4096.0));
    uint SAMPLE_COUNT = uint(effective);

    // Solid angle of a single source-cubemap texel, for the mip-bias below.
    float resolution = float(max(u_SourceResolution, 1));
    float saTexel = 4.0 * PI / (6.0 * resolution * resolution);

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
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);

            // PDF of this sample in solid-angle measure: D * NdotH / (4 * HdotV).
            float D = DistributionGGX(NdotH, u_Roughness);
            float pdf = (D * NdotH / (4.0 * HdotV)) + 1e-4;

            // Map the sample's solid angle to a source mip. Lower-probability
            // (wide) samples read coarser mips, pre-averaging the radiance that
            // a single texel would otherwise alias into a firefly.
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 1e-4);
            float mipLevel = max(0.5 * log2(saSample / saTexel), 0.0);

            prefilteredColor += textureLod(u_EnvironmentMap, L, mipLevel).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = (totalWeight > 0.0) ? (prefilteredColor / totalWeight)
                                           : texture(u_EnvironmentMap, N).rgb;

    o_Color = vec4(prefilteredColor, 1.0);
}
