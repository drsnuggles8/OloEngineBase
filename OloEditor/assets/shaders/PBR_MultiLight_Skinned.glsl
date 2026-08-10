// =============================================================================
// PBR_MultiLight_Skinned.glsl - PBR Shader with Multi-Light Support for Skinned Meshes
// Part of OloEngine Enhanced PBR System
// Supports skeletal animation with bone matrices and multi-light PBR rendering
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5, decision A3): the SKINNED two-stream vertex pull.
// The engine `Vertex` stream (V1: 32 B -- vec3 position @0, vec3 normal @12,
// vec2 uv @24) arrives on the engine-wide vertex-pull binding 57. The BONE
// stream (V2: 32 B -- uvec4 BoneIDs @0, vec4 Weights @16) is a SECOND vertex
// buffer on the same VAO (MeshSource::BuildBoneInfluenceBuffer) and arrives on
// the reserved second pull binding 63, which AssembleAndPushRootData maps to
// VAO stream 1. Both strides are 8 floats. Bone IDs are u32 in memory and the
// pull view is float[], so they come back through floatBitsToInt -- exactly the
// reinterpretation GL performs for the `Int4` bone-id attribute. Pulled locals
// below main() carry the ATTRIBUTE NAMES, so the body is identical on both
// routes; the GL attribute branch is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
layout(std430, binding = 63) readonly buffer OloBonePull
{
    float v[];
} b_Bones;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in ivec4 a_BoneIDs;
layout(location = 4) in vec4 a_BoneWeights;
#endif

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Previous-frame view-projection for forward-path TAA velocity
    // emission. See PBR_MultiLight.glsl for the full design note.
    mat4 u_PrevViewProjection;
};

// Model UBO (binding 3)
#include "include/InstanceBlock_Vertex.glsl"

// Bone Matrices UBO (binding 4)
layout(std140, binding = 4) uniform BoneMatrices {
    mat4 u_BoneTransforms[100];
};

// Previous-frame bone matrices for per-bone velocity. CommandDispatch always
// populates this UBO — when the caller has no actual prev pose it aliases
// the current palette here, so a read returns either the real previous pose
// or the current pose (zero bone motion) and never undefined memory.
layout(std140, binding = 31) uniform PrevBoneMatrices {
    mat4 u_PrevBoneTransforms[100];
};

// Output to fragment shader
layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec4 v_ClipPosCurr;
layout(location = 4) out vec4 v_ClipPosPrev;

// Depth-prepass contract: the color pass re-tests at GL_LEQUAL against depth
// written by DepthPrepass_Skinned/_MaskSkinned.glsl, which replicate this exact
// position math. `invariant` forbids per-program rounding differences.
invariant gl_Position;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec3 a_Normal = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
    int boneBase = gl_VertexIndex * 8;
    ivec4 a_BoneIDs = ivec4(floatBitsToInt(b_Bones.v[boneBase + 0]), floatBitsToInt(b_Bones.v[boneBase + 1]),
                          floatBitsToInt(b_Bones.v[boneBase + 2]), floatBitsToInt(b_Bones.v[boneBase + 3]));
    vec4 a_BoneWeights = vec4(b_Bones.v[boneBase + 4], b_Bones.v[boneBase + 5], b_Bones.v[boneBase + 6], b_Bones.v[boneBase + 7]);
#endif
    OLO_INSTANCE_FORWARD();
    // Calculate bone transformation
    mat4 boneTransform = mat4(0.0);
    mat4 prevBoneTransform = mat4(0.0);
    float totalWeight = a_BoneWeights.x + a_BoneWeights.y + a_BoneWeights.z + a_BoneWeights.w;
    if (totalWeight > 0.001)
    {
        for (int i = 0; i < 4; ++i)
        {
            int boneID = a_BoneIDs[i];
            if (boneID >= 0 && boneID < 100)
            {
                boneTransform     += u_BoneTransforms[boneID]     * a_BoneWeights[i];
                prevBoneTransform += u_PrevBoneTransforms[boneID] * a_BoneWeights[i];
            }
        }
    }
    else
    {
        // Vertex has no bone influence — pass through without skinning
        boneTransform = mat4(1.0);
        prevBoneTransform = mat4(1.0);
    }

    // Transform position and normal by bones
    vec4 localPosition = boneTransform * vec4(a_Position, 1.0);
    vec3 localNormal = mat3(boneTransform) * a_Normal;
    vec4 prevLocalPosition = prevBoneTransform * vec4(a_Position, 1.0);

    // Transform to world space
    v_WorldPos = vec3(u_Model * localPosition);
    v_Normal = mat3(u_Normal) * localNormal;
    v_TexCoord = a_TexCoord;

    vec4 clipCurr = u_ViewProjection * vec4(v_WorldPos, 1.0);
    // Combine per-bone previous pose with u_PrevModel so the emitted motion
    // vector captures both rigid entity motion and intra-skeleton bone
    // deltas — matching what PBR_GBuffer_Skinned.glsl does in Deferred.
    vec4 prevWorldPos = u_PrevModel * prevLocalPosition;
    vec4 clipPrev = u_PrevViewProjection * prevWorldPos;

    v_ClipPosCurr = clipCurr;
    v_ClipPosPrev = clipPrev;

    gl_Position = clipCurr;
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"
#include "include/SnowCommon.glsl"
#include "include/AtmosphereShading.glsl"

// Camera UBO (binding 0) - for view position
// Fragment-side CameraMatrices must declare the same block layout as the
// vertex stage — glLinkProgram() rejects per-program UBO blocks whose
// members disagree between stages. The trailing u_PrevViewProjection is
// unused in fragment but its declaration keeps the block types identical.
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
};

// Multi-Light UBO (binding 5)
layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int u_MaxLights;
    int u_ShadowCasterCount;
    int u_DirectionalLightCount;
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
    float u_AlphaCutoff;        // Alpha cutoff for MASK mode
    int u_EnableLightProbes;    // Enable light probe indirect diffuse
    float u_IBLIntensity;       // Runtime IBL strength multiplier
    int u_AlphaMode;            // 0=Opaque, 1=Mask, 2=Blend
    int _pbrPad2;
    // Per-material heap offsets (issue #691 Phase 3). MUST mirror
    // PBRMaterialUBO::HeapOffsets — std140 shifts every later field if the two
    // layouts disagree, and this block is the LAST member so a missing
    // declaration reads garbage rather than failing to link.
    //   [0] albedo, metallicRoughness, normal, ao
    //   [1] emissive, environment, irradiance, prefilter
    //   [2] brdfLut, diffuse(legacy), specular(legacy), unused
    uvec4 u_MaterialHeapOffsets[3];
};

// Snow UBO (binding 13)
layout(std140, binding = 13) uniform SnowParams {
    vec4 u_SnowCoverageParams;
    vec4 u_SnowAlbedoAndRoughness;
    vec4 u_SnowSSSColorAndIntensity;
    vec4 u_SnowSparkleParams;
    vec4 u_SnowFlags;
};

// Texture bindings following ShaderBindingLayout.
//
// CONVERTED WHOLE, identical split to PBR_MultiLight (§5c): the five
// material-local maps ride the per-material UBO lanes, the environment probe,
// IBL trio and shadow arrays come from the shared published table. Skinning
// changes only the vertex stage, so the fragment-side binding story is the same.
#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define OLO_MATERIAL_HEAP_READER 1
#define u_AlbedoMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET)
#define u_MetallicRoughnessMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_METALLIC_ROUGHNESS_OFFSET)
#define u_NormalMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_NORMAL_OFFSET)
#define u_AOMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_AO_OFFSET)
#define u_EmissiveMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_EMISSIVE_OFFSET)

#define u_EnvironmentMap OLO_HEAP_TEX_CUBE(9)
#define u_IrradianceMap OLO_HEAP_TEX_CUBE(10)
#define u_PrefilterMap OLO_HEAP_TEX_CUBE(11)
#define u_BRDFLutMap OLO_HEAP_TEX_2D(12)

#define u_ShadowMapCSM OLO_HEAP_TEX_2D_ARRAY_SHADOW(8)
#define u_ShadowAtlas OLO_HEAP_TEX_2D_ARRAY_SHADOW(13)
#define u_ShadowMapCSMRaw OLO_HEAP_TEX_2D_ARRAY(33)
#define u_ShadowAtlasRaw OLO_HEAP_TEX_2D_ARRAY(34)
#else
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

// Shadow map textures — CSM array + the budgeted local-light shadow atlas
// (issue #435; the atlas replaced the spot array and the 4 point cubemaps).
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM; // TEX_SHADOW (CSM)
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowAtlas; // TEX_SHADOW_ATLAS (1-layer)
// Comparison-OFF raw-depth views of the textures above for the PCSS blocker search.
layout(binding = 33) uniform sampler2DArray u_ShadowMapCSMRaw; // TEX_SHADOW_CSM_RAW
layout(binding = 34) uniform sampler2DArray u_ShadowAtlasRaw;  // TEX_SHADOW_ATLAS_RAW
#endif

// Shadow UBO (binding 6)
layout(std140, binding = 6) uniform ShadowData {
    mat4 u_DirectionalLightSpaceMatrices[4];
    vec4 u_CascadePlaneDistances;
    vec4 u_ShadowParams;  // x=bias, y=normalBias, z=softness, w=maxShadowDistance
    mat4 u_AtlasEntryMatrices[48];    // light VP per shadow-atlas entry (spot = 1 entry, point = 6 face entries)
    vec4 u_AtlasEntryScaleOffset[48]; // xy = UV scale, zw = UV offset of the entry's atlas tile
    int u_DirectionalShadowEnabled;
    int u_AtlasEntryCount;
    int u_ShadowMapResolution;
    int u_AtlasResolution;
    int u_CascadeDebugEnabled;
    int u_SoftShadowMode;  // 0 = legacy hardware PCF, 1 = PCSS (contact-hardening)
    int _shadowPad1;
    int _shadowPad2;
};

// Clustered light lists (issue #435). Included AFTER the ShadowData block +
// atlas samplers above: with FPLUS_ATLAS_SHADOWS defined, the per-cluster
// light evaluator attenuates every culled light by its shadow-atlas entry.
#define FPLUS_ATLAS_SHADOWS 1
#include "include/ForwardPlusCommon.glsl"

// =============================================================================
// INPUT/OUTPUT
// =============================================================================

// Input from vertex shader
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec4 v_ClipPosCurr;
layout(location = 4) in vec4 v_ClipPosPrev;

// Output
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Forward-path TAA motion vector (matches PBR_MultiLight.glsl).
layout(location = 3) out vec2 o_Velocity;

// Octahedral encode: unit normal → RG16F [-1,1]²
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// Model UBO (binding 3) for entity ID access
// Fragment-side ModelMatrices must match the vertex stage's layout
// (which includes the trailing u_PrevModel). u_PrevModel is unused in
// fragment but the declaration keeps the two stages' block types
// identical so glLinkProgram() succeeds.
#include "include/InstanceBlock.glsl"

// =============================================================================
// MAIN FRAGMENT SHADER
// =============================================================================

void main()
{
    // glTF MASK alpha test (texture.a * baseColorFactor.a < cutoff).
    if (u_AlphaMode == 1)
    {
        float sampledAlpha = u_BaseColorFactor.a;
        if (u_UseAlbedoMap == 1)
            sampledAlpha *= texture(u_AlbedoMap, v_TexCoord).a;
        if (sampledAlpha < u_AlphaCutoff)
            discard;
    }

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

    // Weather response + cloud shadow (issue #633) — same order as
    // PBR_MultiLight.glsl: wetness before any lighting reads
    // albedo/roughness; cloud shadow applied per directional light below.
    atmosphereApplyWetness(albedo, roughness, N);
    float cloudShadow = atmosphereCloudShadow(v_WorldPos);

    // Calculate direct lighting from all lights
    vec3 Lo = vec3(0.0);

    // Forward+ path: use per-cluster culled light lists for point/spot lights
    bool fplusActive = (fplus_Params.z != 0u);
    if (fplusActive)
    {
        float fplusViewDepth = -(u_View * vec4(v_WorldPos, 1.0)).z;
        Lo += fplusEvaluateTileLights(N, V, v_WorldPos, albedo, metallic, roughness, fplusViewDepth);
    }

    // UBO light loop: when Forward+ is active, only evaluate directional lights
    // (stored at the start of the array). When Forward+ is off, evaluate all lights.
    int loopCount = fplusActive ? min(u_DirectionalLightCount, MAX_LIGHTS)
                                : min(u_LightCount, MAX_LIGHTS);
    for (int i = 0; i < loopCount; ++i)
    {
        int lightType = int(u_Lights[i].position.w);

        vec3 lightContrib = calculateLightContribution(u_Lights[i], N, V, albedo, metallic, roughness, v_WorldPos);
        if (lightType == DIRECTIONAL_LIGHT)
        {
            lightContrib *= cloudShadow;
        }
        if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
            // Compute view-space depth for cascade selection
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
        // Apply spot light shadows (atlas entry, issue #435)
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
        // Apply point light shadows (6 consecutive atlas cube-face entries)
        else if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
        {
            // Sphere area lights shadow from the emitter centre (the
            // representative point), so both types share the point path:
            // direction.w carries the BASE atlas entry of the 6 face tiles.
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
                    0, // PCF only on cube faces (matches the old cubemap path)
                    u_ShadowParams.z
                );
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
        ambient *= u_IBLIntensity;
    }
    else
    {
        ambient = calculateSimpleAmbient(albedo, metallic, ao);
    }

    // Combine lighting — AO attenuates ambient only
    vec3 color = ambient * ao + Lo + emissive;

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

    // Screen-space velocity - see PBR_MultiLight.glsl for the derivation.
    // Skinned meshes combine per-bone pose delta (PrevBoneMatrices, binding 31)
    // with u_PrevModel so both rigid motion and intra-skeleton animation
    // contribute to the motion vector.
    vec2 ndcCurr = v_ClipPosCurr.xy / v_ClipPosCurr.w;
    vec2 ndcPrev = v_ClipPosPrev.xy / v_ClipPosPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
