// OLO_NORMAL_MAP_TBN_EXEMPT: terrain is not an imported-mesh material. Its normals come from a
// splat/triplanar blend of a sampler2DArray over an ANALYTIC tangent frame (here: the exact
// axis-aligned face normal of a cubic voxel), not from screen-space UV derivatives. PBRCommon's
// derivative TBN neither applies nor is shared with it. See RenderPathDrift.
// =============================================================================
// Terrain_VoxelGreedy.glsl - Packed-quad voxel PBR shader (issue #727)
//
// Instanced sibling of Terrain_Voxel.glsl. Where that shader draws a marching-
// cubes triangle soup, this one draws ONE shared unit quad instanced once per
// merged greedy quad and rebuilds the corners here from an 8-byte per-instance
// record. Geometry decode lives in include/VoxelQuadUnpack.glsl, which mirrors
// Terrain/Voxel/VoxelQuad.h.
// =============================================================================

#type vertex
#version 460 core

#include "include/VoxelQuadUnpack.glsl"

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, amendment (76)): two-stream vertex pull, same
// shape as Foliage_Instance.glsl. Stream 0 on binding 57 is the 8-byte shared
// unit quad {vec2 corner}; stream 1 on binding 63 is the per-chunk 8-byte
// instance VB {uint geometry, uint material}, indexed by gl_InstanceIndex.
// Declared as uint[] rather than float[] because the instance stream is
// integer data — the pull binding only carries a device address, so the
// element type is the shader's to choose. The GL attribute branch is untouched.
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
layout(location = 0) in vec2 a_Corner;       // shared unit quad, (u, v) in [0,1]
layout(location = 1) in int  a_QuadGeometry; // packed geometry word (28 bits used, always >= 0)
layout(location = 2) in int  a_QuadMaterial; // packed material word
#endif

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
};

// Model UBO (binding 3 → InstanceBuffer SSBO). gl_InstanceIndex here is the
// QUAD index, not an InstanceData index — the chunk uploads exactly one
// InstanceData entry (its terrain transform * chunk placement), so indexing
// InstanceData by it would read past a 224-byte upload. Same contract, and the
// same past device-loss, as foliage.
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
    // Chunk-local VOXEL units — the chunk origin and voxel size ride u_Model,
    // which is what keeps the per-quad record down to 32 bits.
    vec3 localPos = oloVoxelQuadCorner(quad, a_Corner);

    vec4 worldPos = u_Model * vec4(localPos, 1.0);
    v_WorldPos = worldPos.xyz;
    v_Normal = normalize(mat3(u_Normal) * quad.Normal);
    v_Material = quad.Material;
    gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"
#include "include/AtmosphereShading.glsl"
#include "include/VoxelQuadUnpack.glsl"

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
};

// Multi-Light UBO (binding 5)
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
    mat4 u_AtlasEntryMatrices[48];
    vec4 u_AtlasEntryScaleOffset[48];
    int u_DirectionalShadowEnabled;
    int u_AtlasEntryCount;
    int u_ShadowMapResolution;
    int u_AtlasResolution;
    int u_CascadeDebugEnabled;
    int u_SoftShadowMode;
    int _shadowPad1;
    int _shadowPad2;
};

// Model UBO (binding 3)
#include "include/InstanceBlock.glsl"

// Terrain UBO (binding 10)
#include "include/TerrainParamsBlock.glsl"

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_ShadowMapCSM OLO_HEAP_TEX_2D_ARRAY_SHADOW(8)  // TEX_SHADOW
#define u_ShadowAtlas OLO_HEAP_TEX_2D_ARRAY_SHADOW(13)  // TEX_SHADOW_ATLAS
#else
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM;
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowAtlas;
#endif
#ifdef OLO_BINDLESS
#define u_ShadowMapCSMRaw OLO_HEAP_TEX_2D_ARRAY(33)  // TEX_SHADOW_CSM_RAW
#define u_ShadowAtlasRaw OLO_HEAP_TEX_2D_ARRAY(34)  // TEX_SHADOW_ATLAS_RAW
#else
layout(binding = 33) uniform sampler2DArray u_ShadowMapCSMRaw;
layout(binding = 34) uniform sampler2DArray u_ShadowAtlasRaw;
#endif

#ifdef OLO_BINDLESS
#define u_IrradianceMap OLO_HEAP_TEX_CUBE(10)  // TEX_USER_0
#define u_PrefilterMap OLO_HEAP_TEX_CUBE(11)  // TEX_USER_1
#define u_BRDFLutMap OLO_HEAP_TEX_2D(12)  // TEX_USER_2
#else
layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D u_BRDFLutMap;
#endif

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

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
layout(location = 3) out vec2 o_Velocity;

vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// The material index doubles as the terrain texture-array layer. Clamped to the
// 8 layers the TerrainParams tiling vectors describe, so a material index a
// scene invented cannot sample outside the array.
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

    // Camera-relative (issue #429): triplanar tiling is world-anchored.
    vec3 wpTri = v_WorldPos + u_RenderOrigin;
    vec4 albedoX = texture(u_TerrainAlbedoArray, vec3(wpTri.yz * tiling, layer));
    vec4 albedoY = texture(u_TerrainAlbedoArray, vec3(wpTri.xz * tiling, layer));
    vec4 albedoZ = texture(u_TerrainAlbedoArray, vec3(wpTri.xy * tiling, layer));
    vec3 albedo = (albedoX.rgb * triWeights.x + albedoY.rgb * triWeights.y + albedoZ.rgb * triWeights.z);

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

    // No texture arrays bound → the per-material fallback palette, so a scene
    // with no terrain layer textures still shows its strata.
    if (albedo == vec3(0.0))
    {
        albedo = oloVoxelFallbackAlbedo(v_Material);
        roughness = 0.9;
        metallic = 0.0;
        ao = 1.0;
    }

    vec3 V = normalize(u_CameraPosition - v_WorldPos);

    atmosphereApplyWetness(albedo, roughness, N);
    float cloudShadow = atmosphereCloudShadow(v_WorldPos);

    vec3 Lo = vec3(0.0);
    for (int i = 0; i < min(u_LightCount, MAX_LIGHTS); ++i)
    {
        vec3 lightContrib = calculateLightContribution(u_Lights[i], N, V, albedo, metallic, roughness, v_WorldPos);

        int lightType = int(u_Lights[i].position.w);
        if (lightType == DIRECTIONAL_LIGHT)
        {
            lightContrib *= cloudShadow;
        }
        if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
            vec4 viewSpacePos = u_View * vec4(v_WorldPos, 1.0);
            float viewDepth = viewSpacePos.z;

            float shadow = calculateCascadedShadowFactorCSM(
                u_ShadowMapCSM,
                u_ShadowMapCSMRaw,
                v_WorldPos,
                viewDepth,
                u_DirectionalLightSpaceMatrices,
                u_CascadePlaneDistances,
                u_ShadowParams,
                u_ShadowMapResolution,
                u_SoftShadowMode
            );
            lightContrib *= shadow;
        }
        else if (lightType == SPOT_LIGHT)
        {
            int atlasEntry = int(u_Lights[i].direction.w);
            if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
            {
                float shadow = calculateAtlasEntryShadow(
                    v_WorldPos,
                    u_AtlasEntryMatrices[atlasEntry],
                    u_AtlasEntryScaleOffset[atlasEntry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_ShadowParams.x,
                    u_AtlasResolution,
                    u_SoftShadowMode,
                    u_ShadowParams.z
                );
                lightContrib *= shadow;
            }
        }
        else if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
        {
            int baseEntry = int(u_Lights[i].direction.w);
            if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
            {
                vec3 lightPos = u_Lights[i].position.xyz;
                int entry = baseEntry + atlasCubeFace(v_WorldPos - lightPos);
                float shadow = calculateAtlasEntryShadow(
                    v_WorldPos,
                    u_AtlasEntryMatrices[entry],
                    u_AtlasEntryScaleOffset[entry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_ShadowParams.x,
                    u_AtlasResolution,
                    0,
                    u_ShadowParams.z
                );
                lightContrib *= shadow;
            }
        }

        Lo += lightContrib;
    }

    vec3 ambient = calculateSimpleAmbient(albedo, metallic, ao);
    vec3 color = ambient + Lo;
    color = mix(color, color * ao, 0.5);

    o_Color = vec4(color, 1.0);
    o_EntityID = u_EntityID;

    vec3 viewNormal = normalize(mat3(u_View) * N);
    o_ViewNormal = octEncode(viewNormal);

    vec4 clipCurr = u_ViewProjection     * vec4(v_WorldPos, 1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_WorldPos, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
