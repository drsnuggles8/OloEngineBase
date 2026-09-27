// =============================================================================
// DeferredLightingFragment.glsl - the fragment stage of DeferredLighting.glsl,
// shared with DeferredIndirectSpecular.glsl (issue #1325).
//
// With OLO_DEFERRED_INDIRECT_SPECULAR_OUTPUT defined it writes the reflection
// tiers' input -- the indirect specular term this body composes and its
// weight -- instead of the lit colour. Everything else is the lighting stage.
// =============================================================================

#include "PBRCommon.glsl"
#include "LightProbeSampling.glsl"

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
    float u_AtlasDepthBias; // local-light atlas constant depth bias, normalized [0,1] (#1119)
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
    vec4 u_MSAAParams;       // x=SampleCount (float, >=1), y=ReSTIR DI live, z=ReSTIR GI live,
                             // w=MaterialDebugView (issue #1231)
    // Screen-space AO applied to the AMBIENT term here (issue #1336):
    //   x = 1 when u_ScreenSpaceAO multiplies the ambient split in this pass
    //       (OloEngine::SelectScreenSpaceAOApplication), 0 = no screen AO
    //   y = strength, z/w = the reconstruction projection's (2,2) / (3,2)
    //       coefficients, the pair SSAORenderPass uploads, for the upsample.
    vec4 u_ScreenAOParams;
    // x = 1 when screen-space contact shadows multiply the PRIMARY directional
    // light's visibility (Lights[0]) in this pass (issue #1336); yzw = unused.
    vec4 u_LightingFlags;
    // The skin profile table (issue #1231), indexed by the three-bit slot the
    // G-Buffer flags lane carries. MUST mirror DeferredControlsData in
    // DeferredLightingPass.cpp, which static_asserts this size.
    //   xyz = SpecularTint, LINEAR Rec.709, unitless [0,1]
    //   w   = SkinEvaluationModel as a float (exact: a small integer)
    // A slot nobody claimed stays neutral, so a stale slot reads as "no profile
    // effect" rather than as garbage.
    vec4 u_SkinProfileParams[15];
    // THE LEAF PROFILE TABLE (issue #1234) — the second tenant of the same
    // three-bit G-Buffer slot field, here for the same reason the skin table
    // above is: the transmission lobe's shape is authored per foliage LAYER,
    // and a fullscreen lighting pass has no per-layer state. Read only under
    // MaterialKind::Foliage; a pixel has one kind, so the two tables can never
    // both be consulted for one pixel. MUST mirror DeferredControlsData in
    // DeferredLightingPass.cpp, which static_asserts this size.
    //   Tint: rgb = transmission tint PRE-MULTIPLIED by strength (the same
    //         product the forward path's u_LeafTransmit.rgb carries, in the
    //         same order), w = the raw strength.
    //   Lobe: x = distortion, y = power, z = wrap, w = environment scale —
    //         exactly the `lobe` vec4 oloFoliageTransmission* takes.
    // A slot nobody claimed stays all-zero, which shades as no transmission.
    vec4 u_LeafProfileTint[15];
    vec4 u_LeafProfileLobe[15];
    // The skin transmission table (issue #1242), indexed by the same three-bit
    // slot as u_SkinProfileParams and read under the same MaterialKind::Skin
    // test. Mirrors DeferredControlsData::SkinTransmitScatter / SkinTransmitScaling.
    //
    //   Scatter: xyz = ScatterColor * Strength, w = Anisotropy
    //   Scaling: xyz = Burley scaling d (MILLIMETRES), w = Power
    vec4 u_SkinTransmitScatter[15];
    vec4 u_SkinTransmitScaling[15];
    // The layered specular table (issue #1243), indexed by the same three-bit
    // slot as u_SkinProfileParams and read under the same MaterialKind::Skin
    // test. Mirrors DeferredControlsData::SkinSpecularLobe.
    //
    //   x = LobeMix (w)              y = LobeRoughnessScale (s)
    //   z = NormalVarianceStrength   w = 0, reserved
    //
    // z is CARRIED BUT NOT READ HERE. The variance filter runs where the normal
    // is built — the G-Buffer writer — so this pass inherits an already filtered
    // roughness out of the G-Buffer and only the lobe pair is left to apply.
    // The lane is packed by the same function the forward path uses, which is
    // why it carries a field this path has no use for.
    //
    // A slot nobody claimed stays all-zero: one lobe, no filtering.
    vec4 u_SkinSpecularLobe[15];

    // The oral surface table (issue #1245), indexed by the same three-bit slot
    // as the three tables above. Mirrors DeferredControlsData::SkinOralLane.
    //
    //   x = CoatStrength   y = CoatRoughness
    //   z = CoatF0 (derived on the CPU from the authored CoatIor)
    //   w = CavityOcclusion
    //
    // ALL FOUR ARE READ HERE, unlike the lane above: the coat is a lighting-time
    // BRDF and the cavity weight gates the transmitted lobe, which this pass
    // also evaluates. So this table is the deferred path's only route to either.
    //
    // A slot nobody claimed stays all-zero: dry, transmission untouched.
    vec4 u_SkinOralLane[15];
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
#include "BindlessHeap.glsl"

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
#define u_ReSTIRDIRadiance OLO_HEAP_TEX_2D(73)            // TEX_RESTIR_DI_RADIANCE (#1140)
#define u_ReSTIRGIRadiance OLO_HEAP_TEX_2D(74)            // TEX_RESTIR_GI_RADIANCE (#1169)
#define u_ScreenSpaceAO    OLO_HEAP_TEX_2D(20)            // TEX_SSAO (#1336)
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
layout(binding = 73) uniform sampler2D u_ReSTIRDIRadiance; // ReSTIR DI resolved direct lighting (#1140)
layout(binding = 74) uniform sampler2D u_ReSTIRGIRadiance; // ReSTIR GI resolved indirect diffuse (#1169)
// Screen-space AO (SSAO / GTAO, issue #1336): visibility for the AMBIENT term,
// applied here rather than to the composed frame. Bound to white when no AO
// technique produced it; u_ScreenAOParams.x says whether it is meaningful.
layout(binding = 20) uniform sampler2D u_ScreenSpaceAO;
#endif

// Clustered light lists (issue #435) — included after the ShadowData block +
// atlas samplers so the evaluator can attenuate culled lights by their entry.
#define FPLUS_ATLAS_SHADOWS 1
#include "ForwardPlusCommon.glsl"

// Distance-impostor reflection probes (issue #705). The two cube-array slots
// (14/15) stay slot-based in BOTH variants on purpose: ReflectionProbeArray
// publishes them via PublishTextureOffsetAndBind (offset staged AND real bind
// issued), the DDGI-atlas pattern the bindless pipeline test allowlists.
#define OLO_REFLECTION_PROBE_SAMPLERS
#include "ReflectionProbes.glsl"

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
#ifdef OLO_DEFERRED_INDIRECT_SPECULAR_OUTPUT
// THE REFLECTION TIERS' INPUT (issue #1325): this stage compiled a second time,
// writing the indirect specular term it composes and that term's weight into
// IndirectSpecular instead of the lit colour. o_Color / o_SkinDiffuse become
// locals, so the body below is the lighting body, unedited.
layout(location = 0) out vec4 o_IndirectSpecular;
layout(location = 1) out vec4 o_IndirectSpecularWeight;
vec4 o_Color;
vec4 o_SkinDiffuse;
#else
layout(location = 0) out vec4 o_Color;
// The DIFFUSION HAND-OFF (issue #1241), bound to scene-colour attachment 4.
//
// LOCATION 1, NOT 4: a fragment output's location indexes the DRAW BUFFER LIST,
// not the attachment number, and DeferredLightingPass binds {attachment 0,
// attachment 4} for this draw -- it writes into the middle of the scene
// framebuffer and must not touch entity IDs, view normals or velocity, which the
// G-Buffer pass already filled. The forward shaders, whose pass binds all five
// in order, spell the same target as location 4.
layout(location = 1) out vec4 o_SkinDiffuse;
#endif

// The snow layer (issue #1451), BEFORE the shared lighting body so its snow
// terms compile in. This pass only reads a weight, so no coverage and no wind.
#define OLO_SNOW_LAYER_NO_COVERAGE 1
#include "SnowLayer.glsl"
#include "DeferredLightingShared.glsl"

#define OLO_SSAO_TAP_DEPTH(uv) texture(u_GBufferDepth, (uv)).r
#include "ScreenSpaceAOSampling.glsl"
#define OLO_CONTACT_SHADOW_TAP_DEPTH(uv) texture(u_GBufferDepth, (uv)).r
#include "ContactShadowCommon.glsl"

void main()
{
    float depth = texture(u_GBufferDepth, v_TexCoord).r;
    if (depth >= 0.999999)
    {
        o_Color = vec4(texture(u_GBufferEmissive, v_TexCoord).rgb, 1.0);
        // Sky. No surface, so no subsurface transport -- but the target still
        // has to be WRITTEN, because an MRT output left alone is undefined.
        o_SkinDiffuse = vec4(0.0);
#ifdef OLO_DEFERRED_INDIRECT_SPECULAR_OUTPUT
        // And no lobe for a reflection tier to answer for.
        o_IndirectSpecular = vec4(0.0);
        o_IndirectSpecularWeight = vec4(0.0);
#endif
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

    // THE SNOW LAYER (issue #1451). RT3.a — the material profile — carries the
    // snow weight the G-Buffer writer blended the material by, and RT1 the
    // snow-FILLED normal. The shading normal is rebuilt from that normal with
    // the same function the forward shaders call, at the ABSOLUTE position.
    float snowWeight = clamp(texture(u_GBufferVelocity, v_TexCoord).a, 0.0, 1.0);
    N = oloSnowLayerShadingNormal(N, worldPos + u_RenderOrigin, snowWeight);

    // Screen-space AO for the ambient term (issue #1336) — the same upsample
    // PostProcess_SSAOApply runs on the forward paths, handed to the ambient
    // split alone. 1.0 when no AO technique ran.
    float screenAO = 1.0;
    if (u_ScreenAOParams.x > 0.5)
        screenAO = oloScreenSpaceAOVisibility(
            oloSampleScreenSpaceAO(u_ScreenSpaceAO, v_TexCoord, u_ScreenAOParams.z, u_ScreenAOParams.w),
            u_ScreenAOParams.y);

    // Contact shadows for the primary directional light (issue #1336): a
    // visibility for THAT light, applied inside the loop — not to the frame.
    float sunContactVisibility = 1.0;
    if (u_LightingFlags.x > 0.5)
        sunContactVisibility = oloContactShadowVisibility(v_TexCoord, depth, N, gl_FragCoord.xy);

    vec3 color = ComputeDeferredLitSplit(albedo, metallic, N, roughness, ao, screenAO, sunContactVisibility,
                                         emissive, worldPos, bakedGI, snowWeight, o_SkinDiffuse);

    o_Color = vec4(color, 1.0);
#ifdef OLO_DEFERRED_INDIRECT_SPECULAR_OUTPUT
    o_IndirectSpecular = vec4(g_OloIndirectSpecular, 1.0);
    o_IndirectSpecularWeight = vec4(g_OloIndirectSpecularWeight, 1.0);
#endif
}
