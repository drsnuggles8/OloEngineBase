// =============================================================================
// Terrain_Voxel_GBuffer.glsl - Deferred G-Buffer variant of Terrain_Voxel.glsl.
//
// Voxel-generated mesh (marching cubes) rendered into the 4-RT G-Buffer with
// triplanar-projected material data (albedo, normal, AO, roughness, metallic).
// Writes `emissive.a = 0.0` (lit flag) so `ComputeDeferredLit` evaluates full
// PBR + shadows + IBL on the G-Buffer data. Terrain is assumed static —
// velocity is zero.
//
// Selected by `Renderer3D::DrawVoxelTerrain` when the deferred path is active.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

#include "include/InstanceBlock_Vertex.glsl"

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;

void main()
{
    OLO_INSTANCE_FORWARD();
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_WorldPos = worldPos.xyz;
    v_Normal = normalize(mat3(u_Normal) * a_Normal);
    gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

// Mirror the vertex-stage ModelMatrices block so u_EntityID is available
// for the location=4 picking write. SPIR-V link validation requires the
// padding fields to match the vertex declaration exactly.
#include "include/InstanceBlock.glsl"

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

layout(binding = 25) uniform sampler2DArray u_TerrainAlbedoArray;
layout(binding = 26) uniform sampler2DArray u_TerrainNormalArray;
layout(binding = 27) uniform sampler2DArray u_TerrainARMArray;

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;

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

void main()
{
    vec3 N = normalize(v_Normal);
    float triplanarSharpness = max(u_TerrainParams.w, 4.0);

    vec3 absNormal = abs(N);
    vec3 triWeights = pow(absNormal, vec3(triplanarSharpness));
    triWeights /= (triWeights.x + triWeights.y + triWeights.z + 0.0001);

    float tiling = u_LayerTilingScales0[0];
    if (tiling < 0.001)
        tiling = 0.1;

    vec4 albedoX = texture(u_TerrainAlbedoArray, vec3(v_WorldPos.yz * tiling, 0.0));
    vec4 albedoY = texture(u_TerrainAlbedoArray, vec3(v_WorldPos.xz * tiling, 0.0));
    vec4 albedoZ = texture(u_TerrainAlbedoArray, vec3(v_WorldPos.xy * tiling, 0.0));
    vec3 albedo = albedoX.rgb * triWeights.x
                + albedoY.rgb * triWeights.y
                + albedoZ.rgb * triWeights.z;

    vec4 armX = texture(u_TerrainARMArray, vec3(v_WorldPos.yz * tiling, 0.0));
    vec4 armY = texture(u_TerrainARMArray, vec3(v_WorldPos.xz * tiling, 0.0));
    vec4 armZ = texture(u_TerrainARMArray, vec3(v_WorldPos.xy * tiling, 0.0));
    vec4 arm = armX * triWeights.x + armY * triWeights.y + armZ * triWeights.z;
    float ao = arm.r;
    float roughness = arm.g;
    float metallic = arm.b;

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

    // Procedural fallback when texture arrays are unbound (black albedo).
    if (albedo == vec3(0.0))
    {
        albedo = vec3(0.35, 0.32, 0.28); // stone colour
        roughness = 0.9;
        metallic = 0.0;
        ao = 1.0;
    }

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    // emissive.a = 0.0 → lit (full PBR evaluated by DeferredLightingPass).
    o_GBufferEmissive = vec4(0.0, 0.0, 0.0, 0.0);
    // Static terrain → zero velocity.
    o_GBufferVelocity = vec2(0.0);
    o_GBufferEntityID = u_EntityID;
}
