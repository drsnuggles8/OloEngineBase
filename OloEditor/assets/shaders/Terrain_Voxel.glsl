// =============================================================================
// Terrain_Voxel.glsl - Voxel Override PBR Shader with Triplanar Mapping
// Part of OloEngine Terrain System (Phase 6)
// VS → FS pipeline (no tessellation — MC generates dense triangle meshes)
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;

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

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;

void main()
{
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_WorldPos = worldPos.xyz;
    v_Normal = normalize(mat3(u_Normal) * a_Normal);
    gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"

// Camera UBO (binding 0)
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

// Model UBO (binding 3)
layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

// Terrain UBO (binding 10) — reuse terrain layer tiling/sharpness for texture arrays
layout(std140, binding = 10) uniform TerrainParams {
    vec4 u_WorldSizeAndHeightScale;
    vec4 u_TerrainParams;
    int u_HeightmapResolution;
    int _terrainPad0;
    int _terrainPad1;
    int _terrainPad2;
    vec4 u_TessFactors;
    vec4 u_TessFactors2;
    vec4 u_LayerTilingScales0;
    vec4 u_LayerTilingScales1;
    vec4 u_LayerBlendSharpness0;
    vec4 u_LayerBlendSharpness1;
};

// Shadow maps
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM;
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowMapSpot;
layout(binding = 14) uniform samplerCubeShadow u_ShadowMapPoint0;
layout(binding = 15) uniform samplerCubeShadow u_ShadowMapPoint1;
layout(binding = 16) uniform samplerCubeShadow u_ShadowMapPoint2;
layout(binding = 17) uniform samplerCubeShadow u_ShadowMapPoint3;

// IBL textures
layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D u_BRDFLutMap;

// Terrain texture arrays (same as heightmap terrain — reuses splatmap layer 0)
layout(binding = 25) uniform sampler2DArray u_TerrainAlbedoArray;
layout(binding = 26) uniform sampler2DArray u_TerrainNormalArray;
layout(binding = 27) uniform sampler2DArray u_TerrainARMArray;

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;

vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// =============================================================================
// TRIPLANAR MAPPING — always used for voxel meshes (arbitrary orientations)
// =============================================================================

void main()
{
    vec3 N = normalize(v_Normal);
    float triplanarSharpness = max(u_TerrainParams.w, 4.0);

    // Triplanar blend weights from world-space normal
    vec3 absNormal = abs(N);
    vec3 triWeights = pow(absNormal, vec3(triplanarSharpness));
    triWeights /= (triWeights.x + triWeights.y + triWeights.z + 0.0001);

    // Use layer 0 tiling scale for voxel triplanar sampling
    float tiling = u_LayerTilingScales0[0];
    if (tiling < 0.001)
        tiling = 0.1; // Sensible default

    // Sample albedo with triplanar projection
    vec4 albedoX = texture(u_TerrainAlbedoArray, vec3(v_WorldPos.yz * tiling, 0.0));
    vec4 albedoY = texture(u_TerrainAlbedoArray, vec3(v_WorldPos.xz * tiling, 0.0));
    vec4 albedoZ = texture(u_TerrainAlbedoArray, vec3(v_WorldPos.xy * tiling, 0.0));
    vec3 albedo = (albedoX.rgb * triWeights.x + albedoY.rgb * triWeights.y + albedoZ.rgb * triWeights.z);

    // Sample ARM with triplanar projection
    vec4 armX = texture(u_TerrainARMArray, vec3(v_WorldPos.yz * tiling, 0.0));
    vec4 armY = texture(u_TerrainARMArray, vec3(v_WorldPos.xz * tiling, 0.0));
    vec4 armZ = texture(u_TerrainARMArray, vec3(v_WorldPos.xy * tiling, 0.0));
    vec4 arm = armX * triWeights.x + armY * triWeights.y + armZ * triWeights.z;

    float ao = arm.r;
    float roughness = arm.g;
    float metallic = arm.b;

    // Sample normal map with triplanar projection
    vec3 normX = texture(u_TerrainNormalArray, vec3(v_WorldPos.yz * tiling, 0.0)).rgb * 2.0 - 1.0;
    vec3 normY = texture(u_TerrainNormalArray, vec3(v_WorldPos.xz * tiling, 0.0)).rgb * 2.0 - 1.0;
    vec3 normZ = texture(u_TerrainNormalArray, vec3(v_WorldPos.xy * tiling, 0.0)).rgb * 2.0 - 1.0;
    vec3 triNormal = normalize(normX * triWeights.x + normY * triWeights.y + normZ * triWeights.z);

    // Build TBN from world normal and apply tangent-space normal map
    vec3 T = normalize(cross(N, vec3(0.0, 0.0, 1.0)));
    if (length(T) < 0.001)
        T = normalize(cross(N, vec3(1.0, 0.0, 0.0)));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    N = normalize(TBN * triNormal);

    // If no texture arrays are bound, use procedural cave material
    if (albedo == vec3(0.0))
    {
        albedo = vec3(0.35, 0.32, 0.28); // Stone color
        roughness = 0.9;
        metallic = 0.0;
        ao = 1.0;
    }

    vec3 V = normalize(u_CameraPosition - v_WorldPos);

    // Direct lighting with shadows
    vec3 Lo = vec3(0.0);
    for (int i = 0; i < min(u_LightCount, MAX_LIGHTS); ++i)
    {
        vec3 lightContrib = calculateLightContribution(u_Lights[i], N, V, albedo, metallic, roughness, v_WorldPos);

        int lightType = int(u_Lights[i].position.w);
        if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
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

    // Ambient
    vec3 ambient = calculateSimpleAmbient(albedo, metallic, ao);
    vec3 color = ambient + Lo;
    color = mix(color, color * ao, 0.5);

    o_Color = vec4(color, 1.0);
    o_EntityID = u_EntityID;

    vec3 viewNormal = normalize(mat3(u_View) * N);
    o_ViewNormal = octEncode(viewNormal);
}
