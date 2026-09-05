// =============================================================================
// DeferredLighting.glsl - Deferred lighting composition pass (full)
// Part of OloEngine Deferred Renderer
//
// Reads the G-Buffer produced by PBR_GBuffer{,_Skinned}.glsl and
// evaluates opaque PBR lighting for every visible fragment. Output is
// linear HDR RGBA16F ready for post-processing.
//
// Expected texture bindings (match ShaderBindingLayout TEX_GBUFFER_*):
//   slot 43 — RT0  RGBA8   albedo.rgb + metallic
//   slot 44 — RT1  RGBA16F octNormal.xy + roughness + AO
//   slot 45 — RT2  RGBA16F emissive.rgb + material flags
//   slot 46 — RT3  RG16F   screen-space velocity (unused in lighting)
//   slot 47 — depth (D32F)
//   slot 69 — RT5  RGBA16F baked lightmap irradiance + coverage (issue #865)
//
// The per-pixel shading body lives in include/DeferredLightingShared.glsl
// so the MSAA variant (DeferredLighting_MSAA.glsl) can re-use the exact same
// math on per-sample inputs without code drift.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
// the pipeline has no vertex-input state at all. Binding 57 is the engine-wide
// vertex-pull binding (ShaderBindingLayout::SSBO_VERTEX_PULL); the root struct
// carries this buffer's device address, so the SAME 20-byte
// {vec3 position, vec2 uv} stream the attribute path consumes is read by index
// instead. OLO_VULKAN is defined only on the Vulkan shaderc route; the GL
// branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    int base = gl_VertexIndex * 5;
    vec3 position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
    v_TexCoord = vec2(b_Vertices.v[base + 3], b_Vertices.v[base + 4]);
    gl_Position = vec4(position, 1.0);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}
#endif

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"
#include "include/LightProbeSampling.glsl"

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // std140 padding so u_RenderOrigin lands at offset 272 (matches the shared
    // CameraMatrices layout). Named distinctly from the binding-8 MotionBlur
    // block's u_PrevViewProjection to avoid a nameless-block global-scope
    // collision; the previous-frame VP is unused in the deferred lit pass.
    mat4 _camPrevViewProjectionPad;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
};

// MultiLight UBO (binding 5)
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
    // Hybrid ray-traced shadow routing (issue #1056). Which light index reads
    // which channel of u_RayTracedShadowMask; -1 = the channel is unassigned.
    // Written by RayTracedShadowPass AFTER its draws, so a frame where the
    // trace did not run leaves x below at 0 and this whole branch off.
    ivec4 u_RayTracedShadowLightIndices;
    vec4 u_RayTracedShadowParams; // x = mask active, yzw reserved
};

// MotionBlur UBO (binding 8) for u_InverseViewProjection.
layout(std140, binding = 8) uniform MotionBlurMatrices {
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

// Per-pass deferred controls UBO (binding 30).
layout(std140, binding = 30) uniform DeferredLightingControls {
    vec4 u_DeferredControls; // x=EnableIBL, y=EnableLightProbes, z=IBLIntensity, w=CascadeDebug
    vec4 u_MSAAParams;       // x=SampleCount (float, >=1), yzw reserved
};

// Texture inputs. Under heap-bindless (issue #691) every one of these
// becomes a heap lookup keyed by the SAME slot number the bindful branch
// declares, so the two variants cannot disagree about which texture is which,
// and the shader BODY below is byte-identical between them.
//
// DeferredLightingPass stages all thirteen through the seam and calls
// FlushHeapOffsets() before its fullscreen draw, so §5c's "a C++ bind and its
// declaration move together" holds for the whole file — it converts whole or
// not at all, which is why there is one #ifdef block and not three.
#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
// IBL cubemaps.
#define u_IrradianceMap  OLO_HEAP_TEX_CUBE(10)             // TEX_USER_0
#define u_PrefilterMap   OLO_HEAP_TEX_CUBE(11)             // TEX_USER_1
#define u_BRDFLutMap     OLO_HEAP_TEX_2D(12)               // TEX_USER_2
// Shadow maps — identical slots to PBR_MultiLight (CSM + atlas, issue #435).
#define u_ShadowMapCSM   OLO_HEAP_TEX_2D_ARRAY_SHADOW(8)   // TEX_SHADOW
#define u_ShadowAtlas    OLO_HEAP_TEX_2D_ARRAY_SHADOW(13)  // TEX_SHADOW_ATLAS
// Comparison-OFF raw-depth views of the textures above for the PCSS blocker search.
#define u_ShadowMapCSMRaw OLO_HEAP_TEX_2D_ARRAY(33)        // TEX_SHADOW_CSM_RAW
#define u_ShadowAtlasRaw  OLO_HEAP_TEX_2D_ARRAY(34)        // TEX_SHADOW_ATLAS_RAW
// Ray-traced shadow visibility mask (issue #1056) — one channel per light.
#define u_RayTracedShadowMask OLO_HEAP_TEX_2D(72)          // TEX_RAY_TRACED_SHADOW
#else
// IBL cubemaps.
layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D   u_BRDFLutMap;

// Shadow maps — identical slots to PBR_MultiLight (CSM + atlas, issue #435).
layout(binding = 8)  uniform sampler2DArrayShadow u_ShadowMapCSM;
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowAtlas;
// Comparison-OFF raw-depth views of the textures above for the PCSS blocker search.
layout(binding = 33) uniform sampler2DArray u_ShadowMapCSMRaw;
layout(binding = 34) uniform sampler2DArray u_ShadowAtlasRaw;
// Ray-traced shadow visibility mask (issue #1056) — one channel per light,
// 1 = lit. Always bound (to an opaque white 1x1 when the pass did not run), so
// the sampler can never dangle; the ROUTING above, not the texture, is what
// says whether it is meaningful.
layout(binding = 72) uniform sampler2D u_RayTracedShadowMask;
#endif

// Clustered light lists (issue #435) — included after the ShadowData block +
// atlas samplers so the evaluator can attenuate culled lights by their entry.
#define FPLUS_ATLAS_SHADOWS 1
#include "include/ForwardPlusCommon.glsl"

// Distance-impostor reflection probes (issue #705). The two cube-array slots
// (14/15) stay slot-based in BOTH variants on purpose: ReflectionProbeArray
// publishes them via PublishTextureOffsetAndBind (offset staged AND real bind
// issued), the DDGI-atlas pattern the bindless pipeline test allowlists.
#define OLO_REFLECTION_PROBE_SAMPLERS
#include "include/ReflectionProbes.glsl"

// G-Buffer samplers (non-MSAA variant).
#ifdef OLO_BINDLESS
#define u_GBufferAlbedo   OLO_HEAP_TEX_2D(43)  // TEX_GBUFFER_ALBEDO
#define u_GBufferNormal   OLO_HEAP_TEX_2D(44)  // TEX_GBUFFER_NORMAL
#define u_GBufferEmissive OLO_HEAP_TEX_2D(45)  // TEX_GBUFFER_EMISSIVE
#define u_GBufferVelocity OLO_HEAP_TEX_2D(46)  // TEX_GBUFFER_VELOCITY
#define u_GBufferDepth    OLO_HEAP_TEX_2D(47)  // TEX_GBUFFER_DEPTH
#define u_GBufferBakedGI  OLO_HEAP_TEX_2D(69)  // TEX_GBUFFER_BAKEDGI
#else
layout(binding = 43) uniform sampler2D u_GBufferAlbedo;
layout(binding = 44) uniform sampler2D u_GBufferNormal;
layout(binding = 45) uniform sampler2D u_GBufferEmissive;
layout(binding = 46) uniform sampler2D u_GBufferVelocity;
layout(binding = 47) uniform sampler2D u_GBufferDepth;
layout(binding = 69) uniform sampler2D u_GBufferBakedGI;
#endif

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

#include "include/DeferredLightingShared.glsl"

void main()
{
    float depth = texture(u_GBufferDepth, v_TexCoord).r;
    if (depth >= 0.999999)
    {
        o_Color = vec4(texture(u_GBufferEmissive, v_TexCoord).rgb, 1.0);
        return;
    }

    vec4 gAlbedo    = texture(u_GBufferAlbedo,   v_TexCoord);
    vec4 gNormal    = texture(u_GBufferNormal,   v_TexCoord);
    vec4 gEmissive  = texture(u_GBufferEmissive, v_TexCoord);
    // The .a of RT2 is the material-flags BITFIELD, not radiometry (issue
    // #996): it must never be interpolated between two texels, because a
    // blend of two valid codes is a third code nobody wrote. RGB keeps the
    // filtered fetch above (it IS radiometry, and it is what stays
    // byte-identical); the lane is taken by texel here.
    //
    // The texel comes from v_TexCoord and textureSize rather than
    // gl_FragCoord, so this stays a nearest fetch of the right texel even if
    // the lighting viewport ever stops being 1:1 with the G-Buffer (dynamic
    // resolution scaling) — the previous filtered read was correct only by
    // that alignment, and nothing enforces it.
    {
        ivec2 flagSize = textureSize(u_GBufferEmissive, 0);
        ivec2 flagTexel = clamp(ivec2(v_TexCoord * vec2(flagSize)), ivec2(0), flagSize - ivec2(1));
        gEmissive.a = texelFetch(u_GBufferEmissive, flagTexel, 0).a;
    }

    vec3 albedo    = gAlbedo.rgb;
    float metallic = gAlbedo.a;
    vec3 N         = OctDecodeGB(gNormal.xy);
    float roughness = max(gNormal.z, MIN_ROUGHNESS);
    float ao       = gNormal.w;
    // Pass emissive RGB + flag alpha through; ComputeDeferredLit handles the
    // unlit fast-path when emissive.a > 0.5.
    vec4 emissive  = gEmissive;

    // ReconstructWorldPosGB inverts the WORLD view-projection (binding 8), so it
    // returns an ABSOLUTE world position (issue #429). The deferred lit pass reads
    // render-RELATIVE camera / lights / shadow matrices / probe bounds (all shifted
    // by the render origin), so bring the reconstructed position into the same
    // relative space before lighting. No-op near origin (u_RenderOrigin == 0).
    vec3 worldPos = ReconstructWorldPosGB(v_TexCoord, depth) - u_RenderOrigin;
    // RT5: baked lightmap irradiance + coverage (issue #865). A zero bind, an
    // unbaked scene, or a draw with no atlas region all arrive here as vec4(0),
    // which the ladder reads as "no baked GI" and falls through to probes/IBL.
    vec4 bakedGI = texture(u_GBufferBakedGI, v_TexCoord);

    vec3 color = ComputeDeferredLit(albedo, metallic, N, roughness, ao, emissive, worldPos, bakedGI);

    o_Color = vec4(color, 1.0);
}
