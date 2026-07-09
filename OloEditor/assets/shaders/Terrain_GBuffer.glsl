// =============================================================================
// Terrain_GBuffer.glsl - Deferred G-Buffer variant of Terrain_PBR.glsl.
//
// VS → TCS → TES → FS pipeline identical to Terrain_PBR up through material
// sampling. Instead of evaluating lighting + shadows + IBL in the fragment,
// this variant writes the blended splatmap / triplanar material into the
// 4-RT G-Buffer with `emissive.a = 0.0` (lit flag) so
// `DeferredLightingPass` performs full PBR evaluation.
//
// **Snow handling:** snow_weight-modulated material blend (albedo →
// snow albedo, roughness → snow roughness, normal → perturbed snow normal).
// The subsurface-scatter pass used by the forward path is not available in
// deferred; snow is treated as a diffuse overlay for now.
//
// **Velocity:** terrain is assumed static (zero velocity written). A future
// dynamic-terrain pass could add per-entity `u_PrevModel` + previous-frame
// heightmap sampling here.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec3 a_Normal;

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

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Camera-relative (issue #429): full tail so u_RenderOrigin lands at its
    // std140 offset (272). Deferred terrain adds it back to reconstruct absolute
    // world position for the world-anchored triplanar/snow/brush patterns.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

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
            gl_TessLevelInner[0] = u_TessFactors.x;
            gl_TessLevelOuter[0] = u_TessFactors.y;
            gl_TessLevelOuter[1] = u_TessFactors.z;
            gl_TessLevelOuter[2] = u_TessFactors.w;
        }
        else
        {
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

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Camera-relative (issue #429): full tail so u_RenderOrigin lands at its
    // std140 offset (272). Deferred terrain adds it back to reconstruct absolute
    // world position for the world-anchored triplanar/snow/brush patterns.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

#include "include/InstanceBlock_Single.glsl"

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

layout(std140, binding = 16) uniform SnowAccumulationParams {
    mat4 u_ClipmapViewProj[3];
    vec4 u_ClipmapCenterAndExtent[3];
    vec4 u_AccumulationParams;
    vec4 u_DisplacementParams;
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
    pos.y = sampledHeight;

    if (u_DisplacementParams.z > 0.5)
    {
        vec2 clipCenter = u_ClipmapCenterAndExtent[0].xy;
        float clipExtent = u_ClipmapCenterAndExtent[0].z;
        vec3 worldP = (u_Model * vec4(pos, 1.0)).xyz + u_RenderOrigin; // camera-relative (issue #429)
        vec2 snowUV = (worldP.xz - clipCenter) / clipExtent + 0.5;
        if (snowUV.x >= 0.0 && snowUV.x <= 1.0 && snowUV.y >= 0.0 && snowUV.y <= 1.0)
        {
            float snowDepth = texture(u_SnowDepthMap, snowUV).r;
            pos.y += snowDepth * u_DisplacementParams.x;
        }
    }

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

// Inlined helpers (avoid pulling SnowCommon.glsl which depends on PBR helpers).
float hash11(float n) { return fract(sin(n) * 43758.5453); }
vec3 hash33(vec3 p)
{
    return vec3(hash11(dot(p, vec3(127.1, 311.7,  74.7))),
                hash11(dot(p, vec3(269.5, 183.3, 246.1))),
                hash11(dot(p, vec3(113.5, 271.9, 124.6))));
}
vec3 smoothNoise3(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    vec3 a = mix(hash33(i + vec3(0,0,0)), hash33(i + vec3(1,0,0)), f.x);
    vec3 b = mix(hash33(i + vec3(0,1,0)), hash33(i + vec3(1,1,0)), f.x);
    vec3 c = mix(a, b, f.y);
    vec3 d = mix(hash33(i + vec3(0,0,1)), hash33(i + vec3(1,0,1)), f.x);
    vec3 e = mix(hash33(i + vec3(0,1,1)), hash33(i + vec3(1,1,1)), f.x);
    return mix(c, mix(d, e, f.y), f.z);
}
vec3 perturbSnowNormal(vec3 N, vec3 worldPos, float strength)
{
    if (strength < 0.001) return N;
    vec3 n1 = smoothNoise3(worldPos *  8.0) * 2.0 - 1.0;
    vec3 n2 = smoothNoise3(worldPos * 32.0) * 2.0 - 1.0;
    vec3 perturb = (n1 * 0.6 + n2 * 0.4) * strength;
    perturb -= N * dot(perturb, N);
    return normalize(N + perturb);
}
float computeSnowWeight(float worldPosY, float normalY, float heightStart,
                        float heightFull, float slopeStart, float slopeFull)
{
    float h = smoothstep(heightStart, heightFull, worldPosY);
    float s = smoothstep(slopeFull, slopeStart, normalY);
    return h * s;
}

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Camera-relative (issue #429): full tail so u_RenderOrigin lands at its
    // std140 offset (272). Deferred terrain adds it back to reconstruct absolute
    // world position for the world-anchored triplanar/snow/brush patterns.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

// Terrain is single-instance; match the tess_eval include so v_InstanceIndex
// isn't declared as fragment input without a producer (link error).
#include "include/InstanceBlock_Single.glsl"

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

// Brush preview UBO (editor-only overlay).
layout(std140, binding = 11) uniform BrushPreview {
    vec4 u_BrushPosAndRadius;
    vec4 u_BrushParams;
};

layout(std140, binding = 13) uniform SnowParams {
    vec4 u_SnowCoverageParams;
    vec4 u_SnowAlbedoAndRoughness;
    vec4 u_SnowSSSColorAndIntensity;
    vec4 u_SnowSparkleParams;
    vec4 u_SnowFlags;
};

layout(std140, binding = 16) uniform SnowAccumulationParamsFS {
    mat4 u_ClipmapViewProjFS[3];
    vec4 u_ClipmapCenterAndExtentFS[3];
    vec4 u_AccumulationParamsFS;
    vec4 u_DisplacementParamsFS;
};

layout(binding = 24) uniform sampler2D u_TerrainSplatmap0;
layout(binding = 25) uniform sampler2DArray u_TerrainAlbedoArray;
layout(binding = 26) uniform sampler2DArray u_TerrainNormalArray;
layout(binding = 27) uniform sampler2DArray u_TerrainARMArray;
layout(binding = 28) uniform sampler2D u_TerrainSplatmap1;
layout(binding = 30) uniform sampler2D u_SnowDepthMapFS;

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

layout(location = 0) out vec4 o_GBufferAlbedo;
layout(location = 1) out vec4 o_GBufferNormal;
layout(location = 2) out vec4 o_GBufferEmissive;
layout(location = 3) out vec2 o_GBufferVelocity;
layout(location = 4) out int  o_GBufferEntityID;

vec2 octEncodeGB(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// =============================================================================
// SPLATMAP HELPERS (duplicated from Terrain_PBR.glsl — same logic, G-Buffer output)
// =============================================================================

float getLayerTiling(int layer)
{
    if (layer < 4) return u_LayerTilingScales0[layer];
    else           return u_LayerTilingScales1[layer - 4];
}

float getLayerBlendSharpness(int layer)
{
    if (layer < 4) return u_LayerBlendSharpness0[layer];
    else           return u_LayerBlendSharpness1[layer - 4];
}

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

vec4 sampleLayerAlbedo(int layer, vec2 uv)
{
    return texture(u_TerrainAlbedoArray, vec3(uv * getLayerTiling(layer), float(layer)));
}

vec3 sampleLayerNormal(int layer, vec2 uv)
{
    vec3 n = texture(u_TerrainNormalArray, vec3(uv * getLayerTiling(layer), float(layer))).rgb;
    return n * 2.0 - 1.0;
}

vec4 sampleLayerARM(int layer, vec2 uv)
{
    return texture(u_TerrainARMArray, vec3(uv * getLayerTiling(layer), float(layer)));
}

vec4 triplanarSampleAlbedo(int layer, vec3 worldPos, vec3 blendWeights)
{
    worldPos += u_RenderOrigin; // camera-relative: world-anchored tiling (issue #429)
    float tiling = getLayerTiling(layer);
    vec4 xProj = texture(u_TerrainAlbedoArray, vec3(worldPos.yz * tiling, float(layer)));
    vec4 yProj = texture(u_TerrainAlbedoArray, vec3(worldPos.xz * tiling, float(layer)));
    vec4 zProj = texture(u_TerrainAlbedoArray, vec3(worldPos.xy * tiling, float(layer)));
    return xProj * blendWeights.x + yProj * blendWeights.y + zProj * blendWeights.z;
}

vec3 triplanarSampleNormal(int layer, vec3 worldPos, vec3 blendWeights)
{
    worldPos += u_RenderOrigin; // camera-relative: world-anchored tiling (issue #429)
    float tiling = getLayerTiling(layer);
    vec3 xNorm = texture(u_TerrainNormalArray, vec3(worldPos.yz * tiling, float(layer))).rgb * 2.0 - 1.0;
    vec3 yNorm = texture(u_TerrainNormalArray, vec3(worldPos.xz * tiling, float(layer))).rgb * 2.0 - 1.0;
    vec3 zNorm = texture(u_TerrainNormalArray, vec3(worldPos.xy * tiling, float(layer))).rgb * 2.0 - 1.0;
    return normalize(xNorm * blendWeights.x + yNorm * blendWeights.y + zNorm * blendWeights.z);
}

vec4 triplanarSampleARM(int layer, vec3 worldPos, vec3 blendWeights)
{
    worldPos += u_RenderOrigin; // camera-relative: world-anchored tiling (issue #429)
    float tiling = getLayerTiling(layer);
    vec4 xARM = texture(u_TerrainARMArray, vec3(worldPos.yz * tiling, float(layer)));
    vec4 yARM = texture(u_TerrainARMArray, vec3(worldPos.xz * tiling, float(layer)));
    vec4 zARM = texture(u_TerrainARMArray, vec3(worldPos.xy * tiling, float(layer)));
    return xARM * blendWeights.x + yARM * blendWeights.y + zARM * blendWeights.z;
}

void main()
{
    vec3 N = normalize(v_Normal);
    // Camera-relative (issue #429): absolute world position for the world-
    // anchored brush/snow patterns below (lighting/velocity keep v_WorldPos).
    vec3 worldPosAbs = v_WorldPos + u_RenderOrigin;
    int layerCount = int(u_TerrainParams.z);
    float triplanarSharpness = u_TerrainParams.w;

    bool useSplatmap = (layerCount > 0);

    vec3 albedo;
    float metallic;
    float roughness;
    float ao;
    vec3 normalMap = vec3(0.0, 0.0, 1.0);

    if (useSplatmap)
    {
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

        vec3 absNormal = abs(N);
        vec3 triWeights = pow(absNormal, vec3(max(triplanarSharpness, 1.0)));
        triWeights /= (triWeights.x + triWeights.y + triWeights.z + 0.0001);

        float slope = 1.0 - N.y;
        bool useTriplanar = (slope > 0.4);

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

        heightBlend(weights, heights, layerCount);

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
        float slope = 1.0 - N.y;
        vec3 grassColor = vec3(0.15, 0.35, 0.08);
        vec3 rockColor  = vec3(0.45, 0.4, 0.35);
        albedo = mix(grassColor, rockColor, smoothstep(0.3, 0.7, slope));
        metallic = 0.0;
        roughness = mix(0.85, 0.95, slope);
        ao = 1.0;
    }

    // Apply TBN-space normal when splatmap is active.
    if (useSplatmap)
    {
        vec3 T = normalize(cross(N, vec3(0.0, 0.0, 1.0)));
        if (length(T) < 0.001)
            T = normalize(cross(N, vec3(1.0, 0.0, 0.0)));
        vec3 B = cross(N, T);
        mat3 TBN = mat3(T, B, N);
        N = normalize(TBN * normalMap);
    }

    // Brush preview overlay (editor).
    if (u_BrushParams.x > 0.5)
    {
        vec3 brushCenter = u_BrushPosAndRadius.xyz;
        float brushRadius = u_BrushPosAndRadius.w;
        float falloff = u_BrushParams.y;

        float dist = length(worldPosAbs.xz - brushCenter.xz);
        float normalizedDist = dist / max(brushRadius, 0.001);

        if (normalizedDist < 1.0)
        {
            float innerRadius = falloff;
            float weight = normalizedDist < innerRadius ? 1.0 :
                0.5 + 0.5 * cos(3.14159265 * (normalizedDist - innerRadius) / (1.0 - innerRadius));

            vec3 brushColor = u_BrushParams.z < 0.5 ? vec3(0.0, 0.8, 1.0) : vec3(0.2, 1.0, 0.3);

            float edgeDist = abs(normalizedDist - 1.0);
            float ring = smoothstep(0.03, 0.0, edgeDist);

            float fill = weight * 0.15;
            albedo = mix(albedo, brushColor, fill + ring * 0.6);
        }
    }

    // Snow overlay — blend material properties by snow weight. SSS/sparkle
    // are forward-only (need lighting-pass access); see docs limitations.
    if (u_SnowFlags.x > 0.5)
    {
        vec3 worldNormal = normalize(v_Normal);
        float snowWeight = computeSnowWeight(worldPosAbs.y, worldNormal.y,
                                             u_SnowCoverageParams.x, u_SnowCoverageParams.y,
                                             u_SnowCoverageParams.z, u_SnowCoverageParams.w);

        if (u_DisplacementParamsFS.z > 0.5)
        {
            vec2 clipCenterFS = u_ClipmapCenterAndExtentFS[0].xy;
            float clipExtentFS = u_ClipmapCenterAndExtentFS[0].z;
            vec2 snowUVFS = (worldPosAbs.xz - clipCenterFS) / clipExtentFS + 0.5;
            if (snowUVFS.x >= 0.0 && snowUVFS.x <= 1.0 && snowUVFS.y >= 0.0 && snowUVFS.y <= 1.0)
            {
                float accumulatedDepth = texture(u_SnowDepthMapFS, snowUVFS).r;
                float maxDepth = u_AccumulationParamsFS.y;
                float depthFactor = clamp(accumulatedDepth / max(maxDepth, 0.01), 0.0, 1.0);
                snowWeight = max(snowWeight, depthFactor);
            }
        }

        if (snowWeight > 0.001)
        {
            vec3 snowAlbedo = u_SnowAlbedoAndRoughness.rgb;
            float snowRoughness = u_SnowAlbedoAndRoughness.w;
            float normalPerturbStr = u_SnowSparkleParams.w;
            vec3 snowN = perturbSnowNormal(N, worldPosAbs, normalPerturbStr);

            albedo = mix(albedo, snowAlbedo, snowWeight);
            roughness = mix(roughness, snowRoughness, snowWeight);
            metallic = mix(metallic, 0.0, snowWeight);
            N = normalize(mix(N, snowN, snowWeight));
            // Snow fills crevices — bias AO upward under heavy snow.
            ao = mix(ao, 1.0, snowWeight * 0.6);
        }
    }

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    // emissive.a = 0.0 → lit. Terrain does not emit light of its own.
    o_GBufferEmissive = vec4(0.0, 0.0, 0.0, 0.0);
    // Static terrain → zero screen-space velocity.
    o_GBufferVelocity = vec2(0.0);
    o_GBufferEntityID = u_EntityID;
}
