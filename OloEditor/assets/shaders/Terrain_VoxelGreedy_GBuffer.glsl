// OLO_NORMAL_MAP_TBN_EXEMPT: see Terrain_VoxelGreedy.glsl — the tangent frame is the analytic
// axis-aligned face normal of a cubic voxel, not a derivative TBN.
// =============================================================================
// Terrain_VoxelGreedy_GBuffer.glsl - Deferred G-Buffer variant of
// Terrain_VoxelGreedy.glsl (issue #727).
//
// Same instanced packed-quad vertex stage; writes the 4-RT G-Buffer instead of
// shading forward. `emissive.a = 0.0` (lit flag) so ComputeDeferredLit runs the
// full PBR pass. Voxel terrain is static, so velocity is zero.
//
// Selected by Renderer3D::DrawVoxelQuads when the deferred path is active.
// =============================================================================

#type vertex
#version 460 core

#include "include/VoxelQuadUnpack.glsl"

#ifdef OLO_VULKAN
// #691 Phase 8 (ADR 0011 §5, amendment (76)): two-stream vertex pull. Stream 0
// (binding 57) = shared unit quad {vec2 corner}; stream 1 (binding 63) = the
// per-chunk instance VB {uint geometry, uint material}. See
// Terrain_VoxelGreedy.glsl for the full note.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
layout(std430, binding = 63) readonly buffer OloInstancePull
{
    uint v[];
} b_Instances;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec2 a_Corner;
layout(location = 1) in int  a_QuadGeometry;
layout(location = 2) in int  a_QuadMaterial;
#endif

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

#define OLO_INSTANCE_SINGLE 1
#include "include/InstanceBlock_Vertex.glsl"

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) flat out uint v_Material;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int cornerBase = gl_VertexIndex * 2;
    vec2 a_Corner = vec2(b_Vertices.v[cornerBase + 0], b_Vertices.v[cornerBase + 1]);
    int instanceBase = gl_InstanceIndex * 2;
    uint geometryWord = b_Instances.v[instanceBase + 0];
    uint materialWord = b_Instances.v[instanceBase + 1];
#else
    uint geometryWord = uint(a_QuadGeometry);
    uint materialWord = uint(a_QuadMaterial);
#endif
    OLO_INSTANCE_FORWARD();

    OloVoxelQuad quad = oloUnpackVoxelQuad(geometryWord, materialWord);
    vec3 localPos = oloVoxelQuadCorner(quad, a_Corner);

    vec4 worldPos = u_Model * vec4(localPos, 1.0);
    v_WorldPos = worldPos.xyz;
    v_Normal = normalize(mat3(u_Normal) * quad.Normal);
    v_Material = quad.Material;
    gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

#include "include/InstanceBlock.glsl"
#include "include/VoxelQuadUnpack.glsl"

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

layout(std140, binding = 10) uniform TerrainParams {
    vec4 u_WorldSizeAndHeightScale;
    vec4 u_TerrainParams;
    int u_HeightmapResolution;
    int u_TerrainGpuDriven;
    int u_TerrainGpuGridRes;
    int _terrainPad2;
    vec4 u_TessFactors;
    vec4 u_TessFactors2;
    vec4 u_LayerTilingScales0;
    vec4 u_LayerTilingScales1;
    vec4 u_LayerBlendSharpness0;
    vec4 u_LayerBlendSharpness1;
};

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_TerrainAlbedoArray OLO_HEAP_TEX_2D_ARRAY(25)  // TEX_TERRAIN_ALBEDO_ARRAY
#define u_TerrainNormalArray OLO_HEAP_TEX_2D_ARRAY(26)  // TEX_TERRAIN_NORMAL_ARRAY
#define u_TerrainARMArray OLO_HEAP_TEX_2D_ARRAY(27)  // TEX_TERRAIN_ARM_ARRAY
#else
layout(binding = 25) uniform sampler2DArray u_TerrainAlbedoArray;
layout(binding = 26) uniform sampler2DArray u_TerrainNormalArray;
layout(binding = 27) uniform sampler2DArray u_TerrainARMArray;
#endif

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) flat in uint v_Material;

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

float voxelLayerIndex()
{
    return float(min(v_Material, 7u));
}

float voxelLayerTiling()
{
    uint layer = min(v_Material, 7u);
    float tiling = (layer < 4u) ? u_LayerTilingScales0[layer] : u_LayerTilingScales1[layer - 4u];
    return (tiling < 0.001) ? 0.1 : tiling;
}

void main()
{
    vec3 N = normalize(v_Normal);
    float triplanarSharpness = max(u_TerrainParams.w, 4.0);

    vec3 absNormal = abs(N);
    vec3 triWeights = pow(absNormal, vec3(triplanarSharpness));
    triWeights /= (triWeights.x + triWeights.y + triWeights.z + 0.0001);

    float tiling = voxelLayerTiling();
    float layer = voxelLayerIndex();

    vec3 wpTri = v_WorldPos + u_RenderOrigin; // camera-relative (issue #429)
    vec4 albedoX = texture(u_TerrainAlbedoArray, vec3(wpTri.yz * tiling, layer));
    vec4 albedoY = texture(u_TerrainAlbedoArray, vec3(wpTri.xz * tiling, layer));
    vec4 albedoZ = texture(u_TerrainAlbedoArray, vec3(wpTri.xy * tiling, layer));
    vec3 albedo = albedoX.rgb * triWeights.x
                + albedoY.rgb * triWeights.y
                + albedoZ.rgb * triWeights.z;

    vec4 armX = texture(u_TerrainARMArray, vec3(wpTri.yz * tiling, layer));
    vec4 armY = texture(u_TerrainARMArray, vec3(wpTri.xz * tiling, layer));
    vec4 armZ = texture(u_TerrainARMArray, vec3(wpTri.xy * tiling, layer));
    vec4 arm = armX * triWeights.x + armY * triWeights.y + armZ * triWeights.z;
    float ao = arm.r;
    float roughness = arm.g;
    float metallic = arm.b;

    // Blend the normal map in 0..1 texel space and decode AFTERWARDS, so an
    // unbound / empty terrain normal array is detectable.
    //
    // Decoding first is a trap: an unbound sampler reads solid black, and
    // `black * 2 - 1` is (-1,-1,-1) — a unit-length vector indistinguishable
    // from a legitimate perturbation. Feeding that through the TBN rotates the
    // face normal ~55 degrees off true, so most faces end up pointing away from
    // the sun and the whole mesh renders black while the geometry, the
    // materials and the lighting are all provably fine. A cubic voxel face
    // normal is EXACT and analytic; perturbing it with garbage is strictly
    // worse than leaving it alone.
    vec3 packedNormal = texture(u_TerrainNormalArray, vec3(wpTri.yz * tiling, layer)).rgb * triWeights.x
                      + texture(u_TerrainNormalArray, vec3(wpTri.xz * tiling, layer)).rgb * triWeights.y
                      + texture(u_TerrainNormalArray, vec3(wpTri.xy * tiling, layer)).rgb * triWeights.z;

    if (dot(packedNormal, packedNormal) > 1e-5)
    {
        vec3 triNormal = normalize(packedNormal * 2.0 - 1.0);

        // Pick the reference axis BEFORE crossing, rather than crossing and
        // testing the result.
        //
        // N here is an EXACT axis-aligned face normal, so cross(N, (0,0,1)) is
        // the zero vector for every +/-Z quad - a third of the mesh - and
        // normalize(0) is NaN. The usual `if (length(T) < 0.001)` rescue cannot
        // fire on that, because every comparison against NaN is false, so the
        // fallback axis is dead code and the NaN propagates into the lit normal
        // (and, in the deferred variant, into the G-Buffer). Terrain_Voxel.glsl
        // gets away with the same lines only because an exactly-axis-aligned
        // normal is measure-zero on a marching-cubes isosurface; on cubic
        // voxels it is the common case.
        vec3 reference = (abs(N.z) < 0.99) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
        vec3 T = normalize(cross(N, reference));
        vec3 B = cross(N, T);
        N = normalize(mat3(T, B, N) * triNormal);
    }

    if (albedo == vec3(0.0))
    {
        albedo = oloVoxelFallbackAlbedo(v_Material);
        roughness = 0.9;
        metallic = 0.0;
        ao = 1.0;
    }

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    o_GBufferEmissive = vec4(0.0, 0.0, 0.0, 0.0);
    o_GBufferVelocity = vec2(0.0);
    o_GBufferEntityID = u_EntityID;
}
