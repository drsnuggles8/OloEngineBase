// =============================================================================
// IrradianceConvolutionAdvanced.glsl - Quality-configurable diffuse irradiance
// Part of OloEngine PBR System
//
// The "advanced" diffuse irradiance generator selected by
// IBLPrecompute::GenerateIrradianceMapAdvanced (the production default when
// IBLConfiguration::UseSphericalHarmonics is false). Where the baseline
// IrradianceConvolution.glsl walks a fixed sampleDelta=0.025 hemisphere grid,
// this path uses cosine-weighted importance sampling with a configurable,
// quality-scaled sample count and mip-biased environment lookups. That removes
// the baseline's banding at low resolutions and lets Ultra converge with fewer
// directions than the ~10k the fixed grid implies.
//
// Normalisation matches the baseline exactly: the output is the *normalised*
// irradiance E(N)/PI, which evaluates to 1.0 for uniform-white input (pinned by
// PbrIrradianceTest.UniformWhiteYieldsNormalisedUnity). With cosine-importance
// sampling the Monte-Carlo estimator of E(N)/PI collapses to the plain sample
// mean (1/N) * Sum L_i, since the cos(theta)/PI weight cancels the pdf.
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

// Hammersley / bitwise radical inverse / branchless OrthonormalBasis.
#include "include/MathCommon.glsl"

// Cosine-weighted hemisphere sample around N. pdf = cos(theta) / PI.
vec3 ImportanceSampleCosine(vec2 Xi, vec3 N, out float cosTheta)
{
    float phi = 2.0 * PI * Xi.x;
    cosTheta = sqrt(1.0 - Xi.y); // Malley's method: project a disk sample up
    float sinTheta = sqrt(Xi.y);

    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    vec3 tangent, bitangent;
    OrthonormalBasis(N, tangent, bitangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

void main()
{
    vec3 N = normalize(v_LocalPos);

    // Quality-driven sample count, clamped to a safe window.
    int effective = int(clamp(float(u_SampleCount) * max(u_QualityMultiplier, 0.0625), 64.0, 8192.0));
    uint SAMPLE_COUNT = uint(effective);

    // Solid angle of a source texel, for mip-biased lookups that pre-average
    // bright pixels and cut the diffuse noise floor.
    float resolution = float(max(u_SourceResolution, 1));
    float saTexel = 4.0 * PI / (6.0 * resolution * resolution);

    vec3 irradiance = vec3(0.0);

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        float cosTheta;
        vec3 L = ImportanceSampleCosine(Xi, N, cosTheta);

        float pdf = max(cosTheta / PI, 1e-4);
        float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 1e-4);
        float mipLevel = max(0.5 * log2(saSample / saTexel), 0.0);

        // cos(theta)/pdf == PI, and the E/PI normalisation divides it back out,
        // so each direction contributes its radiance unweighted: mean of L.
        irradiance += textureLod(u_EnvironmentMap, L, mipLevel).rgb;
    }

    irradiance /= float(SAMPLE_COUNT);

    o_Color = vec4(irradiance, 1.0);
}
