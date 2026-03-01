// =============================================================================
// Terrain_Depth.glsl - Terrain Depth-Only Shader for Shadow Maps
// Part of OloEngine Terrain System (Phase 2) — with tessellation
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec3 a_Normal;

layout(location = 0) out vec3 v_Position;
layout(location = 1) out vec2 v_TexCoord;

void main()
{
    v_Position = a_Position;
    v_TexCoord = a_TexCoord;
}

#type tess_control
#version 460 core

layout(vertices = 3) out;

layout(location = 0) in vec3 v_Position[];
layout(location = 1) in vec2 v_TexCoord[];

layout(location = 0) out vec3 tc_Position[];
layout(location = 1) out vec2 tc_TexCoord[];

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

void main()
{
    tc_Position[gl_InvocationID] = v_Position[gl_InvocationID];
    tc_TexCoord[gl_InvocationID] = v_TexCoord[gl_InvocationID];

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
            // Shadow maps use moderate fixed tessellation
            gl_TessLevelInner[0] = 4.0;
            gl_TessLevelOuter[0] = 4.0;
            gl_TessLevelOuter[1] = 4.0;
            gl_TessLevelOuter[2] = 4.0;
        }
    }
}

#type tess_evaluation
#version 460 core

layout(triangles, equal_spacing, ccw) in;

layout(location = 0) in vec3 tc_Position[];
layout(location = 1) in vec2 tc_TexCoord[];

// Camera UBO (binding 0) — holds light VP during shadow pass
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
    vec4 u_AccumulationParams;
    vec4 u_DisplacementParams;
};

void main()
{
    vec3 pos = gl_TessCoord.x * tc_Position[0]
             + gl_TessCoord.y * tc_Position[1]
             + gl_TessCoord.z * tc_Position[2];
    vec2 uv  = gl_TessCoord.x * tc_TexCoord[0]
             + gl_TessCoord.y * tc_TexCoord[1]
             + gl_TessCoord.z * tc_TexCoord[2];

    // Displace Y from heightmap
    float heightScale = u_WorldSizeAndHeightScale.z;
    pos.y = texture(u_TerrainHeightmap, uv).r * heightScale;

    // Snow accumulation displacement (must match Terrain_PBR.glsl)
    if (u_DisplacementParams.z > 0.5)
    {
        vec2 clipCenter = u_ClipmapCenterAndExtent[0].xy;
        float clipExtent = u_ClipmapCenterAndExtent[0].z;
        vec3 worldP = (u_Model * vec4(pos, 1.0)).xyz;
        vec2 snowUV = (worldP.xz - clipCenter) / clipExtent + 0.5;
        if (snowUV.x >= 0.0 && snowUV.x <= 1.0 && snowUV.y >= 0.0 && snowUV.y <= 1.0)
        {
            float snowDepth = texture(u_SnowDepthMap, snowUV).r;
            pos.y += snowDepth * u_DisplacementParams.x;
        }
    }

    // Morph blend
    float morphFactor = u_TessFactors2.y;
    float meshHeight = gl_TessCoord.x * tc_Position[0].y
                     + gl_TessCoord.y * tc_Position[1].y
                     + gl_TessCoord.z * tc_Position[2].y;
    pos.y = mix(pos.y, meshHeight, morphFactor);

    gl_Position = u_ViewProjection * u_Model * vec4(pos, 1.0);
}

#type fragment
#version 460 core

void main()
{
    // Depth is written automatically by the rasterizer
}
