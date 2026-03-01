// =============================================================================
// Terrain_PBR.glsl - Terrain PBR Rendering Shader with GPU Tessellation
// Part of OloEngine Terrain System (Phase 2)
// VS → TCS → TES → FS pipeline with adaptive tessellation from quadtree LOD
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec3 a_Normal;

// Pass through to tessellation control shader
layout(location = 0) out vec3 v_Position;
layout(location = 1) out vec2 v_TexCoord;
layout(location = 2) out vec3 v_Normal;

void main()
{
    v_Position = a_Position;
    v_TexCoord = a_TexCoord;
    v_Normal = a_Normal;
}

#type tess_control
#version 460 core

layout(vertices = 3) out;

layout(location = 0) in vec3 v_Position[];
layout(location = 1) in vec2 v_TexCoord[];
layout(location = 2) in vec3 v_Normal[];

layout(location = 0) out vec3 tc_Position[];
layout(location = 1) out vec2 tc_TexCoord[];
layout(location = 2) out vec3 tc_Normal[];

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Terrain UBO (binding 10)
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

// Distance-based tessellation with quadtree LOD override
float calcTessLevel(vec3 p0, vec3 p1)
{
    vec3 mid = (p0 + p1) * 0.5;
    float dist = distance(mid, u_CameraPosition);
    float edgeLen = distance(p0, p1);
    float projScale = u_Projection[1][1] * 0.5;
    float screenLen = (edgeLen * projScale) / max(dist, 0.001);
    return clamp(screenLen / 8.0, 1.0, 64.0);
}

void main()
{
    tc_Position[gl_InvocationID] = v_Position[gl_InvocationID];
    tc_TexCoord[gl_InvocationID] = v_TexCoord[gl_InvocationID];
    tc_Normal[gl_InvocationID]   = v_Normal[gl_InvocationID];

    if (gl_InvocationID == 0)
    {
        if (u_TessFactors2.w > 0.5)
        {
            // Use quadtree-provided tessellation factors
            gl_TessLevelInner[0] = u_TessFactors.x;
            gl_TessLevelOuter[0] = u_TessFactors.y;
            gl_TessLevelOuter[1] = u_TessFactors.z;
            gl_TessLevelOuter[2] = u_TessFactors.w;
        }
        else
        {
            // Fallback: distance-based tessellation
            float e0 = calcTessLevel(v_Position[1], v_Position[2]);
            float e1 = calcTessLevel(v_Position[2], v_Position[0]);
            float e2 = calcTessLevel(v_Position[0], v_Position[1]);

            gl_TessLevelInner[0] = (e0 + e1 + e2) / 3.0;
            gl_TessLevelOuter[0] = e0;
            gl_TessLevelOuter[1] = e1;
            gl_TessLevelOuter[2] = e2;
        }
    }
}

#type tess_evaluation
#version 460 core

layout(triangles, equal_spacing, ccw) in;

layout(location = 0) in vec3 tc_Position[];
layout(location = 1) in vec2 tc_TexCoord[];
layout(location = 2) in vec3 tc_Normal[];

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

// Terrain UBO (binding 10)
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

layout(binding = 23) uniform sampler2D u_TerrainHeightmap;
layout(binding = 30) uniform sampler2D u_SnowDepthMap;

// Snow Accumulation UBO (binding 16)
layout(std140, binding = 16) uniform SnowAccumulationParams {
    mat4 u_ClipmapViewProj[3];
    vec4 u_ClipmapCenterAndExtent[3];
    vec4 u_AccumulationParams;   // x=rate, y=maxDepth, z=meltRate, w=restorationRate
    vec4 u_DisplacementParams;   // x=displacementScale, y=snowDensity, z=enabled, w=numRings
};

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

vec3 interpolate3(vec3 a, vec3 b, vec3 c)
{
    return gl_TessCoord.x * a + gl_TessCoord.y * b + gl_TessCoord.z * c;
}

vec2 interpolate2(vec2 a, vec2 b, vec2 c)
{
    return gl_TessCoord.x * a + gl_TessCoord.y * b + gl_TessCoord.z * c;
}

void main()
{
    vec3 pos = interpolate3(tc_Position[0], tc_Position[1], tc_Position[2]);
    vec2 uv  = interpolate2(tc_TexCoord[0], tc_TexCoord[1], tc_TexCoord[2]);
    vec3 nrm = normalize(interpolate3(tc_Normal[0], tc_Normal[1], tc_Normal[2]));

    float heightScale = u_WorldSizeAndHeightScale.z;
    float sampledHeight = texture(u_TerrainHeightmap, uv).r * heightScale;

    // Displace Y from heightmap
    pos.y = sampledHeight;

    // Snow accumulation displacement
    float snowDisplacement = 0.0;
    if (u_DisplacementParams.z > 0.5)
    {
        // Convert world XZ to clipmap UV (ring 0)
        vec2 clipCenter = u_ClipmapCenterAndExtent[0].xy;
        float clipExtent = u_ClipmapCenterAndExtent[0].z;
        // Compute world position for this vertex
        vec3 worldP = (u_Model * vec4(pos, 1.0)).xyz;
        vec2 snowUV = (worldP.xz - clipCenter) / clipExtent + 0.5;
        if (snowUV.x >= 0.0 && snowUV.x <= 1.0 && snowUV.y >= 0.0 && snowUV.y <= 1.0)
        {
            float snowDepth = texture(u_SnowDepthMap, snowUV).r;
            snowDisplacement = snowDepth * u_DisplacementParams.x;
            pos.y += snowDisplacement;
        }
    }

    // Recompute normal from heightmap derivatives
    float texelSize = u_TerrainParams.x;
    float worldSizeX = u_WorldSizeAndHeightScale.x;
    float worldSizeZ = u_WorldSizeAndHeightScale.y;

    float hL = texture(u_TerrainHeightmap, uv + vec2(-texelSize, 0.0)).r * heightScale;
    float hR = texture(u_TerrainHeightmap, uv + vec2( texelSize, 0.0)).r * heightScale;
    float hD = texture(u_TerrainHeightmap, uv + vec2(0.0, -texelSize)).r * heightScale;
    float hU = texture(u_TerrainHeightmap, uv + vec2(0.0,  texelSize)).r * heightScale;

    float dX = (hR - hL) / (2.0 * texelSize * worldSizeX);
    float dZ = (hU - hD) / (2.0 * texelSize * worldSizeZ);
    nrm = normalize(vec3(-dX, 1.0, -dZ));

    // LOD morph blending to prevent popping
    float morphFactor = u_TessFactors2.y;
    float meshHeight = interpolate3(tc_Position[0], tc_Position[1], tc_Position[2]).y;
    vec3 meshNormal = normalize(interpolate3(tc_Normal[0], tc_Normal[1], tc_Normal[2]));
    pos.y = mix(sampledHeight, meshHeight, morphFactor);
    nrm = normalize(mix(nrm, meshNormal, morphFactor));

    vec4 worldPos = u_Model * vec4(pos, 1.0);
    v_WorldPos = worldPos.xyz;
    v_Normal = mat3(u_Normal) * nrm;
    v_TexCoord = uv;

    gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"
#include "include/SnowCommon.glsl"
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

// Terrain UBO (binding 10)
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

// Brush Preview UBO (binding 11) — editor-only terrain brush visualization
layout(std140, binding = 11) uniform BrushPreview {
    vec4 u_BrushPosAndRadius;  // xyz = world position, w = radius
    vec4 u_BrushParams;        // x = active (1.0/0.0), y = falloff, z = mode, w = unused
};

// Snow UBO (binding 13)
layout(std140, binding = 13) uniform SnowParams {
    vec4 u_SnowCoverageParams;
    vec4 u_SnowAlbedoAndRoughness;
    vec4 u_SnowSSSColorAndIntensity;
    vec4 u_SnowSparkleParams;
    vec4 u_SnowFlags;
};

// Snow Accumulation UBO (binding 16) — fragment access
layout(std140, binding = 16) uniform SnowAccumulationParamsFS {
    mat4 u_ClipmapViewProjFS[3];
    vec4 u_ClipmapCenterAndExtentFS[3];
    vec4 u_AccumulationParamsFS;
    vec4 u_DisplacementParamsFS;
};

layout(binding = 30) uniform sampler2D u_SnowDepthMapFS;

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

// Terrain texture arrays
layout(binding = 24) uniform sampler2D u_TerrainSplatmap0;     // Layers 0-3 weights
layout(binding = 25) uniform sampler2DArray u_TerrainAlbedoArray;
layout(binding = 26) uniform sampler2DArray u_TerrainNormalArray;
layout(binding = 27) uniform sampler2DArray u_TerrainARMArray;
layout(binding = 28) uniform sampler2D u_TerrainSplatmap1;     // Layers 4-7 weights

// =============================================================================
// INPUT/OUTPUT
// =============================================================================

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

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
// SPLATMAP MATERIAL HELPERS
// =============================================================================

// Get tiling scale for a given layer index
float getLayerTiling(int layer)
{
    if (layer < 4)
        return u_LayerTilingScales0[layer];
    else
        return u_LayerTilingScales1[layer - 4];
}

// Get height blend sharpness for a given layer index
float getLayerBlendSharpness(int layer)
{
    if (layer < 4)
        return u_LayerBlendSharpness0[layer];
    else
        return u_LayerBlendSharpness1[layer - 4];
}

// Height-based blending: sharper transitions based on height value from ARM.a channel
// weights: raw splatmap weights; heights: per-layer height values
void heightBlend(inout float weights[8], float heights[8], int layerCount)
{
    float maxHeight = -1e10;
    for (int i = 0; i < layerCount; ++i)
    {
        float h = heights[i] + weights[i];
        maxHeight = max(maxHeight, h);
    }

    float sum = 0.0;
    for (int i = 0; i < layerCount; ++i)
    {
        float h = heights[i] + weights[i];
        float sharpness = getLayerBlendSharpness(i);
        weights[i] = max(h - maxHeight + (1.0 / max(sharpness, 0.01)), 0.0);
        sum += weights[i];
    }

    if (sum > 0.0)
    {
        for (int i = 0; i < layerCount; ++i)
            weights[i] /= sum;
    }
}

// Sample a layer using planar UV
vec4 sampleLayerAlbedo(int layer, vec2 uv)
{
    return texture(u_TerrainAlbedoArray, vec3(uv * getLayerTiling(layer), float(layer)));
}

vec3 sampleLayerNormal(int layer, vec2 uv)
{
    vec3 n = texture(u_TerrainNormalArray, vec3(uv * getLayerTiling(layer), float(layer))).rgb;
    return n * 2.0 - 1.0; // Decode from [0,1] to [-1,1]
}

vec4 sampleLayerARM(int layer, vec2 uv)
{
    return texture(u_TerrainARMArray, vec3(uv * getLayerTiling(layer), float(layer)));
}

// Triplanar sampling for steep slopes
vec4 triplanarSampleAlbedo(int layer, vec3 worldPos, vec3 blendWeights)
{
    float tiling = getLayerTiling(layer);
    vec4 xProj = texture(u_TerrainAlbedoArray, vec3(worldPos.yz * tiling, float(layer)));
    vec4 yProj = texture(u_TerrainAlbedoArray, vec3(worldPos.xz * tiling, float(layer)));
    vec4 zProj = texture(u_TerrainAlbedoArray, vec3(worldPos.xy * tiling, float(layer)));
    return xProj * blendWeights.x + yProj * blendWeights.y + zProj * blendWeights.z;
}

vec3 triplanarSampleNormal(int layer, vec3 worldPos, vec3 blendWeights)
{
    float tiling = getLayerTiling(layer);
    vec3 xNorm = texture(u_TerrainNormalArray, vec3(worldPos.yz * tiling, float(layer))).rgb * 2.0 - 1.0;
    vec3 yNorm = texture(u_TerrainNormalArray, vec3(worldPos.xz * tiling, float(layer))).rgb * 2.0 - 1.0;
    vec3 zNorm = texture(u_TerrainNormalArray, vec3(worldPos.xy * tiling, float(layer))).rgb * 2.0 - 1.0;
    return normalize(xNorm * blendWeights.x + yNorm * blendWeights.y + zNorm * blendWeights.z);
}

vec4 triplanarSampleARM(int layer, vec3 worldPos, vec3 blendWeights)
{
    float tiling = getLayerTiling(layer);
    vec4 xARM = texture(u_TerrainARMArray, vec3(worldPos.yz * tiling, float(layer)));
    vec4 yARM = texture(u_TerrainARMArray, vec3(worldPos.xz * tiling, float(layer)));
    vec4 zARM = texture(u_TerrainARMArray, vec3(worldPos.xy * tiling, float(layer)));
    return xARM * blendWeights.x + yARM * blendWeights.y + zARM * blendWeights.z;
}

// =============================================================================
// MAIN FRAGMENT SHADER
// =============================================================================

void main()
{
    vec3 N = normalize(v_Normal);
    int layerCount = int(u_TerrainParams.z);
    float triplanarSharpness = u_TerrainParams.w;

    // Determine if we should use splatmap or procedural fallback
    bool useSplatmap = (layerCount > 0);

    vec3 albedo;
    float metallic;
    float roughness;
    float ao;
    vec3 normalMap = vec3(0.0, 0.0, 1.0);

    if (useSplatmap)
    {
        // Read splatmap weights
        float weights[8];
        vec4 splat0 = texture(u_TerrainSplatmap0, v_TexCoord);
        weights[0] = splat0.r;
        weights[1] = splat0.g;
        weights[2] = splat0.b;
        weights[3] = splat0.a;

        if (layerCount > 4)
        {
            vec4 splat1 = texture(u_TerrainSplatmap1, v_TexCoord);
            weights[4] = splat1.r;
            weights[5] = splat1.g;
            weights[6] = splat1.b;
            weights[7] = splat1.a;
        }
        else
        {
            weights[4] = 0.0; weights[5] = 0.0; weights[6] = 0.0; weights[7] = 0.0;
        }

        // Triplanar blend weights for steep slopes
        vec3 absNormal = abs(N);
        vec3 triWeights = pow(absNormal, vec3(max(triplanarSharpness, 1.0)));
        triWeights /= (triWeights.x + triWeights.y + triWeights.z + 0.0001);

        // Determine if triplanar is needed (slope threshold)
        float slope = 1.0 - N.y;
        bool useTriplanar = (slope > 0.4);

        // Sample per-layer ARM alpha as height for height-based blending
        float heights[8];
        for (int i = 0; i < layerCount; ++i)
        {
            vec4 armSample;
            if (useTriplanar)
                armSample = triplanarSampleARM(i, v_WorldPos, triWeights);
            else
                armSample = sampleLayerARM(i, v_TexCoord);
            heights[i] = armSample.a;
        }
        for (int i = layerCount; i < 8; ++i)
            heights[i] = 0.0;

        // Apply height-based blending
        heightBlend(weights, heights, layerCount);

        // Blend layers
        albedo = vec3(0.0);
        metallic = 0.0;
        roughness = 0.0;
        ao = 0.0;
        normalMap = vec3(0.0);

        for (int i = 0; i < layerCount; ++i)
        {
            if (weights[i] < 0.001)
                continue;

            vec4 layerAlbedo;
            vec3 layerNormal;
            vec4 layerARM;

            if (useTriplanar)
            {
                layerAlbedo = triplanarSampleAlbedo(i, v_WorldPos, triWeights);
                layerNormal = triplanarSampleNormal(i, v_WorldPos, triWeights);
                layerARM = triplanarSampleARM(i, v_WorldPos, triWeights);
            }
            else
            {
                layerAlbedo = sampleLayerAlbedo(i, v_TexCoord);
                layerNormal = sampleLayerNormal(i, v_TexCoord);
                layerARM = sampleLayerARM(i, v_TexCoord);
            }

            albedo += layerAlbedo.rgb * weights[i];
            normalMap += layerNormal * weights[i];
            ao += layerARM.r * weights[i];
            roughness += layerARM.g * weights[i];
            metallic += layerARM.b * weights[i];
        }

        normalMap = normalize(normalMap);
    }
    else
    {
        // Procedural fallback (no splatmap assigned)
        float slope = 1.0 - N.y;
        vec3 grassColor = vec3(0.15, 0.35, 0.08);
        vec3 rockColor = vec3(0.45, 0.4, 0.35);
        albedo = mix(grassColor, rockColor, smoothstep(0.3, 0.7, slope));
        metallic = 0.0;
        roughness = mix(0.85, 0.95, slope);
        ao = 1.0;
    }

    // Apply tangent-space normal mapping (when splatmap is active)
    if (useSplatmap)
    {
        // Build TBN from world normal (terrain-specific: T always ~X, B always ~Z)
        vec3 T = normalize(cross(N, vec3(0.0, 0.0, 1.0)));
        if (length(T) < 0.001)
            T = normalize(cross(N, vec3(1.0, 0.0, 0.0)));
        vec3 B = cross(N, T);
        mat3 TBN = mat3(T, B, N);
        N = normalize(TBN * normalMap);
    }

    vec3 V = normalize(u_CameraPosition - v_WorldPos);

    // Calculate direct lighting from all lights
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

    // Ambient / IBL
    vec3 ambient = calculateSimpleAmbient(albedo, metallic, ao);

    vec3 color = ambient + Lo;
    color = mix(color, color * ao, 0.5);

    // Brush preview overlay
    if (u_BrushParams.x > 0.5)
    {
        vec3 brushCenter = u_BrushPosAndRadius.xyz;
        float brushRadius = u_BrushPosAndRadius.w;
        float falloff = u_BrushParams.y;

        float dist = length(v_WorldPos.xz - brushCenter.xz);
        float normalizedDist = dist / max(brushRadius, 0.001);

        if (normalizedDist < 1.0)
        {
            // Cosine falloff matches the actual brush strength
            float innerRadius = falloff;
            float weight = normalizedDist < innerRadius ? 1.0 :
                0.5 + 0.5 * cos(3.14159265 * (normalizedDist - innerRadius) / (1.0 - innerRadius));

            // Brush color: cyan for sculpt, green for paint
            vec3 brushColor = u_BrushParams.z < 0.5 ? vec3(0.0, 0.8, 1.0) : vec3(0.2, 1.0, 0.3);

            // Ring at outer edge
            float edgeDist = abs(normalizedDist - 1.0);
            float ring = smoothstep(0.03, 0.0, edgeDist);

            // Fill with falloff visualization
            float fill = weight * 0.15;

            color = mix(color, brushColor, fill + ring * 0.6);
        }
    }

    // Snow overlay (applied after brush preview so snow is visible under brush)
    float snowWeight = 0.0;
    if (u_SnowFlags.x > 0.5)
    {
        vec3 worldNormal = normalize(v_Normal);
        snowWeight = computeSnowWeight(v_WorldPos.y, worldNormal,
                                       u_SnowCoverageParams.x, u_SnowCoverageParams.y,
                                       u_SnowCoverageParams.z, u_SnowCoverageParams.w,
                                       u_SnowFlags.y);

        // Boost snow weight from accumulation depth map
        if (u_DisplacementParamsFS.z > 0.5)
        {
            vec2 clipCenterFS = u_ClipmapCenterAndExtentFS[0].xy;
            float clipExtentFS = u_ClipmapCenterAndExtentFS[0].z;
            vec2 snowUVFS = (v_WorldPos.xz - clipCenterFS) / clipExtentFS + 0.5;
            if (snowUVFS.x >= 0.0 && snowUVFS.x <= 1.0 && snowUVFS.y >= 0.0 && snowUVFS.y <= 1.0)
            {
                float accumulatedDepth = texture(u_SnowDepthMapFS, snowUVFS).r;
                float maxDepth = u_AccumulationParamsFS.y;
                // Depth-based weight: thicker snow = stronger coverage
                float depthFactor = clamp(accumulatedDepth / max(maxDepth, 0.01), 0.0, 1.0);
                snowWeight = max(snowWeight, depthFactor);
            }
        }

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

            vec3 snowN = perturbSnowNormal(N, v_WorldPos, normalPerturbStr);

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

            // Snow ambient: snow has very high albedo (~0.95) and scatters
            // significant indirect light. Use a higher ambient factor than the
            // standard 0.03 to capture sky light bouncing off the snow surface.
            vec3 snowAmbient = 0.15 * snowAlbedo;
            vec3 snowColor = snowAmbient + snowLo;

            color = mix(color, snowColor, snowWeight);
        }
    }

    // Output
    o_Color = vec4(color, 1.0);
    // SSS mask: write snow weight to alpha for SSSRenderPass bilateral blur.
    // Alpha is reset to 1.0 by SSS_Blur before PostProcess (see SnowCommon.glsl contract).
    if (snowWeight > 0.001)
        o_Color.a = snowWeight;
    o_EntityID = u_EntityID;

    // View-space normal for SSAO/post-processing.
    // Snow fills geometric crevices, creating a smoother surface. Blend the
    // terrain normal toward world-up so SSAO "sees" the filled-in geometry
    // rather than producing dark occlusion in crevices hidden under snow.
    // Do NOT use the noise-perturbed snow normal here — that micro-detail is
    // for lighting only; feeding it to SSAO causes false gray occlusion.
    vec3 outputN = N;
    if (snowWeight > 0.001)
    {
        outputN = normalize(mix(N, vec3(0.0, 1.0, 0.0), snowWeight * 0.6));
    }
    vec3 viewNormal = normalize(mat3(u_View) * outputN);
    o_ViewNormal = octEncode(viewNormal);
}
