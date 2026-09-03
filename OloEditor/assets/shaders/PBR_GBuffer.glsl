// =============================================================================
// PBR_GBuffer.glsl - Deferred G-Buffer write shader
// Part of OloEngine Deferred Renderer
//
// Writes metallic-roughness PBR surface parameters into a 4-RT G-Buffer.
// Lighting, IBL, shadow sampling and snow overlays are deferred to
// DeferredLightingPass.
//
// G-Buffer attachment layout (matches GBuffer.h):
//   RT0 (RGBA8)   — Albedo.rgb  + Metallic(A)
//   RT1 (RGBA16F) — OctNormal.xy + Roughness(z) + AO(w)
//   RT2 (RGBA16F) — Emissive.rgb + MaterialFlags(A)
//   RT3 (RG16F)   — Screen-space velocity (curr.xy - prev.xy)
//   RT4 (R32I)    — Picking entity ID
//   RT5 (RGBA16F) — Baked lightmap irradiance E.rgb + coverage .a (issue #865)
//
// RT5 is why this shader carries UV2 at all. The deferred lighting pass shades
// from the G-Buffer and by then the fragment has neither UV2 nor instance
// identity, so the atlas fetch has to happen HERE — the last stage that still
// has both — and the result travels as irradiance, which is what the ambient
// ladder consumes on the forward path too. Everything downstream of this file
// treats RT5 exactly as PBR_MultiLight.glsl treats
// sampleLightmapIrradiance()'s return value.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V1 engine-vertex pull. On the Vulkan route the
// vertex data is READ, not fetched -- binding 57 is the engine-wide vertex-pull
// binding and the root struct carries this buffer's device address. The stream
// is the engine `Vertex` (32 B: vec3 position @0, vec3 normal @12, vec2 uv @24),
// so the per-vertex stride is 8 floats. Pulled locals below main() carry the
// ATTRIBUTE NAMES, which keeps the body identical on both routes; the GL
// attribute branch is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
// Lightmap UV2 pull (issue #866/#865) — same reservation and the same reasoning
// as PBR_MultiLight.glsl: bones and lightmap UV2 are mutually exclusive per mesh
// (MeshSource::Build fills VAO stream 1 with one or the other), and this is the
// non-skinned G-Buffer variant, so it never shares a VAO with
// PBR_GBuffer_Skinned.glsl's bone data.
layout(std430, binding = 63) readonly buffer OloLightmapUVPull
{
    vec2 v[];
} b_LightmapUV;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
// Lightmap UV2 (issue #439): a SEPARATE MeshSource stream at attribute 3
// (static meshes only — skinned VAOs keep bones at 3/4 and never carry it).
// Every static VAO exposes the attribute; unbaked meshes back it with the
// stride-0 constant stub, so the layout is identical across draws.
layout(location = 3) in vec2 a_TexCoord2;
#endif

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Model UBO (binding 3)
#include "include/InstanceBlock_Vertex.glsl"

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
layout(location = 5) out vec2 v_TexCoord2;

// Depth-prepass contract: the color pass re-tests at GL_LEQUAL against depth
// written by DepthPrepass*.glsl, which replicates this exact position math.
// `invariant` forbids optimizations that would round differently per program.
invariant gl_Position;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec3 a_Normal = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
    // Gate the pull on the SAME per-instance signal sampleLightmapIrradiance()
    // uses. An unbaked static mesh has no stream-1 buffer, so an unconditional
    // pull resolves to the frame arena's fixed-size null block and a real mesh's
    // vertex count runs off the end of it — a buffer-device-address read has no
    // bounds, so that is a device loss, not a clamped read (ADR 0011 amendment
    // (89)). LightmapScaleOffset.x > 0 is only ever published for a mesh whose
    // UV2 stream was actually built.
    vec2 a_TexCoord2 = vec2(0.0);
    if (instances[gl_InstanceIndex].LightmapScaleOffset.x > 0.0)
    {
        a_TexCoord2 = b_LightmapUV.v[gl_VertexIndex];
    }
#endif
    OLO_INSTANCE_FORWARD();
    v_WorldPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_Normal = mat3(u_Normal) * a_Normal;
    v_TexCoord = a_TexCoord;
    v_TexCoord2 = a_TexCoord2;

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

// FIRST — the sampler declarations below expand its accessors on the bindless
// build. Contributes nothing on the slot-based build.
#include "include/BindlessHeap.glsl"

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
    float u_AlphaCutoff;
    int u_EnableLightProbes;
    float u_IBLIntensity;
    int u_AlphaMode;        // 0=Opaque, 1=Mask, 2=Blend
    int u_PBRModel;             // PBRModel selector: 0=Legacy, 1=ClosureV2 (issue #975)
    // Per-material heap offsets (issue #691). MUST mirror
    // PBRMaterialUBO::HeapOffsets — std140 shifts every later field if the two
    // layouts disagree, and this block is the LAST member so a missing
    // declaration reads garbage rather than failing to link.
    //   [0] albedo, metallicRoughness, normal, ao
    //   [1] emissive, environment, irradiance, prefilter
    //   [2] brdfLut, diffuse(legacy), specular(legacy), unused
    uvec4 u_MaterialHeapOffsets[3];
};

// Model UBO (binding 3) — entity-ID is not written from the G-Buffer path
// (picking remains a Forward-path responsibility). Block re-declared here
// identical to the vertex stage so GLSL/SPIR-V link validation accepts it;
// u_PrevModel goes unused by the fragment stage but must be present to keep
// block signatures matched across stages.
#include "include/InstanceBlock.glsl"

// Canonical GPU Scene material record (issue #994). Included AFTER the
// instance block because the link it resolves travels in
// InstanceData::GPUSceneRef. This is the raster path that consumes the record:
// every material constant below comes from the registry's committed record
// when the draw carries a live link, and from the per-draw UBO otherwise.
//
// The textures still bind through the slot path — the record's heap offsets
// read OLO_GPU_SCENE_HEAP_OFFSET_UNRESOLVED wherever the descriptor heap is
// off (GL's default), and #805 has not landed — so the record supplies the
// FACTORS and the FLAGS, and the sampler set stays exactly as it was.
#include "include/GPUSceneMaterialResolve.glsl"

// Baked lightmap atlas (issue #439): UBO 1 + sampler 16. Included AFTER the
// instance block because the per-draw atlas region it needs travels in
// InstanceData::LightmapScaleOffset.
#include "include/LightmapSampling.glsl"

// Texture bindings — must match PBR_MultiLight so material data works unchanged.
//
// This shader declares ONLY the material five, so converting it is the whole
// job (§5c: the unit of conversion is a C++ bind and its declaration together).
// Everything else it reads — the G-Buffer targets it WRITES, the instance block
// — is not a sampler.
#ifdef OLO_BINDLESS
#define OLO_MATERIAL_HEAP_READER 1
#define u_AlbedoMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET)
#define u_MetallicRoughnessMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_METALLIC_ROUGHNESS_OFFSET)
#define u_NormalMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_NORMAL_OFFSET)
#define u_AOMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_AO_OFFSET)
#define u_EmissiveMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_EMISSIVE_OFFSET)
#else
layout(binding = 0) uniform sampler2D u_AlbedoMap;
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap;
layout(binding = 2) uniform sampler2D u_NormalMap;
layout(binding = 4) uniform sampler2D u_AOMap;
layout(binding = 5) uniform sampler2D u_EmissiveMap;
#endif

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec4 v_ClipPosCurr;
layout(location = 4) in vec4 v_ClipPosPrev;
layout(location = 5) in vec2 v_TexCoord2;

layout(location = 0) out vec4 o_GBufferAlbedo;    // RGBA8       albedo + metallic
layout(location = 1) out vec4 o_GBufferNormal;    // RGBA16F     octNormal + roughness + ao
layout(location = 2) out vec4 o_GBufferEmissive;  // RGBA16F     emissive + flags
layout(location = 3) out vec2 o_GBufferVelocity;  // RG16F       screen-space velocity
layout(location = 4) out int  o_GBufferEntityID;  // RED_INTEGER picking entity ID (blitted to SceneColor RT1 by DeferredLightingPass)
layout(location = 5) out vec4 o_GBufferBakedGI;  // RGBA16F     baked lightmap irradiance E + coverage (issue #865)

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
    // Start from the per-draw UBO, then let a live canonical record override
    // it. A real branch, not a ternary: SPIR-V's OpSelect evaluates BOTH
    // operands, so a ternary would read the record even on the unlinked path,
    // where oloGPUSceneMaterial leaves it unwritten. One source of locals
    // either way, so the two sources cannot disagree field by field.
    //
    // The record is byte-identical to what the UBO carries for the same
    // material — both are built from the same Material — which is why the
    // migration is expected to be pixel-identical rather than merely close.
    vec4  matBaseColorFactor   = u_BaseColorFactor;
    vec4  matEmissiveFactor    = u_EmissiveFactor;
    float matMetallicFactor    = u_MetallicFactor;
    float matRoughnessFactor   = u_RoughnessFactor;
    float matNormalScale       = u_NormalScale;
    float matOcclusionStrength = u_OcclusionStrength;
    float matAlphaCutoff       = u_AlphaCutoff;
    int   matAlphaMode         = u_AlphaMode;
    int   matPBRModel          = u_PBRModel;
    bool  matUseAlbedoMap      = bool(u_UseAlbedoMap);
    bool  matUseNormalMap      = bool(u_UseNormalMap);
    bool  matUseMRMap          = bool(u_UseMetallicRoughnessMap);
    bool  matUseAOMap          = bool(u_UseAOMap);
    bool  matUseEmissiveMap    = bool(u_UseEmissiveMap);

    GPUSceneMaterial gpuSceneMaterial;
    if (oloGPUSceneMaterial(instances[v_InstanceIndex].GPUSceneRef, gpuSceneMaterial))
    {
        matBaseColorFactor   = gpuSceneMaterial.BaseColorFactor;
        matEmissiveFactor    = gpuSceneMaterial.EmissiveFactor;
        matMetallicFactor    = gpuSceneMaterial.MetallicFactor;
        matRoughnessFactor   = gpuSceneMaterial.RoughnessFactor;
        matNormalScale       = gpuSceneMaterial.NormalScale;
        matOcclusionStrength = gpuSceneMaterial.OcclusionStrength;
        matAlphaCutoff       = gpuSceneMaterial.AlphaCutoff;
        matAlphaMode         = int(gpuSceneMaterial.AlphaMode);
        matPBRModel          = int(gpuSceneMaterial.ClosureVersion);
        // The record's *Map bits are set exactly when the texture handle is
        // valid, which is the same condition PBRMaterialUBO's Use*Map ints encode.
        matUseAlbedoMap      = (gpuSceneMaterial.Flags & OLO_GPU_SCENE_MATERIAL_ALBEDO_MAP) != 0u;
        matUseNormalMap      = (gpuSceneMaterial.Flags & OLO_GPU_SCENE_MATERIAL_NORMAL_MAP) != 0u;
        matUseMRMap          = (gpuSceneMaterial.Flags & OLO_GPU_SCENE_MATERIAL_METALLIC_ROUGHNESS_MAP) != 0u;
        matUseAOMap          = (gpuSceneMaterial.Flags & OLO_GPU_SCENE_MATERIAL_OCCLUSION_MAP) != 0u;
        matUseEmissiveMap    = (gpuSceneMaterial.Flags & OLO_GPU_SCENE_MATERIAL_EMISSIVE_MAP) != 0u;
    }

    // glTF MASK: discard before any other work when sampled alpha falls below cutoff.
    // Per glTF 2.0 spec, the sampled alpha is texture.a * baseColorFactor.a.
    if (matAlphaMode == 1)
    {
        float sampledAlpha = matBaseColorFactor.a;
        if (matUseAlbedoMap)
            sampledAlpha *= texture(u_AlbedoMap, v_TexCoord).a;
        if (sampledAlpha < matAlphaCutoff)
            discard;
    }

    vec3 albedo = sampleAlbedo(u_AlbedoMap, v_TexCoord, matBaseColorFactor.rgb, matUseAlbedoMap);
    vec2 metallicRoughness = sampleMetallicRoughness(u_MetallicRoughnessMap, v_TexCoord,
                                                     matMetallicFactor, matRoughnessFactor,
                                                     matUseMRMap);
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;

    float ao = sampleAO(u_AOMap, v_TexCoord, matOcclusionStrength, matUseAOMap);
    vec3 emissive = sampleEmissive(u_EmissiveMap, v_TexCoord, matEmissiveFactor.rgb, matUseEmissiveMap);

    // sanitizeSurfaceNormal, not normalize: a zero-length or NaN interpolated normal
    // (zero-area triangle, cancelling smooth normals, bad import) would otherwise write a
    // NaN octahedral normal into the G-Buffer and light up as a white pixel.
    vec3 N = sanitizeSurfaceNormal(v_Normal, dFdx(v_WorldPos), dFdy(v_WorldPos));
    if (matUseNormalMap)
    {
        N = getNormalFromMap(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, matNormalScale);
    }

    // Screen-space velocity in [-1,1] NDC units.
    vec2 ndcCurr = v_ClipPosCurr.xy / max(v_ClipPosCurr.w, 1e-6);
    vec2 ndcPrev = v_ClipPosPrev.xy / max(v_ClipPosPrev.w, 1e-6);
    vec2 velocity = (ndcCurr - ndcPrev) * 0.5; // convert [-2,2] -> [-1,1]

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    // Alpha carries the PBR closure model selector (issue #975): 0=Legacy,
    // 1=ClosureV2. RGBA16F represents small integers exactly, and the
    // deferred lighting pass reads it back with a round().
    o_GBufferEmissive = vec4(emissive, oloEncodeGBufferPbrFlags(matPBRModel)); // flag-lane layout: see oloEncodeGBufferPbrFlags (#975)
    o_GBufferVelocity = velocity;
    o_GBufferEntityID = u_EntityID;
    // vec4(0) whenever the scene kill switch is off, this draw has no atlas
    // region, or the texel was never baked — the deferred ambient ladder then
    // falls through to probes/IBL exactly as it did before #865. Coverage is the
    // sampler's alpha, never the colour: a validly baked pure-black texel must
    // keep its darkness rather than glow with sky IBL.
    o_GBufferBakedGI = sampleLightmapIrradiance(v_TexCoord2, instances[v_InstanceIndex].LightmapScaleOffset);
}
