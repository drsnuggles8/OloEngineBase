// =============================================================================
// PBR_MultiLight.glsl - Physically Based Rendering Shader with Multi-Light Support
// Part of OloEngine Enhanced PBR System
// Supports metallic-roughness workflow (glTF 2.0 standard) with multiple lights
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
// Lightmap UV2 pull (issue #866). Rides the reserved SECOND pull binding (63,
// "OloBonePull" in ShaderBindingLayout.h) rather than minting a new engine-wide
// number: bones and baked lightmap UVs are mutually exclusive per mesh
// (MeshSource::Build only ever builds one of the two "stream 1" buffers —
// !HasSkeleton() gates the lightmap stream, HasSkeleton() gates the bone
// stream), and this shader is the non-skinned PBR variant, so it never shares
// a VAO with PBR_MultiLight_Skinned.glsl's bone data. Same precedent as
// FoliageRenderer / ParticleBatchRenderer riding 63 for their own per-instance
// data (ShaderBindingLayout.h's SSBO_BONE_PULL comment lists all tenants). A
// VAO with no lightmap UV buffer (unbaked static mesh) leaves stream 1 short —
// AssembleAndPushRootData resolves it to the zero address, and
// LightmapScaleOffset's all-zero default gates sampling regardless.
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
// A VAO without the stream leaves the attribute disabled and GL supplies
// (0,0), which lands outside every atlas region and samples nothing.
layout(location = 3) in vec2 a_TexCoord2;
#endif

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Forward-path velocity support: previous-frame view-projection used
    // together with u_PrevModel to reconstruct per-object screen-space
    // motion into scene FB RT3. Equals ViewProjection on the first frame
    // so velocity starts at zero.
    mat4 u_PrevViewProjection;
};

// Model UBO (binding 3)
#include "include/InstanceBlock_Vertex.glsl"

// Output to fragment shader
layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
// Clip-space positions for per-pixel velocity reconstruction in the fragment
// shader. Passing them through the interpolators (rather than recomputing
// from v_WorldPos) gives correct perspective-interpolated motion on moving
// objects, matching the deferred G-Buffer path.
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
    // Gate the read on the SAME per-instance signal sampleLightmapIrradiance()
    // uses (scaleOffset.x <= 0.0 => no lightmap for this draw): an unbaked
    // static mesh never gets a stream-1 buffer (MeshSource::Build's stub
    // branch is a Vulkan no-op), so an unconditional pull resolves to the
    // arena's null block, and a real mesh's vertex count routinely exceeds
    // that block's fixed size -- an out-of-bounds buffer-device-address READ,
    // which is a GPU page fault/device loss on Vulkan, not a clamped read
    // (amendment (78)/(84)). LightmapScaleOffset.x > 0 is only ever published
    // for a mesh whose UV2 stream was actually built, so this branch is both
    // necessary (skips the unsafe read) and sufficient (never skips a safe one).
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

    vec4 clipCurr = u_ViewProjection * vec4(v_WorldPos, 1.0);
    vec4 prevWorldPos = u_PrevModel * vec4(a_Position, 1.0);
    vec4 clipPrev = u_PrevViewProjection * prevWorldPos;

    v_ClipPosCurr = clipCurr;
    v_ClipPosPrev = clipPrev;

    gl_Position = clipCurr;
}

#type fragment
#version 460 core

// THE VULKAN MATERIAL-HEAP ARM (ADR 0011 amendment (96)). The five
// material-local maps are reached by runtime heap index on this backend;
// everything else this shader samples keeps its classic binding, which is what
// (96) scopes and what a heap array's ABSENT binding decoration makes possible.
//
// THE DIRECTIVES MUST SIT HERE, before any other token: GLSL requires every
// `#extension` to precede all non-preprocessor tokens, and an include below
// cannot satisfy that (BindlessHeap.glsl's note). Guarded by `#ifdef OLO_VULKAN`
// so the GL tier — which compiles this same source at vulkan_1_2 WITHOUT the
// macro — never sees them; a conditional is not a token, so the rule still holds.
//
// OLO_MATERIAL_VULKAN_HEAP_READER is read twice: PBRCommon.glsl's OLO_MAT_*
// wrappers switch on it (a combined sampler cannot cross a function call), and
// VulkanShader scans the source for it to decide that this program reads
// per-material offsets — which is what makes CommandDispatch skip the five
// binds. Defined BEFORE the PBRCommon include below, necessarily.
#ifdef OLO_VULKAN
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require
#define OLO_MATERIAL_VULKAN_HEAP_READER 1
#endif

// FIRST, because the sampler declarations below expand its accessor macros on
// the bindless build. The heap block itself is #ifdef-guarded internally, so on
// the slot-based build this include contributes nothing.
#include "include/BindlessHeap.glsl"

#include "include/PBRCommon.glsl"
// Virtual Shadow Maps (issue #702) — self-contained (UBO 79/80, page-table
// SSBO 54, sampler 65). BindForSampling publishes a DISABLED globals block
// when VSM is off, so the runtime branch below costs nothing then.
#include "include/VirtualShadowSampling.glsl"
#include "include/SnowCommon.glsl"
#include "include/LightProbeSampling.glsl"
#include "include/AtmosphereShading.glsl"

// Camera UBO (binding 0) - for view position
// Fragment-side CameraMatrices must declare the same block layout as the
// vertex stage (above) — glLinkProgram() rejects per-program UBO blocks
// whose members disagree between stages. We include the trailing
// u_PrevViewProjection so the layouts match; the fragment shader doesn't
// reference it, and the GLSL compiler dead-strips it.
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
#if defined(OLO_BINDLESS) || defined(OLO_MATERIAL_VULKAN_HEAP_READER)
    // The per-material offset lanes, declared on EITHER bindless arm. The
    // C++ PBRMaterialUBO always uploads them (sizeof == 336 since issue #1244);
    // a std140 block may declare a PREFIX of what the CPU writes, which is why
    // the slot-based build can stop before these and stay correct. Note the
    // prefix now has to run through the #1231 lanes, not stop at u_PBRModel:
    // this arm's u_MaterialHeapOffsets sits after them, so leaving them out
    // would move the offsets by 16 B. Must be LAST — the lane layout in
    // include/BindlessHeap.glsl and CommandDispatch::WriteMaterialHeapOffsets
    // both assume it.
    uvec4 u_MaterialHeapOffsets[3];
#endif
};

// Snow UBO (binding 13)
layout(std140, binding = 13) uniform SnowParams {
    vec4 u_SnowCoverageParams;      // (heightStart, heightFull, slopeStart, slopeFull)
    vec4 u_SnowAlbedoAndRoughness;  // (albedo.rgb, roughness)
    vec4 u_SnowSSSColorAndIntensity;// (sssColor.rgb, sssIntensity)
    vec4 u_SnowSparkleParams;       // (sparkleIntensity, sparkleDensity, sparkleScale, normalPerturbStrength)
    vec4 u_SnowFlags;               // (enabled, pad, pad, pad)
};

// =============================================================================
// TEXTURE BINDINGS
// =============================================================================

// CONVERTED WHOLE, and that is the rule rather than a preference (§5c). Taking
// the bindless route is a property of the PROGRAM: it makes
// Shader::IsBoundProgramBindless() true, so HeapBinding::BindTextureOrOffset
// stages an offset and issues NO bind for EVERY input this shader declares. A
// single sampler left as `layout(binding = N)` here would therefore read black
// — which is exactly how the shadow maps were lost when only the material five
// were converted, and how Terrain_Depth lost its snow depth map.
//
// TWO OFFSET SOURCES, split by WHO OWNS THE BINDING, not by convenience:
//   * the five MATERIAL-LOCAL maps change per draw and ride in PBRMaterialUBO,
//     which the draw path already uploads per material;
//   * everything else is PUBLISHED frame state (the environment probe, the IBL
//     trio, the shadow arrays) that most materials carry no handle for, so it
//     comes from the shared g_OloHeapOffsets table. Routing those per-material
//     resolves an invalid handle to the reserved null and the mesh loses all
//     ambient light.
#ifdef OLO_MATERIAL_VULKAN_HEAP_READER
// The heap arrays and OLO_HEAP_MATERIAL_TEX_2D. Guarded internally by
// `#ifdef OLO_VULKAN`, so it contributes nothing on any other route.
#include "include/DescriptorHeapTextures.glsl"

// THE VULKAN ARM (amendment (96)). Only the five material-local maps convert:
// they are asset-owned Texture2D resting in SHADER_READ_ONLY_OPTIMAL, which is
// the one thing HeapBinding::ResolveShaderHeapTexture can describe. The
// environment cubemap and the IBL trio are baked into render targets and stay
// on their classic bindings below, on BOTH backends.
//
// ONE SAMPLER LANE FOR ALL FIVE: every material 2D descriptor is minted with
// HeapBinding::MaterialTexture2DSampler(), so the sampler offset is frame-
// uniform rather than per-material.
//
// NO `OLO_MATERIAL_HEAP_READER` HERE, and that is not an oversight: that token
// is the GL raw-GLSL route's marker, scanned by CreateProgramFromRawGLSL, and a
// shader carrying it takes the ARB_bindless_texture arm. This arm's marker is
// the one defined at the top of the stage.
#define u_AlbedoMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_MetallicRoughnessMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_METALLIC_ROUGHNESS_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_NormalMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_NORMAL_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_AOMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_AO_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_EmissiveMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_EMISSIVE_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#define u_ThicknessMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_THICKNESS_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)

#elif defined(OLO_BINDLESS)
// The opt-in marker CreateProgramFromRawGLSL scans for to decide that this
// program reads per-material offsets — and therefore that BindPBRTextures must
// skip the five material binds. Deliberately an explicit token: keying on the
// accessor macros or the UBO field would match every shader that merely
// INCLUDES the header below.
#define OLO_MATERIAL_HEAP_READER 1

#define u_AlbedoMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET)
#define u_MetallicRoughnessMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_METALLIC_ROUGHNESS_OFFSET)
#define u_NormalMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_NORMAL_OFFSET)
#define u_AOMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_AO_OFFSET)
#define u_EmissiveMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_EMISSIVE_OFFSET)
#define u_ThicknessMap OLO_MATERIAL_TEX_2D(OLO_MATERIAL_THICKNESS_OFFSET)

#else
// Texture bindings following ShaderBindingLayout
layout(binding = 0) uniform sampler2D u_AlbedoMap;          // TEX_DIFFUSE
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap; // TEX_SPECULAR (repurposed)
layout(binding = 2) uniform sampler2D u_NormalMap;          // TEX_NORMAL
layout(binding = 4) uniform sampler2D u_AOMap;              // TEX_AMBIENT
layout(binding = 5) uniform sampler2D u_EmissiveMap;        // TEX_EMISSIVE
layout(binding = 76) uniform sampler2D u_ThicknessMap;      // TEX_SKIN_THICKNESS (issue #1242)
#endif

// THE PUBLISHED ENVIRONMENT AND IBL SET, and it is a SEPARATE fork on purpose.
//
// Amendment (96) converts the five material-local maps and nothing else, so
// these four are classic on the Vulkan arm exactly as they are on the slot-based
// one — and writing them out inside the Vulkan arm above would be a SECOND copy
// of the same four declarations. That copy is not merely redundant: the §5c scan
// evaluates the file with OLO_BINDLESS defined, reads a fork whose leading
// condition does not mention OLO_BINDLESS as pass-through, and would then see
// these declarations survive on the GL bindless route — where they genuinely
// would read black, because there the seam withholds every bind. One fork keyed
// on OLO_BINDLESS keeps the GL arm's macros and the shared classic declarations
// mutually exclusive, which is what both the compiler and the scan need.
//
// A heap array carries no Binding decoration, so the four keep their
// VkDescriptorSetAndBindingMappingEXT entries untouched next to the converted
// five — the property (96) rests on, measured on SDK 1.4.357.0.
#ifdef OLO_BINDLESS
#define u_EnvironmentMap OLO_HEAP_TEX_CUBE(9)
#define u_IrradianceMap OLO_HEAP_TEX_CUBE(10)
#define u_PrefilterMap OLO_HEAP_TEX_CUBE(11)
#define u_BRDFLutMap OLO_HEAP_TEX_2D(12)
#else
layout(binding = 9) uniform samplerCube u_EnvironmentMap;   // TEX_ENVIRONMENT

// IBL textures (if available)
layout(binding = 10) uniform samplerCube u_IrradianceMap;   // TEX_USER_0
layout(binding = 11) uniform samplerCube u_PrefilterMap;    // TEX_USER_1
layout(binding = 12) uniform sampler2D u_BRDFLutMap;        // TEX_USER_2
#endif

// Shadow map textures — CSM array + the budgeted local-light shadow atlas
// (issue #435; the atlas replaced the spot array and the 4 point cubemaps).
//
// ONE DEPTH ARRAY REACHED AS TWO VIEWS. The comparison views below and the
// comparison-OFF raw views are the same GL texture with different sampler state,
// which under bindless means two DIFFERENT descriptors: sampler state is baked
// into the handle. CommandDispatch::ShadowDepthSampler mints both, and the seam
// derives ViewDesc::DepthCompare from SamplerDesc::Compare so they cannot drift.
#ifdef OLO_BINDLESS
#define u_ShadowMapCSM OLO_HEAP_TEX_2D_ARRAY_SHADOW(8)
#define u_ShadowAtlas OLO_HEAP_TEX_2D_ARRAY_SHADOW(13)
#define u_ShadowMapCSMRaw OLO_HEAP_TEX_2D_ARRAY(33)
#define u_ShadowAtlasRaw OLO_HEAP_TEX_2D_ARRAY(34)
#else
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
    float u_AtlasDepthBias; // local-light atlas constant depth bias, normalized [0,1] (#1119)
    int _shadowPad2;
};

// Clustered light lists (issue #435). Included AFTER the ShadowData block +
// atlas samplers above: with FPLUS_ATLAS_SHADOWS defined, the per-cluster
// light evaluator attenuates every culled light by its shadow-atlas entry.
#define FPLUS_ATLAS_SHADOWS 1
#include "include/ForwardPlusCommon.glsl"

// Distance-impostor reflection probes (issue #705). The cube-array slots
// (14/15) stay slot-based in BOTH variants: ReflectionProbeArray publishes
// them via PublishTextureOffsetAndBind (offset staged AND real bind issued),
// the DDGI-atlas pattern the bindless pipeline test allowlists.
#define OLO_REFLECTION_PROBE_SAMPLERS
#include "include/ReflectionProbes.glsl"

// =============================================================================
// INPUT/OUTPUT
// =============================================================================

// Input from vertex shader
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec4 v_ClipPosCurr;
layout(location = 4) in vec4 v_ClipPosPrev;
layout(location = 5) in vec2 v_TexCoord2;

// Output
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Forward-path TAA motion vector. Scene FB attachment 3 is RG16F; the
// PostProcessRenderPass binds it as u_Velocity for TAA in Forward /
// Forward+ (Deferred reads G-Buffer RT3 instead).
layout(location = 3) out vec2 o_Velocity;
// Scene FB RT4: the DIFFUSE half of a skin pixel's lighting, handed to the
// screen-space diffusion pass (issue #1241). Zero on every other surface, and on
// a skin surface whose profile is authored against transport version 0. Scene
// colour above still carries the whole composite -- the diffusion pass adds
// `blur(this) - this` -- so a frame with the pass culled is the #1231 frame.
// See include/PBRCommon.glsl, "THE DIFFUSION HAND-OFF".
layout(location = 4) out vec4 o_SkinDiffuse;


// Octahedral encode: unit normal → RG16F [-1,1]²
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// Model UBO (binding 3) for entity ID access
// Fragment-side ModelMatrices must match the vertex stage's block layout
// (which includes the trailing u_PrevModel for per-object velocity).
// glLinkProgram() rejects per-program UBO blocks whose members disagree
// between stages; u_PrevModel is unused in fragment but its declaration
// keeps the two stages' block types identical.
#include "include/InstanceBlock.glsl"

// Baked lightmap sampling (issue #439): UBO 1 + the atlas sampler at TEX 16.
#include "include/LightmapSampling.glsl"
#include "include/AmbientLadder.glsl"

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

    vec3 albedo = OLO_MAT_ALBEDO(u_AlbedoMap, v_TexCoord, u_BaseColorFactor.rgb, bool(u_UseAlbedoMap));
    vec2 metallicRoughness = OLO_MAT_METALLIC_ROUGHNESS(u_MetallicRoughnessMap, v_TexCoord,
                                                        u_MetallicFactor, u_RoughnessFactor,
                                                        bool(u_UseMetallicRoughnessMap));
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;

    float ao = OLO_MAT_AO(u_AOMap, v_TexCoord, u_OcclusionStrength, bool(u_UseAOMap));
    vec3 emissive = OLO_MAT_EMISSIVE(u_EmissiveMap, v_TexCoord, u_EmissiveFactor.rgb, bool(u_UseEmissiveMap));

    // Calculate normal
    vec3 N = normalize(v_Normal);
    if (u_UseNormalMap == 1)
    {
        // THE EXPRESSION-DRIVEN PORE BAND (issue #1243). The skin spelling
        // takes a second, coarser tap of the SAME normal map and scales the
        // difference — see oloSkinDetailTangentNormal in
        // include/SkinLayeredSpecular.glsl for why the band comes out of the
        // map that is already there rather than out of a second one.
        //
        // Branched on the strength, not merely on the kind: the second tap is a
        // real texture fetch, and a skin material whose author left the detail
        // fields at their neutral default must cost what it cost before.
        if (u_MaterialKind == OLO_MATERIAL_KIND_SKIN && u_SkinDetailStrength != 0.0)
            N = OLO_SKIN_MAT_NORMAL(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale,
                                    u_SkinDetailStrength);
        else
            N = OLO_MAT_NORMAL(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale);
    }
    vec3 V = normalize(u_CameraPosition - v_WorldPos);

    // Weather response (issue #633): rain-wet surfaces darken and gloss up
    // before any lighting reads albedo/roughness.
    atmosphereApplyWetness(albedo, roughness, N);

    // THE VARIANCE FILTER (issue #1243). HERE, and the position is load-bearing
    // in both directions: AFTER the normal map and the detail band, because the
    // variance this measures is theirs and taking dFdx of the vertex normal
    // would see only the mesh's curvature; and AFTER the wetness response,
    // because that rewrites `roughness` and filtering the value it is about to
    // replace would be filtering a number nothing shades with.
    //
    // A zero strength — which is what every non-skin material and every profile
    // below transport version 3 uploads — returns `roughness` unchanged and
    // costs one compare. See oloSkinFilteredRoughness.
    if (u_MaterialKind == OLO_MATERIAL_KIND_SKIN)
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
    if (oloSkinEvaluatesOcularSurface(u_MaterialKind, u_SkinEvaluationModel))
    {
        OloSkinOcular oloOcular = oloSkinOcularApply(albedo, N,
                                                     normalize(u_CameraPosition - v_WorldPos),
                                                     u_Model[2].xyz,
                                                     u_SkinOcularCorneaLane, u_SkinOcularIrisLane,
                                                     u_SkinOcularResponseLane, u_SkinOcularTintLane);
        albedo = oloOcular.Albedo;
        N = oloOcular.Normal;
    }

    // ---- THE ORAL SURFACE LANE (issue #1245) -----------------------------
    //
    // Resolved ONCE, through oloSkinEvaluatesOralSurface so the version test is the one
    // function the clustered and deferred paths also call. All-zero for every
    // non-skin pixel and every profile below transport version 4, which is dry
    // and costs one compare per light rather than a GGX evaluation.
    vec4 skinOralLane = oloSkinEvaluatesOralSurface(u_MaterialKind, u_SkinEvaluationModel)
                            ? u_SkinOralLane : vec4(0.0);

    // Cloud shadow (issue #633): the cloudscape occludes the directional body
    // regardless of the CSM gate below — evaluated once, applied per
    // directional light inside the loop.
    float cloudShadow = atmosphereCloudShadow(v_WorldPos);

    // ---- THIN-REGION TRANSMISSION SETUP (issue #1242) -------------------
    //
    // THREE CONDITIONS, AND EACH RULES OUT A DIFFERENT WRONG FRAME.
    //
    //  * the material kind, because transmission is a property of skin and a
    //    Generic material must shade exactly as it did before this feature;
    //  * the transport VERSION, because a profile authored against #1231 or
    //    #1241 must not acquire a new term just because the renderer can
    //    evaluate one (docs/adr/0024). Spelled as an equality against version 2
    //    rather than `>= 2`, so a version this shader has no arm for transmits
    //    NOTHING instead of guessing;
    //  * a non-zero THICKNESS, because a zero thickness means "no volume
    //    authored here" and the other reading of it -- exp(0) = 1, fully
    //    transparent -- is the uniformly emissive head the issue forbids.
    //
    // The thickness is u_SkinThicknessBaseMM (thicknessFactor x the profile's
    // ThicknessScale, converted to MILLIMETRES on the CPU) modulated by the
    // map's red channel. With no map the modulation is 1, so the material's
    // scalar thickness applies uniformly -- which is what makes a head that
    // loses its map fall back to a uniform thickness rather than to none.
    // BOTH TRANSMITTING VERSIONS. The versions are CUMULATIVE — version 3 is
    // "everything version 2 does, plus the layered specular" — so omitting it
    // here would silently stop a version-3 head transmitting through its ears
    // the moment its author turned the lobes on.
    bool isSkinTransmitting = (u_MaterialKind == OLO_MATERIAL_KIND_SKIN) &&
                              ((u_SkinEvaluationModel == OLO_SKIN_MODEL_THICKNESS_TRANSMISSION) ||
                               (u_SkinEvaluationModel == OLO_SKIN_MODEL_LAYERED_SPECULAR) ||
                               (u_SkinEvaluationModel == OLO_SKIN_MODEL_ORAL_SURFACE) ||
                               (u_SkinEvaluationModel == OLO_SKIN_MODEL_OCULAR_SURFACE) ||
                               (u_SkinEvaluationModel == OLO_SKIN_MODEL_ISOTROPIC_GATHER));
    float skinThicknessMM = 0.0;
    if (isSkinTransmitting)
    {
        float thicknessSample = (u_UseThicknessMap != 0) ? texture(u_ThicknessMap, v_TexCoord).r : 1.0;
        skinThicknessMM = u_SkinThicknessBaseMM * clamp(thicknessSample, 0.0, 1.0);
        // Fold the thickness test into the gate so the loop asks one question
        // instead of two, and so a profile at version 2 on a material with no
        // authored thickness costs nothing per light. The CPU has already
        // COUNTED that case as SkinTransmissionFallbackReason::NoThickness
        // (Renderer3DMeshSubmission.cpp) -- this is the shading consequence of
        // that report, not a silent second opinion about it.
        isSkinTransmitting = skinThicknessMM > 0.0;
    }

    // The transmitted lobe, accumulated beside Lo and composited at the end.
    // Kept OUT of the diffuse/specular split on purpose: it is a third
    // transport, not a half of the BRDF, and folding it into the diffuse half
    // would hand #1241's diffusion pass energy that never went through the
    // surface -- which is exactly the double-count #1242's third criterion
    // forbids. Same placement, and the same argument, as the foliage lobe in
    // include/DeferredLightingShared.glsl.
    vec3 transmitted = vec3(0.0);

    // Calculate direct lighting from all lights
    // Direct lighting, diffuse and specular kept apart all the way to the
    // composite (issue #1231). The two halves are summed once, at the end, so a
    // Generic material's pixel is what it always was; the seam exists so skin
    // scattering (#1241) can blur one half without touching the other.
    OloSurfaceLighting Lo = oloSurfaceLightingZero();

    // Forward+ path: use per-cluster culled light lists for point/spot lights
    bool fplusActive = (fplus_Params.z != 0u);
    if (fplusActive)
    {
        float fplusViewDepth = -(u_View * vec4(v_WorldPos, 1.0)).z;
        Lo = oloSurfaceLightingAdd(Lo, fplusEvaluateTileLightsSplit(N, V, v_WorldPos, albedo, metallic,
                                                                    roughness, fplusViewDepth, u_PBRModel,
                                                                    oloSkinLobeFor(u_MaterialKind, u_SkinEvaluationModel,
                                                                                   u_SkinSpecularLane),
                                                                    skinOralLane));
    }

    // UBO light loop: when Forward+ is active, only evaluate directional lights
    // (stored at the start of the array). When Forward+ is off, evaluate all lights.
    int loopCount = fplusActive ? min(u_DirectionalLightCount, MAX_LIGHTS)
                                : min(u_LightCount, MAX_LIGHTS);
    for (int i = 0; i < loopCount; ++i)
    {
        int lightType = int(u_Lights[i].position.w);

        // THE LAYERED CLOSURE (issue #1243). The skin twin of
        // calculateLightContributionSplit, which for a lobe mix of 0 — every
        // non-skin pixel, and every skin pixel below transport version 3 —
        // returns the identical result without evaluating anything twice.
        OloSurfaceLighting lightContrib = oloSkinLightContributionSplit(
            u_Lights[i], N, V, albedo, metallic, roughness, v_WorldPos, u_PBRModel,
            oloSkinLobeFor(u_MaterialKind, u_SkinEvaluationModel, u_SkinSpecularLane));

        // THE VISIBILITY FACTOR, ACCUMULATED RATHER THAN APPLIED (issue #1242).
        //
        // Every branch below used to call oloSurfaceLightingScale on
        // lightContrib in place. They now multiply into ONE float that is
        // applied once, at the bottom of the loop.
        //
        // EXACTLY EQUIVALENT for the reflected lobe: those were a chain of
        // multiplies by a scalar broadcast to vec3, and multiplication is
        // associative, so folding the cloud shadow and the shadow-map factor
        // into one scalar first produces the same product. What it BUYS is that
        // the transmission lobe can be gated by the same number -- which is what
        // #1242's second criterion demands, and what a per-branch in-place scale
        // made impossible without evaluating the shadow a second time.
        //
        // This is the same refactor #1234 made to the DEFERRED path
        // (include/DeferredLightingShared.glsl), done here so the two paths gate
        // their two lobes identically rather than by two different accidents.
        float lightVisibility = 1.0;

        // THE NORMAL THE SHADOW IS BIASED ALONG. For a BACKLIT skin pixel the
        // shading normal points at the viewer and therefore away from the light,
        // so the receiver normal-offset would push the sample point into the
        // head's own shadow-map depth and collapse the very term it is gating.
        // oloSkinShadowNormal returns N unchanged wherever dot(N, L) > 0 -- i.e.
        // wherever the reflected lobe is non-zero -- so ONE lookup serves both
        // lobes, and this is the identity for every non-skin surface.
        vec3 shadowN = N;
        vec3 lightL;
        vec3 lightRadiance;
        bool lightHasDirection = oloLightSample(u_Lights[i], v_WorldPos, lightL, lightRadiance);
        if (isSkinTransmitting && lightHasDirection)
            shadowN = oloSkinShadowNormal(N, lightL);

        if (lightType == DIRECTIONAL_LIGHT)
        {
            lightVisibility *= cloudShadow;
        }
        if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
            // Compute view-space depth for cascade selection
            vec4 viewSpacePos = u_View * vec4(v_WorldPos, 1.0);
            float viewDepth = viewSpacePos.z;

            // VSM owns the directional light when active (issue #702) — the CSM
            // cascades are not rendered at all in that case, so this is an
            // either/or rather than a blend.
            float shadow;
            if (VSM_ENABLED != 0)
            {
                shadow = vsmShadowFactor(v_WorldPos, shadowN);
            }
            else
            {
                shadow = calculateCascadedShadowFactorCSM(
                    u_ShadowMapCSM,
                    u_ShadowMapCSMRaw,
                    v_WorldPos,
                    shadowN,
                    viewDepth,
                    u_DirectionalLightSpaceMatrices,
                    u_CascadePlaneDistances,
                    u_ShadowParams,
                    u_ShadowMapResolution,
                    u_SoftShadowMode
                );
            }
            lightVisibility *= shadow;
        }
        // Apply spot light shadows (atlas entry, issue #435)
        else if (lightType == SPOT_LIGHT)
        {
            // Atlas entry or VSM layer base, decided by vsmLocalShadow (#703).
            int atlasEntry = int(u_Lights[i].direction.w);
            float localShadow;
            if (vsmLocalShadow(v_WorldPos, shadowN, atlasEntry, false, localShadow))
            {
                lightVisibility *= localShadow;
            }
            else if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
            {
                float shadow = calculateAtlasEntryShadow(
                    v_WorldPos,
                    u_AtlasEntryMatrices[atlasEntry],
                    u_AtlasEntryScaleOffset[atlasEntry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_AtlasDepthBias,
                    u_AtlasResolution,
                    u_SoftShadowMode,
                    u_ShadowParams.z
                );
                lightVisibility *= shadow;
            }
        }
        // Apply point light shadows (6 consecutive atlas cube-face entries)
        else if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
        {
            // Sphere area lights shadow from the emitter centre (the
            // representative point), so both types share the point path:
            // direction.w carries the BASE atlas entry of the 6 face tiles.
            int baseEntry = int(u_Lights[i].direction.w);
            float localShadow;
            if (vsmLocalShadow(v_WorldPos, shadowN, baseEntry, true, localShadow))
            {
                lightVisibility *= localShadow;
            }
            else if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
            {
                vec3 lightPos = u_Lights[i].position.xyz;
                int entry = baseEntry + atlasCubeFace(v_WorldPos - lightPos);
                float shadow = calculateAtlasEntryShadow(
                    v_WorldPos,
                    u_AtlasEntryMatrices[entry],
                    u_AtlasEntryScaleOffset[entry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_AtlasDepthBias,
                    u_AtlasResolution,
                    0, // PCF only on cube faces (matches the old cubemap path)
                    u_ShadowParams.z
                );
                lightVisibility *= shadow;
            }
        }

        // THE WET COAT (issue #1245), applied BEFORE the visibility factor and
        // AFTER the surface closure. Both halves of that placement matter.
        //
        // AFTER the closure, because the coat is a layer in FRONT of whatever
        // the closure produced and has to attenuate it — it cannot attenuate a
        // value that has not been computed yet.
        //
        // BEFORE the visibility, so the coat's own lobe is gated by the SAME
        // shadow factor the surface lobe and the transmitted lobe are. A
        // highlight that survived a shadow map would be the brightest wrong
        // pixel in the frame.
        //
        // Skipped for a sphere area light, which oloLightSample declines to give
        // a direction for: that evaluator's representative point depends on N
        // and V, so there is no single L for a second BRDF to be evaluated
        // along. Declined rather than approximated, exactly as the transmitted
        // lobe below declines it.
        //
        // THE COSINE IS APPLIED TO THE RADIANCE HERE, because oloLightSample
        // returns the radiance WITHOUT it while oloSkinLightContributionSplit
        // folds it in before returning. Handing the coat the un-cosine-weighted
        // radiance would light the film by a different number than the surface
        // beneath it and break the partition that makes the coat conservative.
        if (skinOralLane.x > 0.0 && lightHasDirection)
        {
            vec3 coatRadiance = lightRadiance * max(dot(N, lightL), 0.0);
            lightContrib = oloSkinOralApplyCoat(lightContrib, N, V, lightL, coatRadiance, skinOralLane);
        }

        Lo = oloSurfaceLightingAdd(Lo, oloSurfaceLightingScale(lightContrib, vec3(lightVisibility)));

        // THE TRANSMITTED LOBE, gated by the SAME visibility the reflected lobe
        // just was. That single shared factor is the whole of #1242's second
        // criterion: an ear behind a raised hand stops glowing, because whatever
        // darkens its lit face also darkens what comes through it.
        //
        // Skipped for a sphere area light, which oloLightSample declines to give
        // a direction for -- that evaluator's representative point depends on N
        // and V, so there is no single L to transmit along. Declined in the
        // helper rather than approximated here.
        if (isSkinTransmitting && lightHasDirection)
        {
            // THE CAVITY WEIGHT (issue #1245) multiplies HERE and nowhere else.
            // The transmitted lobe is the only term in this shader that is not
            // already occluded — the reflected lobes carry `lightVisibility`
            // and the ambient ladder is scaled by `ao` below — so this is the
            // one place an authored occlusion can stop a closed mouth glowing
            // from inside without being applied twice.
            //
            // A zero CavityOcclusion returns exactly 1, so a version-4 profile
            // that authored only a coat transmits precisely what #1242 shipped.
            transmitted += oloSkinTransmissionDirect(N, V, lightL, lightRadiance, lightVisibility,
                                                     albedo, skinThicknessMM,
                                                     u_SkinTransmitScatter, u_SkinTransmitScaling)
                           * oloSkinOralCavityWeight(ao, skinOralLane.w);
        }
    }

    // Specular reflection source (issue #705): global prefilter at the mirror
    // direction, parallax-corrected per pixel by the distance-impostor probes
    // wherever one covers the shading point. Keeps photometric parity with
    // the deferred path (DeferredLightingShared.glsl does the same).
    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = textureLod(u_PrefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    if (u_EnableIBL == 1)
    {
        float probeViewDepth = -(u_View * vec4(v_WorldPos, 1.0)).z;
        vec4 probeSpecular = oloSampleReflectionProbes(v_WorldPos, N, R,
                                                       roughness * MAX_REFLECTION_LOD, probeViewDepth);
        prefilteredColor = mix(prefilteredColor, probeSpecular.rgb, probeSpecular.a);
    }

    // Calculate ambient lighting — the shared source ladder (AmbientLadder.glsl,
    // issue #439). A lightmapped static surface enters at the baked rung: its
    // probe/IBL indirect diffuse is REPLACED by the baked irradiance through
    // the same helpers, and IBL specular is kept (the bake is diffuse-only).
    // The branch signal is the sample's coverage (.a), never its colour — a
    // validly-baked pure-black texel keeps its darkness.
    vec4 lightmapSample = sampleLightmapIrradiance(v_TexCoord2, instances[v_InstanceIndex].LightmapScaleOffset);
    OloSurfaceLighting ambient = evaluateAmbientLadderSplit(lightmapSample, v_WorldPos, N, V, albedo,
                                         metallic, roughness, ao,
                                         u_IrradianceMap, u_BRDFLutMap, prefilteredColor);

    // Combine lighting — AO attenuates ambient only.
    //
    // The skin profile is applied to the SPECULAR half alone, immediately
    // before the two are summed (issue #1231). That ordering is the point: it is
    // the last moment at which the halves are still separable, and it is where
    // #1241's diffusion of the DIFFUSE half will go. A non-skin material
    // uploads a neutral tint, so this is a multiply by one.
    OloSurfaceLighting lighting = oloSurfaceLightingAdd(oloSurfaceLightingScale(ambient, vec3(ao)), Lo);
    lighting = oloApplySkinProfile(lighting, u_MaterialKind, u_SkinEvaluationModel,
                                   vec3(u_SkinSpecularTintR, u_SkinSpecularTintG, u_SkinSpecularTintB));
    // `transmitted` joins OUTSIDE the diffuse/specular split (issue #1242),
    // beside emissive and for the same reason: it is a third transport, not a
    // half of the BRDF.
    //
    // AND THAT PLACEMENT IS THE ENERGY ARGUMENT, not a tidiness preference. The
    // DIFFUSE half is what o_SkinDiffuse hands #1241's diffusion pass below; if
    // the transmitted term were folded in there, the diffusion pass would blur
    // and re-add energy that never travelled through the surface, and the same
    // photons would be counted by both transports. Outside the split, the
    // diffusion pass cannot see it. See Renderer/SkinTransmission.h for the
    // full three-premise argument.
    vec3 color = oloSurfaceLightingSum(lighting) + transmitted + emissive;

    // Physical transmission / IOR / volume (issue #970).
    //
    // GATED ON transmission > 0, so an ordinary material never executes a line
    // of this and its output is bit-identical to the pre-#970 shader. The
    // branch is uniform across the draw (the factor comes from the material
    // UBO, not a texture), so it costs a scalar test, not divergence.
    //
    // The transmitted radiance comes from the PREFILTERED ENVIRONMENT along the
    // refracted direction, not from a copy of the framebuffer: this engine has
    // no opaque scene-colour copy bound during the scene pass, and inventing
    // one would mean a new render pass in territory another branch owns. The
    // consequence is honest and documented -- glass shows the environment
    // rather than the geometry directly behind it -- and it is what the
    // compatibility table in docs/guides/gltf-material-extensions.md records.
    //
    // Sampling at `roughness * MAX_REFLECTION_LOD` is what makes rough glass
    // frosted, matching the mip the reflection lobe above already uses.
    // TWO TRANSMISSION CLOSURES CANNOT BOTH RUN ON ONE SURFACE (issue #1242).
    //
    // A MaterialKind::Skin material at transport version 2 already transmitted
    // above, through the thin-region term. If this material ALSO carries
    // KHR_materials_transmission the refractive closure below would add a
    // second transport over the same energy — and because it is applied LAST it
    // would dominate, which is the exact double-count the issue's third
    // criterion forbids.
    //
    // SKIN WINS, because that is what the material KIND asked for, and because
    // the refractive closure's own assumptions (a smooth dielectric interface,
    // a single refracted ray) are wrong for skin. The conflict is counted and
    // logged on the CPU as
    // SkinTransmissionFallbackReason::RefractiveTransmissionConflict, and the
    // material inspector says which term was dropped — so the author is told
    // rather than left to wonder why their transmissionFactor does nothing.
    //
    // Gated on `isSkinTransmitting` rather than on the kind alone: a skin
    // material at version 0 or 1 does not transmit here, so it must keep the
    // refractive closure it had before #1242 existed.
    if (u_TransmissionFactor > 0.0 && !isSkinTransmitting)
    {
        vec3 refractDir = oloTransmissionRefractDir(V, N, u_IOR);
        vec3 transmittedEnv = textureLod(u_PrefilterMap, refractDir, roughness * MAX_REFLECTION_LOD).rgb;
        color = oloApplyTransmission(color, transmittedEnv, albedo,
                                     u_TransmissionFactor, metallic, max(dot(N, V), 0.0), u_IOR,
                                     vec3(u_AttenuationSigmaR, u_AttenuationSigmaG, u_AttenuationSigmaB),
                                     u_ThicknessFactor);
    }

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

            // Perturb normal for crystalline micro-surface
            vec3 snowN = perturbSnowNormal(N, v_WorldPos, normalPerturbStr);

            // Recompute lighting with snow BRDF
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

    // The diffusion hand-off (issue #1241). `lighting` is the split from #1231
    // with the profile's specular tint already applied to the other half, which
    // is exactly the state the diffusion pass wants: the diffuse half alone,
    // before it was summed into `color` above.
    o_SkinDiffuse = oloSkinDiffusionOutput(lighting, u_MaterialKind, u_SkinEvaluationModel,
                                           u_SkinProfileSlot,
                                           oloSkinScatteringMask(u_MaterialKind, metallic));
    // Snow REPLACES the shaded colour rather than adding to it (the mix above),
    // so a snow-covered skin pixel's diffuse half is no longer in scene colour
    // and subtracting it would darken the snow. Hand over nothing there.
    if (snowWeight > 0.001)
        o_SkinDiffuse = vec4(0.0);

    vec3 outputN = N;
    if (snowWeight > 0.001)
    {
        outputN = normalize(mix(N, vec3(0.0, 1.0, 0.0), snowWeight * 0.6));
    }
    o_ViewNormal = octEncode(normalize(mat3(u_View) * outputN));

    // Screen-space velocity in NDC units. Matches PBR_GBuffer.glsl's
    // derivation so forward-path TAA sees identically-scaled motion
    // vectors. Static meshes report (0,0) because prevWorldPos == worldPos.
    vec2 ndcCurr = v_ClipPosCurr.xy / v_ClipPosCurr.w;
    vec2 ndcPrev = v_ClipPosPrev.xy / v_ClipPosPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
