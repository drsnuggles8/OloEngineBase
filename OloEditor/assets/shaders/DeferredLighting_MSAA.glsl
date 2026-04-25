// =============================================================================
// DeferredLighting_MSAA.glsl — per-sample deferred lighting composition.
//
// Selected by DeferredLightingPass when GBuffer::GetSampleCount() > 1 AND
// DeferredSettings::PerSampleLighting is true. Samples each G-Buffer
// attachment with sampler2DMS / texelFetch per sub-sample, evaluates full
// PBR lighting per sample, and averages the final HDR colour. This avoids
// the shading-rate collapse of a resolve-before-light approach where MSAA
// would only affect geometric edge samples of the depth/normal during
// G-Buffer write but not the shading itself.
//
// Shares the per-pixel shading body with the non-MSAA variant via
// include/DeferredLightingShared.glsl so there is a single source of truth
// for the PBR math.
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

#include "include/PBRCommon.glsl"
#include "include/LightProbeSampling.glsl"
#include "include/ForwardPlusCommon.glsl"

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int u_MaxLights;
    int u_ShadowCasterCount;
    int u_DirectionalLightCount;
    LightData u_Lights[MAX_LIGHTS];
};

layout(std140, binding = 6) uniform ShadowData {
    mat4 u_DirectionalLightSpaceMatrices[4];
    vec4 u_CascadePlaneDistances;
    vec4 u_ShadowParams;
    mat4 u_SpotLightSpaceMatrices[4];
    vec4 u_PointLightShadowParams[4];
    int u_DirectionalShadowEnabled;
    int u_SpotShadowCount;
    int u_PointShadowCount;
    int u_ShadowMapResolution;
    int u_CascadeDebugEnabled;
    int _shadowPad0;
    int _shadowPad1;
    int _shadowPad2;
};

layout(std140, binding = 8) uniform MotionBlurMatrices {
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

// Per-pass deferred controls UBO (binding 30) — same layout as non-MSAA
// variant so a single C++ upload works for both shader variants.
layout(std140, binding = 30) uniform DeferredLightingControls {
    vec4 u_DeferredControls; // x=EnableIBL, y=EnableLightProbes, z=IBLIntensity, w=CascadeDebug
    vec4 u_MSAAParams;       // x=SampleCount (float, >=1), yzw reserved
};

layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D   u_BRDFLutMap;

layout(binding = 8)  uniform sampler2DArrayShadow u_ShadowMapCSM;
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowMapSpot;
layout(binding = 14) uniform samplerCubeShadow   u_ShadowMapPoint0;
layout(binding = 15) uniform samplerCubeShadow   u_ShadowMapPoint1;
layout(binding = 16) uniform samplerCubeShadow   u_ShadowMapPoint2;
layout(binding = 17) uniform samplerCubeShadow   u_ShadowMapPoint3;

// G-Buffer samplers — MSAA variant uses sampler2DMS (no implicit filtering;
// must use texelFetch per-sample).
layout(binding = 43) uniform sampler2DMS u_GBufferAlbedo;
layout(binding = 44) uniform sampler2DMS u_GBufferNormal;
layout(binding = 45) uniform sampler2DMS u_GBufferEmissive;
layout(binding = 46) uniform sampler2DMS u_GBufferVelocity;
layout(binding = 47) uniform sampler2DMS u_GBufferDepth;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

#include "include/DeferredLightingShared.glsl"

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int sampleCount = max(int(u_MSAAParams.x + 0.5), 1);

    // Fast-path: if every sample reports far-plane depth (no geometry
    // written this pixel), this is pure skybox — just average the skybox
    // samples written during forward overlay / pre-lit emissive.
    bool anyGeom = false;
    for (int s = 0; s < sampleCount; ++s)
    {
        if (texelFetch(u_GBufferDepth, pixel, s).r < 0.999999)
        {
            anyGeom = true;
            break;
        }
    }
    if (!anyGeom)
    {
        vec3 emissiveSum = vec3(0.0);
        for (int s = 0; s < sampleCount; ++s)
            emissiveSum += texelFetch(u_GBufferEmissive, pixel, s).rgb;
        o_Color = vec4(emissiveSum / float(sampleCount), 1.0);
        return;
    }

    // Shade every sample and average. Background (depth==1.0) samples
    // contribute their raw emissive so silhouette pixels where some
    // samples fell outside geometry still anti-alias correctly against
    // the sky / background emissive.
    vec3 accum = vec3(0.0);
    for (int s = 0; s < sampleCount; ++s)
    {
        float depth = texelFetch(u_GBufferDepth, pixel, s).r;
        vec3 emissiveSample = texelFetch(u_GBufferEmissive, pixel, s).rgb;
        if (depth >= 0.999999)
        {
            accum += emissiveSample;
            continue;
        }

        vec4 gAlbedo = texelFetch(u_GBufferAlbedo, pixel, s);
        vec4 gNormal = texelFetch(u_GBufferNormal, pixel, s);
        // Re-fetch emissive with alpha so the unlit-flag path can trigger
        // per-sample. (We sampled RGB above for the skybox fast-path but
        // need .a for PBR-vs-unlit selection inside ComputeDeferredLit.)
        vec4 emissiveFlags = texelFetch(u_GBufferEmissive, pixel, s);

        vec3 albedo     = gAlbedo.rgb;
        float metallic  = gAlbedo.a;
        vec3 N          = OctDecodeGB(gNormal.xy);
        float roughness = max(gNormal.z, MIN_ROUGHNESS);
        float ao        = gNormal.w;

        // Reconstruct world position using per-sample depth + pixel-center
        // UV. Sub-pixel positional jitter is absorbed into the shading —
        // per-sample depth is what disambiguates near-silhouette samples.
        vec3 worldPos = ReconstructWorldPosGB(v_TexCoord, depth);

        accum += ComputeDeferredLit(albedo, metallic, N, roughness, ao, emissiveFlags, worldPos);
    }

    vec3 color = accum / float(sampleCount);
    o_Color = vec4(color, 1.0);
}
