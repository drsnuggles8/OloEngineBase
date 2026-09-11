// =============================================================================
// VirtualGBufferFragment.glsl — the ONE fragment stage of the virtualized-
// geometry hardware raster paths (issues #629, #813). Included whole, after the
// includer's own `#type fragment` / `#version` lines, by:
//   * VirtualMeshGBuffer.glsl    — classic vertex path (MDI vertex pulling)
//   * VirtualMeshletGBuffer.glsl — VK_EXT_mesh_shader path (Vulkan-only)
// The two pipelines must shade identically; keeping the whole stage body here
// makes drift impossible instead of merely reviewable (two-mirrors-drift is a
// named failure mode in this repo). Anything added here lands in BOTH.
//
// Varying contract — every geometry stage feeding this must write exactly:
//   location 0  vec3  v_WorldPos       location 4  vec4      v_ClipPosPrev
//   location 1  vec3  v_Normal         location 5  flat int  v_EntityID
//   location 2  vec2  v_TexCoord       location 6  flat uint v_DbgSlot
//   location 3  vec4  v_ClipPosCurr
// (v_DbgSlot = the draw's VisibleCluster slot: gl_BaseInstanceARB on the MDI
// path, the workgroup's visible[] slot on the mesh-shader path.)
//
// Sibling includes resolve inside include/ (the engine's resolver rebases
// nested includes onto the included file's own directory).
// =============================================================================

#include "PBRCommon.glsl"
#include "VirtualDebugViz.glsl"
#include "VirtualGeometryGpuStructs.glsl"

// Cluster + visible records for the debug visualization (already bound at these
// SSBO points by the cull; read only when u_DebugMode != 0). Struct layouts
// come from the shared header — one GLSL spelling of the C++ mirrors.
layout(std430, binding = 33) readonly buffer VirtualClustersDbgBuf { VirtualCluster dbgClusters[]; };
layout(std430, binding = 38) readonly buffer VirtualVisibleDbgBuf { VisibleCluster dbgVisible[]; };

// PBR Material UBO (binding 2) — identical layout to PBR_GBuffer so the same
// PODMaterialData upload path works unchanged for virtual geometry.
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
    // Physical transmission / IOR / volume (issue #970). MUST mirror the
    // matching scalars in PBRMaterialUBO, which sit BEFORE the heap-offset
    // lanes so those stay last. Declared unconditionally -- unlike the heap
    // offsets these are plain material data that every arm reads, and a
    // shader that omitted them would relayout u_MaterialHeapOffsets by 32 B.
    //
    // u_AttenuationSigma* is the Beer-Lambert extinction coefficient the CPU
    // already derived (Material::GetAttenuationSigma), never the raw glTF
    // attenuation colour + distance: the glTF default distance is +infinity,
    // and deriving on the CPU is what keeps that infinity out of GLSL.
    float u_TransmissionFactor; // 0 = no transmission (the neutral default)
    float u_IOR;                // 1.5 == the F0 0.04 the dielectric path assumes
    float u_ThicknessFactor;    // 0 = thin-walled, > 0 = real volume
    float u_AttenuationSigmaR;
    float u_AttenuationSigmaG;
    float u_AttenuationSigmaB;
    // Uniquely named: a nameless std140 block may not reuse a name already at
    // global scope, and plain _padding0 is taken by other blocks in these same
    // shaders (glslc: "nameless block contains a member that already has a
    // name at global scope").
    float _pbrMaterialPad0;
    float _pbrMaterialPad1;
    // Per-material heap offsets (issue #691). MUST mirror
    // PBRMaterialUBO::HeapOffsets — std140 shifts every later field if the two
    // layouts disagree, and this block is the LAST member so a missing
    // declaration reads garbage rather than failing to link.
    //   [0] albedo, metallicRoughness, normal, ao
    //   [1] emissive, environment, irradiance, prefilter
    //   [2] brdfLut, diffuse(legacy), specular(legacy), unused
    uvec4 u_MaterialHeapOffsets[3];
};

// Converted whole (§5c) — the material five are every sampler this shader has,
// on either bindless arm. The HZB it reads for cluster culling belongs to
// VirtualClusterCull.comp, not here; this stage only writes G-Buffer targets.
//
// THE VULKAN ARM'S OPT-IN IS NOT HERE. `OLO_MATERIAL_VULKAN_HEAP_READER` is
// defined by each INCLUDER (VirtualMeshGBuffer.glsl, VirtualMeshletGBuffer.glsl)
// at the top of its fragment stage, because VulkanShader asks the entry shader's
// own pre-include text for a `#define` and because the two `#extension`
// directives the arm needs must precede every other token. This file only READS
// the token — which is exactly the shared-header shape ShaderSourceScan.h warns
// about, and the reason it is read with `#ifdef` and never defined here.
#include "BindlessHeap.glsl"
#ifdef OLO_MATERIAL_VULKAN_HEAP_READER
// The heap arrays and OLO_HEAP_MATERIAL_TEX_2D; guarded internally by
// `#ifdef OLO_VULKAN`, so it contributes nothing on any other route.
#include "DescriptorHeapTextures.glsl"

// ONE SAMPLER LANE FOR ALL FIVE: every material 2D descriptor is minted with
// HeapBinding::MaterialTexture2DSampler(), so the sampler offset is
// frame-uniform rather than per-material (amendment (96)).
//
// NO `OLO_MATERIAL_HEAP_READER` HERE: that token is the GL raw-GLSL route's
// marker, and a shader carrying it takes the ARB_bindless_texture arm.
#define u_AlbedoMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_MetallicRoughnessMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_METALLIC_ROUGHNESS_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_NormalMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_NORMAL_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_AOMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_AO_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_EmissiveMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_EMISSIVE_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#elif defined(OLO_BINDLESS)
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
layout(location = 5) flat in int v_EntityID;
layout(location = 6) flat in uint v_DbgSlot;
layout(location = 7) in vec2 v_TexCoord2;              // baked lightmap uv2 (issue #867)
layout(location = 8) flat in vec4 v_LightmapScaleOffset; // its atlas region; all-zero = no lightmap

// The atlas sampler + the sentinel-gated decode, shared verbatim with the
// classic path's PBR_GBuffer.glsl. Slot-based, no OLO_BINDLESS token — see the
// include's own header.
#include "LightmapSampling.glsl"

layout(location = 0) out vec4 o_GBufferAlbedo;    // RGBA8       albedo + metallic
layout(location = 1) out vec4 o_GBufferNormal;    // RGBA16F     octNormal + roughness + ao
layout(location = 2) out vec4 o_GBufferEmissive;  // RGBA16F     emissive + flags
layout(location = 3) out vec2 o_GBufferVelocity;  // RG16F       screen-space velocity
layout(location = 4) out int  o_GBufferEntityID;  // RED_INTEGER picking entity ID
// Baked lightmap irradiance target (G-Buffer RT5, issue #865). This shader
// draws no lightmapped receiver, but an MRT output it never writes is
// UNDEFINED in that attachment, and RT5's .a is a coverage flag — undefined
// there reads as "this pixel has baked GI" and the deferred ambient ladder
// shades it from whatever the target happened to hold. Writing vec4(0) is the
// explicit "no baked GI here" every non-lightmapped G-Buffer writer owes the
// lighting pass.
layout(location = 5) out vec4 o_GBufferBakedGI;

// Octahedral encode: unit normal -> [-1,1]^2 (same as PBR_GBuffer.glsl).
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
    // glTF MASK alpha handling, identical to PBR_GBuffer.glsl
    if (u_AlphaMode == 1)
    {
        float sampledAlpha = u_BaseColorFactor.a;
        if (u_UseAlbedoMap == 1)
            sampledAlpha *= texture(u_AlbedoMap, v_TexCoord).a;
        if (sampledAlpha < u_AlphaCutoff)
            discard;
    }

    vec3 albedo = OLO_MAT_ALBEDO(u_AlbedoMap, v_TexCoord, u_BaseColorFactor.rgb, bool(u_UseAlbedoMap));
    vec2 metallicRoughness = OLO_MAT_METALLIC_ROUGHNESS(u_MetallicRoughnessMap, v_TexCoord,
                                                        u_MetallicFactor, u_RoughnessFactor,
                                                        bool(u_UseMetallicRoughnessMap));
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;

    float ao = OLO_MAT_AO(u_AOMap, v_TexCoord, u_OcclusionStrength, bool(u_UseAOMap));
    vec3 emissive = OLO_MAT_EMISSIVE(u_EmissiveMap, v_TexCoord, u_EmissiveFactor.rgb, bool(u_UseEmissiveMap));

    // sanitizeSurfaceNormal, not normalize: see PBR_GBuffer.glsl — a zero/NaN vertex normal
    // must not reach the octahedral G-Buffer encode.
    vec3 N = sanitizeSurfaceNormal(v_Normal, dFdx(v_WorldPos), dFdy(v_WorldPos));
    if (u_UseNormalMap == 1)
    {
        N = OLO_MAT_NORMAL(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale);
    }

    vec2 ndcCurr = v_ClipPosCurr.xy / max(v_ClipPosCurr.w, 1e-6);
    vec2 ndcPrev = v_ClipPosPrev.xy / max(v_ClipPosPrev.w, 1e-6);
    vec2 velocity = (ndcCurr - ndcPrev) * 0.5;

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    o_GBufferEmissive = vec4(emissive, oloEncodeGBufferPbrFlags(u_PBRModel)); // flag-lane layout: see oloEncodeGBufferPbrFlags (#975)
    o_GBufferVelocity = velocity;
    o_GBufferEntityID = v_EntityID;
    // Baked lightmap irradiance into RT5 (issues #865, #867). This is the last
    // stage that still holds BOTH the uv2 and the per-instance atlas region —
    // the deferred lighting pass has neither — which is why RT5 carries the
    // resolved irradiance E rather than the atlas UV.
    //
    // sampleLightmapIrradiance returns vec4(0) whenever the scene has no bake,
    // the region is the all-zero sentinel, or the sampled texel has no
    // coverage, so this is also the "no baked GI here" every G-Buffer writer
    // owes the lighting pass.
    o_GBufferBakedGI = sampleLightmapIrradiance(v_TexCoord2, v_LightmapScaleOffset);

    // Debug visualization (no-op unless a debug mode is active). Resolve this
    // fragment's cluster + LOD from the draw's VisibleCluster record.
    if (u_DebugMode != 0)
    {
        uint ci = dbgVisible[v_DbgSlot].ClusterIndex;
        WriteVirtualDebug(ci, dbgClusters[ci].Lod);
    }
}
