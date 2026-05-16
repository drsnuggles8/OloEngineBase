// =============================================================================
// PBR_GBuffer.glsl - Deferred G-Buffer write shader (Phase 2)
// Part of OloEngine Deferred Renderer
//
// Writes metallic-roughness PBR surface parameters into a 4-RT G-Buffer.
// Lighting, IBL, shadow sampling and snow overlays are deferred to
// DeferredLightingPass (Phase 3).
//
// G-Buffer attachment layout (matches GBuffer.h):
//   RT0 (RGBA8)   — Albedo.rgb  + Metallic(A)
//   RT1 (RGBA16F) — OctNormal.xy + Roughness(z) + AO(w)
//   RT2 (RGBA16F) — Emissive.rgb + MaterialFlags(A)
//   RT3 (RG16F)   — Screen-space velocity (curr.xy - prev.xy)
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

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
    mat4 u_PrevModel;
};

// MotionBlur UBO (binding 8) — reused for previous-frame ViewProjection so we
// can compute screen-space velocity. PrevViewProjection equals
// ViewProjection on the first frame so velocity is zero on static geometry.
layout(std140, binding = 8) uniform MotionBlurMatrices {
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec4 v_ClipPosCurr;
layout(location = 4) out vec4 v_ClipPosPrev;

void main()
{
    v_WorldPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_Normal = mat3(u_Normal) * a_Normal;
    v_TexCoord = a_TexCoord;

    v_ClipPosCurr = u_ViewProjection * vec4(v_WorldPos, 1.0);
    // Per-entity previous-frame transform (u_PrevModel) plus the previous
    // view-projection lets DeferredLightingPass reconstruct full screen-space
    // velocity including object motion. Renderer3D caches prev transforms per
    // entity ID; the first frame copies current→prev so velocity reads zero
    // for newly-spawned geometry.
    vec4 prevWorldPos = u_PrevModel * vec4(a_Position, 1.0);
    v_ClipPosPrev = u_PrevViewProjection * prevWorldPos;

    gl_Position = v_ClipPosCurr;
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"

// PBR Material UBO (binding 2) — identical layout to PBR_MultiLight so the
// same PODMaterialData works for both shader paths without reconversion.
layout(std140, binding = 2) uniform PBRMaterialProperties {
    vec4 u_BaseColorFactor;
    vec4 u_EmissiveFactor;
    float u_MetallicFactor;
    float u_RoughnessFactor;
    float u_NormalScale;
    float u_OcclusionStrength;
    int u_UseAlbedoMap;
    int u_UseNormalMap;
    int u_UseMetallicRoughnessMap;
    int u_UseAOMap;
    int u_UseEmissiveMap;
    int u_EnableIBL;
    int u_ApplyGammaCorrection;
    int u_AlphaCutoff;
    int u_EnableLightProbes;
    float u_IBLIntensity;
    int _pbrPad1;
    int _pbrPad2;
};

// Model UBO (binding 3) — entity-ID is not written from the G-Buffer path
// (picking remains a Forward-path responsibility). Block re-declared here
// identical to the vertex stage so GLSL/SPIR-V link validation accepts it;
// u_PrevModel goes unused by the fragment stage but must be present to keep
// block signatures matched across stages.
layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
    mat4 u_PrevModel;
};

// Texture bindings — must match PBR_MultiLight so material data works unchanged.
layout(binding = 0) uniform sampler2D u_AlbedoMap;
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap;
layout(binding = 2) uniform sampler2D u_NormalMap;
layout(binding = 4) uniform sampler2D u_AOMap;
layout(binding = 5) uniform sampler2D u_EmissiveMap;

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec4 v_ClipPosCurr;
layout(location = 4) in vec4 v_ClipPosPrev;

layout(location = 0) out vec4 o_GBufferAlbedo;    // RGBA8       albedo + metallic
layout(location = 1) out vec4 o_GBufferNormal;    // RGBA16F     octNormal + roughness + ao
layout(location = 2) out vec4 o_GBufferEmissive;  // RGBA16F     emissive + flags
layout(location = 3) out vec2 o_GBufferVelocity;  // RG16F       screen-space velocity
layout(location = 4) out int  o_GBufferEntityID;  // RED_INTEGER picking entity ID (blitted to SceneColor RT1 by DeferredLightingPass)

// Octahedral encode: unit normal -> [-1,1]^2.
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
    vec3 albedo = sampleAlbedo(u_AlbedoMap, v_TexCoord, u_BaseColorFactor.rgb, bool(u_UseAlbedoMap));
    vec2 metallicRoughness = sampleMetallicRoughness(u_MetallicRoughnessMap, v_TexCoord,
                                                     u_MetallicFactor, u_RoughnessFactor,
                                                     bool(u_UseMetallicRoughnessMap));
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;

    float ao = sampleAO(u_AOMap, v_TexCoord, u_OcclusionStrength, bool(u_UseAOMap));
    vec3 emissive = sampleEmissive(u_EmissiveMap, v_TexCoord, u_EmissiveFactor.rgb, bool(u_UseEmissiveMap));

    vec3 N = normalize(v_Normal);
    if (u_UseNormalMap == 1)
    {
        N = getNormalFromMap(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale);
    }

    // Screen-space velocity in [-1,1] NDC units.
    vec2 ndcCurr = v_ClipPosCurr.xy / max(v_ClipPosCurr.w, 1e-6);
    vec2 ndcPrev = v_ClipPosPrev.xy / max(v_ClipPosPrev.w, 1e-6);
    vec2 velocity = (ndcCurr - ndcPrev) * 0.5; // convert [-2,2] -> [-1,1]

    // Alpha cutoff — match Forward path behaviour for foliage-style masking.
    if (u_AlphaCutoff != 0 && u_BaseColorFactor.a < 0.5)
        discard;

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    o_GBufferEmissive = vec4(emissive, 0.0); // flags reserved for Phase 3/4
    o_GBufferVelocity = velocity;
    o_GBufferEntityID = u_EntityID;
}
