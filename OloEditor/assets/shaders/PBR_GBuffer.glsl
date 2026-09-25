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
// The camera block, identical in both stages (GL links a program only if its
// stages declare it the same), extended through u_RenderOrigin: the snow layer
// is anchored in ABSOLUTE world space (issue #1451). The previous-frame VP slot
// is spelled `_cameraPrevViewProjection` because this program takes
// u_PrevViewProjection from the MotionBlurMatrices block instead.
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 _cameraPrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
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

// THE VULKAN MATERIAL-HEAP ARM (ADR 0011 amendment (96)). This shader declares
// only the material five, so on this backend it converts whole. The directives
// must sit here, before any other token — GLSL requires every `#extension` to
// precede all non-preprocessor tokens, and an include below cannot satisfy that
// (BindlessHeap.glsl's note). `#ifdef` is not a token, so the guard is legal and
// the GL tier — which compiles this same source WITHOUT the macro — never sees
// them. PBR_MultiLight.glsl carries the fuller note.
#ifdef OLO_VULKAN
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#define OLO_MATERIAL_VULKAN_HEAP_READER 1
#endif

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
    // MATERIAL KIND + SKIN PROFILE (issue #1231). The first two took over the
    // block's two spare pads; the four after them are an appended vec4. Like the
    // transmission scalars above they are declared UNCONDITIONALLY and sit
    // BEFORE u_MaterialHeapOffsets, so those stay last -- omitting them would
    // relayout the heap offsets by 16 B and every texture would sample the
    // wrong descriptor.
    //
    // u_MaterialKind is WHAT the surface is; u_PBRModel above is which VERSION
    // of the closure evaluates it. Two fields because they are two questions --
    // see docs/adr/0024-material-kind-is-not-the-closure-version.md.
    int u_MaterialKind;          // OLO_MATERIAL_KIND_*: 0=Generic, 1=Snow, 2=Skin
    int u_SkinProfileSlot;       // OLO_SKIN_PROFILE_SLOT_NONE (7) == names no profile
    float u_SkinSpecularTintR;   // LINEAR Rec.709, unitless [0,1]; 1,1,1 is neutral
    float u_SkinSpecularTintG;
    float u_SkinSpecularTintB;
    int u_SkinEvaluationModel;   // OLO_SKIN_MODEL_*, NOT the PBR closure version
    // THIN-REGION TRANSMISSION (issue #1242). Mirrors PBRMaterialUBO's
    // SkinTransmitScatter / SkinTransmitScaling, which sit on a 16-byte boundary
    // at offset 144 so std140 pads nothing in front of them. Declared
    // UNCONDITIONALLY and BEFORE u_MaterialHeapOffsets, like the #970 and #1231
    // lanes above -- omitting them would relayout the heap offsets by 48 B and
    // every texture would sample the wrong descriptor.
    //
    //   u_SkinTransmitScatter: xyz = ScatterColor * Strength, w = Anisotropy
    //   u_SkinTransmitScaling: xyz = Burley scaling d (MILLIMETRES), w = Power
    //
    // See include/SkinTransmission.glsl for what reads them and
    // Renderer/SkinTransmission.h for where the numbers come from.
    vec4 u_SkinTransmitScatter;
    vec4 u_SkinTransmitScaling;
    // The layered specular lane (issue #1243). MUST mirror
    // PBRMaterialUBO::SkinSpecularLane, packed by SkinSpecularLane().
    //   x = LobeMix (w)              y = LobeRoughnessScale (s)
    //   z = NormalVarianceStrength   w = 0, reserved
    // All-zero is neutral: no second lobe and no variance filtering.
    vec4 u_SkinSpecularLane;
    // The oral surface lane (issue #1245). MUST mirror
    // PBRMaterialUBO::SkinOralLane, packed by SkinOralLane().
    //   x = CoatStrength   y = CoatRoughness
    //   z = CoatF0 (derived on the CPU from the authored CoatIor)
    //   w = CavityOcclusion
    // All-zero is neutral: dry, with the transmitted term left as #1242 shipped
    // it. Declared UNCONDITIONALLY and BEFORE u_MaterialHeapOffsets like every
    // lane above it -- omitting it would relayout the heap offsets by 16 B and
    // every texture would sample the wrong descriptor.
    vec4 u_SkinOralLane;
    // The three ocular lanes (issue #1244). MUST mirror
    // PBRMaterialUBO::SkinOcular{Cornea,Iris,Response}Lane, packed by the three
    // matching CPU functions.
    //   Cornea:   x = eta (derived from CorneaIor)  y = curvature ratio
    //             z = iris plane depth, eye radii   w = limbus cosine
    //   Iris:     x = iris radius, eye radii        y = pupil radius, disc units
    //             z = limbal ring width, disc units w = OcularStrength (MASTER)
    //   Response: x = LimbalRingStrength            y = PupilDarkening
    //             z = IrisConcavity                 w = RefractionStrength
    // All-zero is neutral BECAUSE THE MASTER IS ZERO -- the other eleven
    // components are inert rather than meaningful at zero, and that is safe
    // only because irisLane.w gates every one of them. Declared
    // UNCONDITIONALLY and BEFORE u_MaterialHeapOffsets like every lane above
    // them -- omitting them would relayout the heap offsets by 48 B and every
    // texture would sample the wrong descriptor.
    vec4 u_SkinOcularCorneaLane;
    vec4 u_SkinOcularIrisLane;
    vec4 u_SkinOcularResponseLane;
    // The fourth ocular lane: xyz = IrisColor (linear Rec.709),
    // w = the iris edge band. ALL-ZERO IS BLACK HERE, NOT NEUTRAL -- the
    // colour is multiplied in, so neutral is WHITE. Safe only because
    // u_SkinOcularIrisLane.w gates the whole block.
    vec4 u_SkinOcularTintLane;
    int u_UseThicknessMap;            // 0 = no thickness map; the factor alone
    uint u_ThicknessMapHeapOffset;    // bindless descriptor offset; 0xFFFFFFFF = none
    float u_SkinThicknessBaseMM;      // thicknessFactor (m) * profile ThicknessScale, MILLIMETRES
    // The per-draw pore-band gain (issue #1243), from the profile's detail
    // fields and the entity's APPLIED morph weights. 0 leaves the authored
    // normal map untouched; -1 removes its pore band entirely. Took over the
    // slot the explicit pad held, so the prefix still ends 16-byte aligned.
    float u_SkinDetailStrength;
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
// Snow as a material layer (issue #1451) — the forward shaders' functions,
// so a deferred pixel carries the same snow. Brings the Snow UBO (13).
#include "include/SnowLayer.glsl"

// Camera UBO (binding 0), FRAGMENT SIDE (issue #1244).
//
// Declared here because the corneal refraction needs a VIEW DIRECTION, and this
// stage had no camera at all: the G-Buffer pass reads the camera in its vertex
// stage only. `u_CameraPosition - v_WorldPos` is INVARIANT under camera-relative
// rendering (issue #429) — both terms shift by the same render origin — so this
// needs no add-back, unlike a shader sampling an absolute-world pattern.
//
// THE MEMBER LIST MUST MATCH THE VERTEX STAGE'S EXACTLY, member for member,
// including the trailing padding. glLinkProgram() rejects a per-program UBO
// block whose members disagree between stages, and the failure is a link error
// with no line number. PBR_MultiLight.glsl carries the same note for the same
// reason; this block is a copy of the vertex declaration above and must stay one.
// The camera block, identical in both stages (GL links a program only if its
// stages declare it the same), extended through u_RenderOrigin: the snow layer
// is anchored in ABSOLUTE world space (issue #1451). The previous-frame VP slot
// is spelled `_cameraPrevViewProjection` because this program takes
// u_PrevViewProjection from the MotionBlurMatrices block instead.
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 _cameraPrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};


// Canonical GPU Scene material record (issue #994). Included AFTER the
// instance block because the link it resolves travels in
// InstanceData::GPUSceneRef. This is the raster path that consumes the record:
// every material constant below comes from the registry's committed record
// when the draw carries a live link, and from the per-draw UBO otherwise.
//
// The record supplies the FACTORS and the FLAGS; it does not supply the
// textures. Its own `*HeapOffset` fields still read
// OLO_GPU_SCENE_HEAP_OFFSET_UNRESOLVED wherever the engine descriptor heap is
// off (GL's default) and are untouched by #805 — the five material offsets this
// shader reads on the Vulkan arm come from PBRMaterialUBO, resolved through the
// BACKEND's slot region rather than the engine heap (ADR 0011 amendment (96)).
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
#ifdef OLO_MATERIAL_VULKAN_HEAP_READER
// The heap arrays and OLO_HEAP_MATERIAL_TEX_2D; guarded internally by
// `#ifdef OLO_VULKAN`, so it contributes nothing on any other route.
#include "include/DescriptorHeapTextures.glsl"
#include "include/MaterialShaderHeapTable.glsl"

// ONE SAMPLER LANE FOR ALL FIVE: every material 2D descriptor is minted with
// HeapBinding::MaterialTexture2DSampler(), so the sampler offset is
// frame-uniform rather than per-material (amendment (96)).
#define u_AlbedoMap OLO_HEAP_MATERIAL_TEX_2D(matHeapTextures.x, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_MetallicRoughnessMap OLO_HEAP_MATERIAL_TEX_2D(matHeapTextures.y, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_NormalMap OLO_HEAP_MATERIAL_TEX_2D(matHeapTextures.z, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_AOMap OLO_HEAP_MATERIAL_TEX_2D(matHeapTextures.w, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_EmissiveMap OLO_HEAP_MATERIAL_TEX_2D(matHeapEmissive, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_ThicknessMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_THICKNESS_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#elif defined(OLO_BINDLESS)
#define OLO_MATERIAL_HEAP_READER 1
#define u_AlbedoMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET)
#define u_MetallicRoughnessMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_METALLIC_ROUGHNESS_OFFSET)
#define u_NormalMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_NORMAL_OFFSET)
#define u_AOMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_AO_OFFSET)
#define u_EmissiveMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_EMISSIVE_OFFSET)
#define u_ThicknessMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_THICKNESS_OFFSET)
#else
layout(binding = 0) uniform sampler2D u_AlbedoMap;
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap;
layout(binding = 2) uniform sampler2D u_NormalMap;
layout(binding = 4) uniform sampler2D u_AOMap;
layout(binding = 5) uniform sampler2D u_EmissiveMap;
layout(binding = 76) uniform sampler2D u_ThicknessMap;   // TEX_SKIN_THICKNESS (issue #1242)
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
layout(location = 3) out vec4 o_GBufferVelocity;  // RG16F       screen-space velocity
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
    int   matMaterialKind      = u_MaterialKind;
    int   matSkinProfileSlot   = u_SkinProfileSlot;
    bool  matUseAlbedoMap      = bool(u_UseAlbedoMap);
    bool  matUseNormalMap      = bool(u_UseNormalMap);
    bool  matUseMRMap          = bool(u_UseMetallicRoughnessMap);
    bool  matUseAOMap          = bool(u_UseAOMap);
    bool  matUseEmissiveMap    = bool(u_UseEmissiveMap);

    GPUSceneMaterial gpuSceneMaterial;
#ifdef OLO_VULKAN
    uvec4 matHeapTextures = u_MaterialHeapOffsets[0];
    uint matHeapEmissive = OLO_MATERIAL_EMISSIVE_OFFSET;
    OloMaterialShaderHeapRecord heapRecord;
    // Validate the bounded texture table FIRST: even the canonical factor
    // lookup must not dereference an out-of-range GPU-written material index.
    if (oloMaterialShaderHeapRecord(instances[v_InstanceIndex].GPUSceneRef, u_MaterialHeapOffsets[1].yzw, heapRecord) &&
        oloGPUSceneMaterial(instances[v_InstanceIndex].GPUSceneRef, gpuSceneMaterial))
#else
    if (oloGPUSceneMaterial(instances[v_InstanceIndex].GPUSceneRef, gpuSceneMaterial))
#endif
    {
#ifdef OLO_VULKAN
        matHeapTextures = heapRecord.Textures;
        matHeapEmissive = heapRecord.Emissive;
        gpuSceneMaterial.Flags = heapRecord.Flags;
#endif
        matBaseColorFactor   = gpuSceneMaterial.BaseColorFactor;
        matEmissiveFactor    = gpuSceneMaterial.EmissiveFactor;
        matMetallicFactor    = gpuSceneMaterial.MetallicFactor;
        matRoughnessFactor   = gpuSceneMaterial.RoughnessFactor;
        matNormalScale       = gpuSceneMaterial.NormalScale;
        matOcclusionStrength = gpuSceneMaterial.OcclusionStrength;
        matAlphaCutoff       = gpuSceneMaterial.AlphaCutoff;
        matAlphaMode         = int(gpuSceneMaterial.AlphaMode);
        matPBRModel          = int(gpuSceneMaterial.ClosureVersion);
        // Issue #1231 — taken from the record for the same reason the closure
        // version is: one instanced draw can cover several materials, and the
        // per-draw UBO holds only one of them.
        matMaterialKind      = int(gpuSceneMaterial.MaterialKind);
        matSkinProfileSlot   = int(gpuSceneMaterial.SkinProfileSlot);
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

    vec3 albedo = OLO_MAT_ALBEDO(u_AlbedoMap, v_TexCoord, matBaseColorFactor.rgb, matUseAlbedoMap);
    vec2 metallicRoughness = OLO_MAT_METALLIC_ROUGHNESS(u_MetallicRoughnessMap, v_TexCoord,
                                                     matMetallicFactor, matRoughnessFactor,
                                                     matUseMRMap);
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;

    float ao = OLO_MAT_AO(u_AOMap, v_TexCoord, matOcclusionStrength, matUseAOMap);
    vec3 emissive = OLO_MAT_EMISSIVE(u_EmissiveMap, v_TexCoord, matEmissiveFactor.rgb, matUseEmissiveMap);

    // THE SNOW LAYER (issue #1451) — include/SnowLayer.glsl, the same calls
    // PBR_MultiLight makes. This shader had no snow at all, so Deferred meshes
    // rendered bare under a snowfall the Forward path showed. The blended
    // material and the snow-FILLED normal go into the G-Buffer and the
    // weight into RT3.a; the lighting pass rebuilds the shading normal and
    // adds the sparkle from those.
    vec3 snowWorldPos = v_WorldPos + u_RenderOrigin;
    float snowWeight = oloSnowLayerCoverage(snowWorldPos, v_Normal);
    oloSnowLayerBlendMaterial(snowWeight, albedo, metallic, roughness, ao, emissive);

    // sanitizeSurfaceNormal, not normalize: a zero-length or NaN interpolated normal
    // (zero-area triangle, cancelling smooth normals, bad import) would otherwise write a
    // NaN octahedral normal into the G-Buffer and light up as a white pixel.
    vec3 N = sanitizeSurfaceNormal(v_Normal, dFdx(v_WorldPos), dFdy(v_WorldPos));
    if (matUseNormalMap)
    {
        // THE EXPRESSION-DRIVEN PORE BAND (issue #1243), identical to the
        // forward path's so the two write the same normal for the same pixel.
        // Branched on the strength because the skin spelling costs a second
        // tap of the normal map.
        if (matMaterialKind == OLO_MATERIAL_KIND_SKIN && u_SkinDetailStrength != 0.0)
            N = OLO_SKIN_MAT_NORMAL(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, matNormalScale,
                                    u_SkinDetailStrength);
        else
            N = OLO_MAT_NORMAL(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, matNormalScale);
    }

    // THE VARIANCE FILTER (issue #1243), applied HERE so the ROUGHNESS WRITTEN
    // INTO THE G-BUFFER is already filtered and the deferred lighting pass
    // inherits it with no knowledge that anything happened.
    //
    // That is not a convenience, it is the only place it can go. The filter
    // needs dFdx of the shading normal, and the deferred lighting pass is a
    // FULLSCREEN draw whose neighbouring pixels can be different objects
    // entirely — a derivative there measures a silhouette, not sub-pixel
    // variance, and would draw a rough halo around everything in the scene.
    //
    // It also means MSAA does the right thing for free: this runs per SAMPLE,
    // so each sample's roughness reflects its own footprint before the resolve
    // averages them.
    if (matMaterialKind == OLO_MATERIAL_KIND_SKIN)
        roughness = oloSkinFilteredRoughness(roughness, N, u_SkinSpecularLane.z);

    // ---- THE CORNEA AND THE IRIS (issue #1244) ---------------------------
    //
    // HERE, and the position is load-bearing in both directions.
    //
    // AFTER the variance filter above, because that filter measures the
    // SCREEN-SPACE VARIANCE OF THE SURFACE NORMAL and the iris dish tilt is not
    // surface micro-detail. Filtering a smooth authored gradient would widen
    // the corneal highlight for a reason that has nothing to do with roughness
    // -- an eye that goes matte the moment its iris gains depth.
    //
    // BEFORE any light is looked at, because a refraction is NOT a BRDF: it
    // decides WHICH iris point this pixel is, and everything after shades that
    // point. That is the same reason this block appears in the G-Buffer
    // shaders as well as the forward ones and NOT in the deferred lighting
    // pass -- see include/SkinOcularSurface.glsl.
    //
    // THE OPTICAL AXIS IS THE ENTITY TRANSFORM'S +Z, read straight out of the
    // model matrix that include/InstanceBlock.glsl already puts in this stage.
    // Nothing is plumbed and no lane carries it, and that IS the left/right eye
    // convention: both eyes name the SAME .oloskin and differ only by their
    // transforms. Passed UNNORMALIZED -- oloSkinOcularApply tests its squared
    // length before normalizing, so a degenerate transform loses the eye
    // instead of producing a NaN albedo.
    //
    // Gated through oloSkinEvaluatesOcularSurface with the three lanes selected
    // by a ternary AT THIS CALL SITE, never by a helper that returns them. See
    // that function's comment for the miscompile the other shape caused in
    // #1245 -- with three lanes to select, the temptation was larger and so is
    // the blast radius.
    if (oloSkinEvaluatesOcularSurface(matMaterialKind, u_SkinEvaluationModel))
    {
        OloSkinOcular oloOcular = oloSkinOcularApply(albedo, N,
                                                     normalize(u_CameraPosition - v_WorldPos),
                                                     u_Model[2].xyz,
                                                     u_SkinOcularCorneaLane, u_SkinOcularIrisLane,
                                                     u_SkinOcularResponseLane, u_SkinOcularTintLane);
        albedo = oloOcular.Albedo;
        N = oloOcular.Normal;
    }

    // The snow-FILLED normal, the one the forward paths store too.
    N = oloSnowLayerFilledNormal(N, snowWeight);

    // Screen-space velocity in [-1,1] NDC units.
    vec2 ndcCurr = v_ClipPosCurr.xy / max(v_ClipPosCurr.w, 1e-6);
    vec2 ndcPrev = v_ClipPosPrev.xy / max(v_ClipPosPrev.w, 1e-6);
    vec2 velocity = (ndcCurr - ndcPrev) * 0.5; // convert [-2,2] -> [-1,1]

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    // Alpha carries the PBR closure model selector (issue #975): 0=Legacy,
    // 1=ClosureV2. RGBA16F represents small integers exactly, and the
    // deferred lighting pass reads it back with a round().
    o_GBufferEmissive = vec4(emissive, oloEncodeGBufferPbrFlagsEx(matPBRModel, matMaterialKind, matSkinProfileSlot)); // flag-lane layout: see oloEncodeGBufferPbrFlagsEx (#975, #1231)
    // .a: the material profile (#1256) is the snow weight (issue #1451).
    o_GBufferVelocity = vec4(velocity, 1.0, snowWeight);
    o_GBufferEntityID = u_EntityID;
    // vec4(0) whenever the scene kill switch is off, this draw has no atlas
    // region, or the texel was never baked — the deferred ambient ladder then
    // falls through to probes/IBL exactly as it did before #865. Coverage is the
    // sampler's alpha, never the colour: a validly baked pure-black texel must
    // keep its darkness rather than glow with sky IBL.

    // ---- THE DEFERRED THICKNESS LANE (issue #1242) -----------------------
    //
    // A skin pixel that names a profile parks its thickness in RT5's red
    // channel with coverage 0; see oloSkinPackGBufferThickness in
    // include/SkinTransmission.glsl for why that channel is free and what the
    // tenancy rules are.
    //
    // NO TRANSPORT-VERSION TEST ON THE THICKNESS, deliberately -- and note
    // that this is a statement about the THICKNESS and not about the shader.
    // Since issue #1244 this stage does read u_SkinEvaluationModel (the ocular
    // block above), so the old wording here -- "this shader has no
    // SkinEvaluationModel to test" -- is no longer true and has been corrected
    // rather than left to mislead the next reader.
    //
    // What is still true is that the THICKNESS's version test belongs to its
    // READER: the deferred lighting pass gets the version out of
    // u_SkinProfileParams[slot].w, which is the ONE place it lives on that
    // path. So the writer publishes the thickness for every skin
    // pixel that names a profile and the READER decides whether the profile's
    // version wants it. Duplicating the version test here would mean carrying
    // the model through the G-Buffer as well, for no gain: an unused thickness
    // in a channel that was otherwise zero costs nothing.
    //
    // The thickness comes from the per-draw UBO lanes rather than from
    // GPUSceneMaterial, which carries no thickness field. For a single-material
    // draw those are the same number; a GPU-Scene draw that batched two
    // materials with different thickness factors would use the UBO's. Stated
    // rather than hidden -- and the map, which is the per-pixel half, is
    // unaffected either way.
    float skinThicknessMM = 0.0;
    if (matMaterialKind == OLO_MATERIAL_KIND_SKIN && matSkinProfileSlot < OLO_SKIN_PROFILE_SLOT_NONE)
    {
        float thicknessSample = (u_UseThicknessMap != 0) ? texture(u_ThicknessMap, v_TexCoord).r : 1.0;
        skinThicknessMM = u_SkinThicknessBaseMM * clamp(thicknessSample, 0.0, 1.0);
    }

    vec4 bakedGI = sampleLightmapIrradiance(v_TexCoord2, instances[v_InstanceIndex].LightmapScaleOffset);
    // The irradiance WINS wherever there is any: taking a lightmapped surface's
    // indirect light away to make room for a transmission term would trade a
    // visible lighting regression for a subtle gain. The CPU counts that case as
    // SkinTransmissionFallbackReason::DeferredThicknessLaneUnavailable, so the
    // lost per-pixel thickness is reported rather than silently dropped.
    o_GBufferBakedGI = oloSkinPackGBufferThickness(bakedGI, skinThicknessMM > 0.0, skinThicknessMM);
}
