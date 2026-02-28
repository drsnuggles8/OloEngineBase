// =============================================================================
// PBR_MultiLight.glsl - Physically Based Rendering Shader with Multi-Light Support
// Part of OloEngine Enhanced PBR System
// Supports metallic-roughness workflow (glTF 2.0 standard) with multiple lights
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Model UBO (binding 3)
layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

// Output to fragment shader
layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    v_WorldPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_Normal = mat3(u_Normal) * a_Normal;
    v_TexCoord = a_TexCoord;

    gl_Position = u_ViewProjection * vec4(v_WorldPos, 1.0);
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"
#include "include/SnowCommon.glsl"

// Camera UBO (binding 0) - for view position
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Multi-Light UBO (binding 5)
layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int u_MaxLights;
    int u_ShadowCasterCount;
    int _padding;
    LightData u_Lights[MAX_LIGHTS];
};

// PBR Material UBO (binding 2)
layout(std140, binding = 2) uniform PBRMaterialProperties {
    vec4 u_BaseColorFactor;     // Base color (albedo) with alpha
    vec4 u_EmissiveFactor;      // Emissive color
    float u_MetallicFactor;     // Metallic factor
    float u_RoughnessFactor;    // Roughness factor
    float u_NormalScale;        // Normal map scale
    float u_OcclusionStrength;  // AO strength
    int u_UseAlbedoMap;         // Use albedo texture
    int u_UseNormalMap;         // Use normal map
    int u_UseMetallicRoughnessMap; // Use metallic-roughness texture
    int u_UseAOMap;             // Use ambient occlusion map
    int u_UseEmissiveMap;       // Use emissive map
    int u_EnableIBL;            // Enable IBL
    int u_ApplyGammaCorrection; // Apply gamma correction in this pass
    int u_AlphaCutoff;          // Alpha cutoff for transparency
};

// Snow UBO (binding 13)
layout(std140, binding = 13) uniform SnowParams {
    vec4 u_SnowCoverageParams;      // (heightStart, heightFull, slopeStart, slopeFull)
    vec4 u_SnowAlbedoAndRoughness;  // (albedo.rgb, roughness)
    vec4 u_SnowSSSColorAndIntensity;// (sssColor.rgb, sssIntensity)
    vec4 u_SnowSparkleParams;       // (sparkleIntensity, sparkleDensity, sparkleScale, normalPerturbStrength)
    vec4 u_SnowFlags;               // (enabled, pad, pad, pad)
};

// =============================================================================
// TEXTURE BINDINGS
// =============================================================================

// Texture bindings following ShaderBindingLayout
layout(binding = 0) uniform sampler2D u_AlbedoMap;          // TEX_DIFFUSE
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap; // TEX_SPECULAR (repurposed)
layout(binding = 2) uniform sampler2D u_NormalMap;          // TEX_NORMAL
layout(binding = 4) uniform sampler2D u_AOMap;              // TEX_AMBIENT
layout(binding = 5) uniform sampler2D u_EmissiveMap;        // TEX_EMISSIVE
layout(binding = 9) uniform samplerCube u_EnvironmentMap;   // TEX_ENVIRONMENT

// IBL textures (if available)
layout(binding = 10) uniform samplerCube u_IrradianceMap;   // TEX_USER_0
layout(binding = 11) uniform samplerCube u_PrefilterMap;    // TEX_USER_1
layout(binding = 12) uniform sampler2D u_BRDFLutMap;        // TEX_USER_2

// Shadow map textures
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM; // TEX_SHADOW (CSM)
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowMapSpot; // TEX_SHADOW_SPOT

// Point light shadow cubemaps (TEX_SHADOW_POINT_0..3)
layout(binding = 14) uniform samplerCubeShadow u_ShadowMapPoint0;
layout(binding = 15) uniform samplerCubeShadow u_ShadowMapPoint1;
layout(binding = 16) uniform samplerCubeShadow u_ShadowMapPoint2;
layout(binding = 17) uniform samplerCubeShadow u_ShadowMapPoint3;

// Shadow UBO (binding 6)
layout(std140, binding = 6) uniform ShadowData {
    mat4 u_DirectionalLightSpaceMatrices[4];
    vec4 u_CascadePlaneDistances;
    vec4 u_ShadowParams;  // x=bias, y=normalBias, z=softness, w=maxShadowDistance
    mat4 u_SpotLightSpaceMatrices[4];
    vec4 u_PointLightShadowParams[4]; // xyz=position, w=farPlane
    int u_DirectionalShadowEnabled;
    int u_SpotShadowCount;
    int u_PointShadowCount;
    int u_ShadowMapResolution;
    int u_CascadeDebugEnabled;
    int _shadowPad0;
    int _shadowPad1;
    int _shadowPad2;
};

// =============================================================================
// INPUT/OUTPUT
// =============================================================================

// Input from vertex shader
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

// Output
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;

// Octahedral encode: unit normal → RG16F [-1,1]²
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// Model UBO (binding 3) for entity ID access
layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

// =============================================================================
// MAIN FRAGMENT SHADER
// =============================================================================

void main()
{
    vec3 albedo = sampleAlbedo(u_AlbedoMap, v_TexCoord, u_BaseColorFactor.rgb, bool(u_UseAlbedoMap));
    vec2 metallicRoughness = sampleMetallicRoughness(u_MetallicRoughnessMap, v_TexCoord,
                                                     u_MetallicFactor, u_RoughnessFactor,
                                                     bool(u_UseMetallicRoughnessMap));
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;

    float ao = sampleAO(u_AOMap, v_TexCoord, u_OcclusionStrength, bool(u_UseAOMap));
    vec3 emissive = sampleEmissive(u_EmissiveMap, v_TexCoord, u_EmissiveFactor.rgb, bool(u_UseEmissiveMap));

    // Calculate normal
    vec3 N = normalize(v_Normal);
    if (u_UseNormalMap == 1)
    {
        N = getNormalFromMap(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale);
    }
    vec3 V = normalize(u_CameraPosition - v_WorldPos);

    // Calculate direct lighting from all lights
    vec3 Lo = vec3(0.0);
    for (int i = 0; i < min(u_LightCount, MAX_LIGHTS); ++i)
    {
        vec3 lightContrib = calculateLightContribution(u_Lights[i], N, V, albedo, metallic, roughness, v_WorldPos);

        // Apply shadow factor for directional lights (type 0)
        int lightType = int(u_Lights[i].position.w);
        if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
            // Compute view-space depth for cascade selection
            vec4 viewSpacePos = u_View * vec4(v_WorldPos, 1.0);
            float viewDepth = viewSpacePos.z;

            float shadow = calculateCascadedShadowFactorCSM(
                u_ShadowMapCSM,
                v_WorldPos,
                viewDepth,
                u_DirectionalLightSpaceMatrices,
                u_CascadePlaneDistances,
                u_ShadowParams,
                u_ShadowMapResolution
            );
            lightContrib *= shadow;
        }
        // Apply spot light shadows
        else if (lightType == SPOT_LIGHT)
        {
            int spotShadowIdx = int(u_Lights[i].direction.w);
            if (spotShadowIdx >= 0 && spotShadowIdx < u_SpotShadowCount)
            {
                float shadow = calculateShadowFactor(
                    v_WorldPos,
                    u_SpotLightSpaceMatrices[spotShadowIdx],
                    u_ShadowMapSpot,
                    float(spotShadowIdx),
                    u_ShadowParams.x,
                    u_ShadowMapResolution
                );
                lightContrib *= shadow;
            }
        }
        // Apply point light shadows
        else if (lightType == POINT_LIGHT)
        {
            int pointShadowIdx = int(u_Lights[i].direction.w);
            if (pointShadowIdx >= 0 && pointShadowIdx < u_PointShadowCount)
            {
                vec3 lightPos = u_PointLightShadowParams[pointShadowIdx].xyz;
                float farPlane = u_PointLightShadowParams[pointShadowIdx].w;
                float bias = u_ShadowParams.x;

                float shadow = 1.0;
                if (pointShadowIdx == 0)
                    shadow = calculatePointShadowFactor(u_ShadowMapPoint0, v_WorldPos, lightPos, farPlane, bias);
                else if (pointShadowIdx == 1)
                    shadow = calculatePointShadowFactor(u_ShadowMapPoint1, v_WorldPos, lightPos, farPlane, bias);
                else if (pointShadowIdx == 2)
                    shadow = calculatePointShadowFactor(u_ShadowMapPoint2, v_WorldPos, lightPos, farPlane, bias);
                else if (pointShadowIdx == 3)
                    shadow = calculatePointShadowFactor(u_ShadowMapPoint3, v_WorldPos, lightPos, farPlane, bias);

                lightContrib *= shadow;
            }
        }

        Lo += lightContrib;
    }

    // Calculate ambient lighting
    vec3 ambient = vec3(0.0);
    if (u_EnableIBL == 1)
    {
        ambient = calculateIBL(N, V, albedo, metallic, roughness, u_IrradianceMap, u_PrefilterMap, u_BRDFLutMap);
    }
    else
    {
        ambient = calculateSimpleAmbient(albedo, metallic, ao);
    }

    // Combine lighting
    vec3 color = ambient + Lo + emissive;

    // Apply ambient occlusion to ambient lighting only
    color = mix(color, color * ao, 0.5);

    // Cascade debug visualization: tint output by cascade index (applied in linear HDR space)
    if (u_CascadeDebugEnabled != 0 && u_DirectionalShadowEnabled != 0)
    {
        vec4 viewSpacePos = u_View * vec4(v_WorldPos, 1.0);
        float viewDepth = -viewSpacePos.z;
        vec3 cascadeColors[4] = vec3[4](
            vec3(1.0, 0.2, 0.2),  // Cascade 0: red
            vec3(0.2, 1.0, 0.2),  // Cascade 1: green
            vec3(0.2, 0.2, 1.0),  // Cascade 2: blue
            vec3(1.0, 1.0, 0.2)   // Cascade 3: yellow
        );
        int cascadeIdx = 3;
        for (int c = 0; c < 4; ++c)
        {
            if (viewDepth < u_CascadePlaneDistances[c])
            {
                cascadeIdx = c;
                break;
            }
        }
        color = mix(color, cascadeColors[cascadeIdx], 0.3);
    }

    // Snow overlay
    float snowWeight = 0.0;
    if (u_SnowFlags.x > 0.5)
    {
        vec3 worldNormal = normalize(v_Normal);
        snowWeight = computeSnowWeight(v_WorldPos.y, worldNormal,
                                       u_SnowCoverageParams.x, u_SnowCoverageParams.y,
                                       u_SnowCoverageParams.z, u_SnowCoverageParams.w,
                                       u_SnowFlags.y);

        if (snowWeight > 0.001)
        {
            vec3 snowAlbedo = u_SnowAlbedoAndRoughness.rgb;
            float snowRoughness = u_SnowAlbedoAndRoughness.w;
            vec3 sssColor = u_SnowSSSColorAndIntensity.rgb;
            float sssIntensity = u_SnowSSSColorAndIntensity.w;
            float sparkleIntensity = u_SnowSparkleParams.x;
            float sparkleDensity = u_SnowSparkleParams.y;
            float sparkleScale = u_SnowSparkleParams.z;
            float normalPerturbStr = u_SnowSparkleParams.w;

            // Perturb normal for crystalline micro-surface
            vec3 snowN = perturbSnowNormal(N, v_WorldPos, normalPerturbStr);

            // Recompute lighting with snow BRDF
            vec3 snowLo = vec3(0.0);
            for (int i = 0; i < min(u_LightCount, MAX_LIGHTS); ++i)
            {
                vec3 L = vec3(0.0);
                vec3 lightColor = u_Lights[i].color.rgb * u_Lights[i].color.w;
                float attenuation = 1.0;
                int lightType = int(u_Lights[i].position.w);

                if (lightType == DIRECTIONAL_LIGHT)
                {
                    L = normalize(-u_Lights[i].direction.xyz);
                }
                else
                {
                    vec3 toLight = u_Lights[i].position.xyz - v_WorldPos;
                    float dist = length(toLight);
                    L = toLight / dist;
                    float constant = u_Lights[i].attenuationParams.x;
                    float linear = u_Lights[i].attenuationParams.y;
                    float quadratic = u_Lights[i].attenuationParams.z;
                    attenuation = 1.0 / (constant + linear * dist + quadratic * dist * dist);
                }

                vec3 contrib = snowBRDF(snowN, V, L, snowAlbedo, snowRoughness,
                                        sssColor, sssIntensity, sparkleIntensity,
                                        sparkleDensity, sparkleScale, v_WorldPos);
                snowLo += contrib * lightColor * attenuation;
            }

            vec3 snowAmbient = 0.15 * snowAlbedo;
            vec3 snowColor = snowAmbient + snowLo;

            color = mix(color, snowColor, snowWeight);
        }
    }

    o_Color = vec4(color, u_BaseColorFactor.a);
    // SSS mask: write snow weight to alpha for SSSRenderPass bilateral blur.
    // Alpha is reset to 1.0 by SSS_Blur before PostProcess (see SnowCommon.glsl contract).
    if (snowWeight > 0.001)
        o_Color.a = snowWeight;
    o_EntityID = u_EntityID;

    vec3 outputN = N;
    if (snowWeight > 0.001)
    {
        outputN = normalize(mix(N, vec3(0.0, 1.0, 0.0), snowWeight * 0.6));
    }
    o_ViewNormal = octEncode(normalize(mat3(u_View) * outputN));
}
