// =============================================================================
// DeferredLighting.glsl - Deferred lighting composition pass (Phase 3, full)
// Part of OloEngine Deferred Renderer
//
// Reads the 4-RT G-Buffer produced by PBR_GBuffer{,_Skinned}.glsl and
// evaluates opaque PBR lighting for every visible fragment. Output is
// linear HDR RGBA16F ready for post-processing.
//
// Expected texture bindings (match ShaderBindingLayout TEX_GBUFFER_*):
//   slot 43 — RT0  RGBA8   albedo.rgb + metallic
//   slot 44 — RT1  RGBA16F octNormal.xy + roughness + AO
//   slot 45 — RT2  RGBA16F emissive.rgb + material flags
//   slot 46 — RT3  RG16F   screen-space velocity (unused in lighting)
//   slot 47 — depth (D32F)
//
// The per-pixel shading body lives in include/DeferredLightingShared.glsl
// so the MSAA variant (DeferredLighting_MSAA.glsl) can re-use the exact same
// math on per-sample inputs without code drift.
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

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// MultiLight UBO (binding 5)
layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int u_MaxLights;
    int u_ShadowCasterCount;
    int u_DirectionalLightCount;
    LightData u_Lights[MAX_LIGHTS];
};

// Shadow UBO (binding 6)
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

// MotionBlur UBO (binding 8) for u_InverseViewProjection.
layout(std140, binding = 8) uniform MotionBlurMatrices {
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

// Per-pass deferred controls UBO (binding 30).
layout(std140, binding = 30) uniform DeferredLightingControls {
    vec4 u_DeferredControls; // x=EnableIBL, y=EnableLightProbes, z=IBLIntensity, w=CascadeDebug
    vec4 u_MSAAParams;       // x=SampleCount (float, >=1), yzw reserved
};

// IBL cubemaps.
layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D   u_BRDFLutMap;

// Shadow maps — identical slots to PBR_MultiLight.
layout(binding = 8)  uniform sampler2DArrayShadow u_ShadowMapCSM;
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowMapSpot;
layout(binding = 14) uniform samplerCubeShadow   u_ShadowMapPoint0;
layout(binding = 15) uniform samplerCubeShadow   u_ShadowMapPoint1;
layout(binding = 16) uniform samplerCubeShadow   u_ShadowMapPoint2;
layout(binding = 17) uniform samplerCubeShadow   u_ShadowMapPoint3;

// G-Buffer samplers (non-MSAA variant).
layout(binding = 43) uniform sampler2D u_GBufferAlbedo;
layout(binding = 44) uniform sampler2D u_GBufferNormal;
layout(binding = 45) uniform sampler2D u_GBufferEmissive;
layout(binding = 46) uniform sampler2D u_GBufferVelocity;
layout(binding = 47) uniform sampler2D u_GBufferDepth;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

#include "include/DeferredLightingShared.glsl"

void main()
{
    float depth = texture(u_GBufferDepth, v_TexCoord).r;
    if (depth >= 0.999999)
    {
        o_Color = vec4(texture(u_GBufferEmissive, v_TexCoord).rgb, 1.0);
        return;
    }

    vec4 gAlbedo    = texture(u_GBufferAlbedo,   v_TexCoord);
    vec4 gNormal    = texture(u_GBufferNormal,   v_TexCoord);
    vec4 gEmissive  = texture(u_GBufferEmissive, v_TexCoord);

    vec3 albedo    = gAlbedo.rgb;
    float metallic = gAlbedo.a;
    vec3 N         = OctDecodeGB(gNormal.xy);
    float roughness = max(gNormal.z, MIN_ROUGHNESS);
    float ao       = gNormal.w;
    // Pass emissive RGB + flag alpha through; ComputeDeferredLit handles the
    // unlit fast-path when emissive.a > 0.5.
    vec4 emissive  = gEmissive;

    vec3 worldPos = ReconstructWorldPosGB(v_TexCoord, depth);
    vec3 color = ComputeDeferredLit(albedo, metallic, N, roughness, ao, emissive, worldPos);

    o_Color = vec4(color, 1.0);
}
