// =============================================================================
// DepthPrepass_MaskSkinned.glsl - Depth-only prepass for alpha-MASK skinned meshes
//
// Combines DepthPrepass_Skinned.glsl's invariant skinned position path with
// DepthPrepass_Mask.glsl's glTF MASK alpha test. See both for the invariance
// and coverage-matching contracts.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;     // unused, but present in the skinned vertex layout
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in ivec4 a_BoneIDs;
layout(location = 4) in vec4 a_BoneWeights;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Instance transforms SSBO (binding 15)
#include "include/InstanceBlock_Vertex.glsl"

// Bone Matrices UBO (binding 4)
layout(std140, binding = 4) uniform BoneMatrices {
    mat4 u_BoneTransforms[100];
};

layout(location = 2) out vec2 v_TexCoord;

invariant gl_Position;

void main()
{
    OLO_INSTANCE_FORWARD();
    v_TexCoord = a_TexCoord;

    // Bone accumulation mirrors PBR_MultiLight_Skinned / PBR_GBuffer_Skinned.
    mat4 boneTransform = mat4(0.0);
    float totalWeight = a_BoneWeights.x + a_BoneWeights.y + a_BoneWeights.z + a_BoneWeights.w;
    if (totalWeight > 0.001)
    {
        for (int i = 0; i < 4; ++i)
        {
            int boneID = a_BoneIDs[i];
            if (boneID >= 0 && boneID < 100)
            {
                boneTransform += u_BoneTransforms[boneID] * a_BoneWeights[i];
            }
        }
    }
    else
    {
        // Vertex has no bone influence — pass through without skinning
        boneTransform = mat4(1.0);
    }

    vec4 localPosition = boneTransform * vec4(a_Position, 1.0);
    vec3 worldPos = vec3(u_Model * localPosition);
    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
}

#type fragment
#version 460 core

layout(location = 2) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_AlbedoMap;          // TEX_DIFFUSE

// Overdraw counter — see DepthPrepass.glsl. Written only for fragments that
// survive the MASK alpha test below, so the count matches shaded coverage.
layout(location = 0) out vec4 o_OverdrawCount;

// PBR Material UBO (binding 2) — full block layout must match PBR_MultiLight.glsl
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
};

void main()
{
    // glTF MASK alpha test — identical to PBR_MultiLight.glsl so the prepass
    // depth coverage matches the color pass texel-for-texel.
    if (u_AlphaMode == 1)
    {
        float sampledAlpha = u_BaseColorFactor.a;
        if (u_UseAlbedoMap == 1)
            sampledAlpha *= texture(u_AlbedoMap, v_TexCoord).a;
        if (sampledAlpha < u_AlphaCutoff)
            discard;
    }

    o_OverdrawCount = vec4(1.0);
}
