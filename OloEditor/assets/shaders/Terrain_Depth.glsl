// =============================================================================
// Terrain_Depth.glsl - Terrain Depth-Only Shader for Shadow Maps
// Part of OloEngine Terrain System (Phase 2) — with tessellation
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 8 (ADR 0011 §5, amendment (76)): vertex pull from the engine-wide
// binding 57. Draw site is the terrain patch VBO (TerrainChunk.cpp /
// TerrainVertex.h — 32 B: vec3 Position @0, vec2 TexCoord @12, vec3 Normal
// @20), so the stride is 8 floats but the FIELD ORDER differs from the
// engine `Vertex` (uv before normal); the normal is unused by this depth
// stage. Only the vertex stage pulls; the tess control/eval stages read the
// vertex stage's outputs unchanged. The GL attribute branch below is
// untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec3 a_Normal;
#endif

// GPU-driven LOD (issue #714). The shadow-caster draws keep the chunk meshes
// and set u_TerrainGpuDriven = 0, so this is inert for them — it exists so the
// depth program stays usable if the shadow path ever moves onto the node list.
#include "include/TerrainGpuDrivenVertex.glsl"

layout(location = 0) out vec3 v_Position;
layout(location = 1) out vec2 v_TexCoord;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4]);
#endif
    vec3 position = a_Position;
    vec2 texCoord = a_TexCoord;
    vec3 normal = vec3(0.0, 1.0, 0.0); // unused by this stage; the helper wants a slot
    oloTerrainApplyGpuDrivenNode(position, texCoord, normal);

    v_Position = position;
    v_TexCoord = texCoord;
}

#type tess_control
#version 460 core

layout(vertices = 3) out;

layout(location = 0) in vec3 v_Position[];
layout(location = 1) in vec2 v_TexCoord[];

layout(location = 0) out vec3 tc_Position[];
layout(location = 1) out vec2 tc_TexCoord[];

// Terrain UBO (binding 10)
#include "include/TerrainParamsBlock.glsl"

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
    // Camera-relative (issue #429): full tail so u_RenderOrigin is at offset 272.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

// Model UBO (binding 3)
#include "include/InstanceBlock_Single.glsl"

// Terrain UBO (binding 10)
#include "include/TerrainParamsBlock.glsl"

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691 Phase 3, bucket 1). Sampled in the
// VERTEX stage for displacement, which is fine: the heap SSBO (45) and the
// offset table (56) are program-wide, not fragment-only.
//
// Terrain_GBuffer.glsl and Terrain_PBR.glsl bind the same TEX_TERRAIN_HEIGHTMAP
// slot but declare the name in their OWN files, so this #define cannot reach
// them — and Terrain_GBuffer must stay slot-based anyway, being a G-Buffer
// producer the bindless route would misroute.
#ifdef OLO_BINDLESS
#define u_TerrainHeightmap OLO_HEAP_TEX_2D(23)
// CONVERTED FOR THE SAME REASON, and it was missed the first time. Its bind
// (CommandDispatch's TEX_SNOW_DEPTH BindTrackedTextureUnit) already goes
// through HeapBinding::BindTextureOrOffset, which for a bindless-variant
// program records an offset and issues NO bind. Leaving the declaration
// slot-based left this sampler unbound under OLO_RHI_BINDLESS=1, so snow
// deformation silently read zero — §5c: the unit of conversion is a C++ bind
// AND its declaration, together. Terrain_GBuffer/Terrain_PBR declare the name
// in their own files and stay slot-based, so they still get a real bind.
#define u_SnowDepthMap OLO_HEAP_TEX_2D(30)
#else
layout(binding = 23) uniform sampler2D u_TerrainHeightmap;
layout(binding = 30) uniform sampler2D u_SnowDepthMap;
#endif

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
        vec3 worldP = (u_Model * vec4(pos, 1.0)).xyz + u_RenderOrigin; // camera-relative (issue #429)
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
