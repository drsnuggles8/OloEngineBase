// =============================================================================
// DeferredLighting_MSAA.glsl — per-sample deferred lighting composition.
//
// Selected by DeferredLightingPass when GBuffer::GetSampleCount() > 1 AND
// DeferredSettings::PerSampleLighting is true. Samples each G-Buffer
// attachment with sampler2DMS / texelFetch per sub-sample, evaluates full
// PBR lighting per sample, and averages the final HDR colour. This avoids
// the shading-rate collapse of a resolve-before-light approach where MSAA
// would only affect geometric edge samples of the depth/normal during
// G-Buffer write but not the shading itself.
//
// Shares the per-pixel shading body with the non-MSAA variant via
// include/DeferredLightingShared.glsl so there is a single source of truth
// for the PBR math.
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

layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int u_MaxLights;
    int u_ShadowCasterCount;
    int u_DirectionalLightCount;
    LightData u_Lights[MAX_LIGHTS];
};

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

layout(std140, binding = 8) uniform MotionBlurMatrices {
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

// Per-pass deferred controls UBO (binding 30) — same layout as non-MSAA
// variant so a single C++ upload works for both shader variants.
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
    vec4 u_SkinProfileParams[7];
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
    vec4 u_LeafProfileTint[7];
    vec4 u_LeafProfileLobe[7];
    // The skin transmission table (issue #1242), indexed by the same three-bit
    // slot as u_SkinProfileParams and read under the same MaterialKind::Skin
    // test. Mirrors DeferredControlsData::SkinTransmitScatter / SkinTransmitScaling.
    //
    //   Scatter: xyz = ScatterColor * Strength, w = Anisotropy
    //   Scaling: xyz = Burley scaling d (MILLIMETRES), w = Power
    vec4 u_SkinTransmitScatter[7];
    vec4 u_SkinTransmitScaling[7];
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
    vec4 u_SkinSpecularLobe[7];

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
    vec4 u_SkinOralLane[7];
};

layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D   u_BRDFLutMap;

layout(binding = 8)  uniform sampler2DArrayShadow u_ShadowMapCSM;
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowAtlas;
// Comparison-OFF raw-depth views of the textures above for the PCSS blocker search.
layout(binding = 33) uniform sampler2DArray u_ShadowMapCSMRaw;
layout(binding = 34) uniform sampler2DArray u_ShadowAtlasRaw;
// Ray-traced shadow visibility mask (issue #1056) — one channel per light,
// 1 = lit. Always bound (to an opaque white 1x1 when the pass did not run), so
// the sampler can never dangle; the ROUTING in ShadowData, not the texture, is
// what says whether it is meaningful.
layout(binding = 72) uniform sampler2D u_RayTracedShadowMask;
layout(binding = 73) uniform sampler2D u_ReSTIRDIRadiance; // ReSTIR DI resolved direct lighting (#1140)
layout(binding = 74) uniform sampler2D u_ReSTIRGIRadiance; // ReSTIR GI resolved indirect diffuse (#1169)
// Screen-space AO (SSAO / GTAO, issue #1336): visibility for the AMBIENT term,
// applied here rather than to the composed frame. Single-sample, like the AO
// pass that wrote it. Bound to white when no AO technique produced it.
layout(binding = 20) uniform sampler2D u_ScreenSpaceAO;

// Clustered light lists (issue #435) — included after the ShadowData block +
// atlas samplers so the evaluator can attenuate culled lights by their entry.
#define FPLUS_ATLAS_SHADOWS 1
#include "include/ForwardPlusCommon.glsl"

// Distance-impostor reflection probes (issue #705) — slot-based like the
// rest of this variant; published by ReflectionProbeArray::BindForShading.
#define OLO_REFLECTION_PROBE_SAMPLERS
#include "include/ReflectionProbes.glsl"

// G-Buffer samplers — MSAA variant uses sampler2DMS (no implicit filtering;
// must use texelFetch per-sample).
layout(binding = 43) uniform sampler2DMS u_GBufferAlbedo;
layout(binding = 44) uniform sampler2DMS u_GBufferNormal;
layout(binding = 45) uniform sampler2DMS u_GBufferEmissive;
layout(binding = 46) uniform sampler2DMS u_GBufferVelocity;
layout(binding = 47) uniform sampler2DMS u_GBufferDepth;
// RT5 — baked lightmap irradiance + coverage (issue #865). Fetched per SAMPLE
// like the rest: resolving it first would average two charts' coverage across a
// silhouette and shade the pixel from a blend the geometry never had.
layout(binding = 69) uniform sampler2DMS u_GBufferBakedGI;

layout(location = 0) in vec2 v_TexCoord;
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

#include "include/DeferredLightingShared.glsl"

// The AO buffer is single-sample, so its depth-aware upsample weighs against
// sample 0 — the same sample the resolved depth the forward consumer reads
// would have taken its first contribution from. The contact-shadow march reads
// the same tap.
float oloMSAASampleZeroDepth(vec2 uv)
{
    ivec2 size = textureSize(u_GBufferDepth);
    return texelFetch(u_GBufferDepth, clamp(ivec2(uv * vec2(size)), ivec2(0), size - ivec2(1)), 0).r;
}
#define OLO_SSAO_TAP_DEPTH(uv) oloMSAASampleZeroDepth(uv)
#include "include/ScreenSpaceAOSampling.glsl"
#define OLO_CONTACT_SHADOW_TAP_DEPTH(uv) oloMSAASampleZeroDepth(uv)
#include "include/ContactShadowCommon.glsl"

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int sampleCount = max(int(u_MSAAParams.x + 0.5), 1);

    // Fast-path: if every sample reports far-plane depth (no geometry
    // written this pixel), this is pure skybox — just average the skybox
    // samples written during forward overlay / pre-lit emissive.
    bool anyGeom = false;
    for (int s = 0; s < sampleCount; ++s)
    {
        if (texelFetch(u_GBufferDepth, pixel, s).r < 0.999999)
        {
            anyGeom = true;
            break;
        }
    }
    if (!anyGeom)
    {
        vec3 emissiveSum = vec3(0.0);
        for (int s = 0; s < sampleCount; ++s)
            emissiveSum += texelFetch(u_GBufferEmissive, pixel, s).rgb;
        o_Color = vec4(emissiveSum / float(sampleCount), 1.0);
        // All sky. No surface, so no subsurface transport -- but the target
        // still has to be WRITTEN: an MRT output left alone is undefined.
        o_SkinDiffuse = vec4(0.0);
        return;
    }

    // Shade every sample and average. Background (depth==1.0) samples
    // contribute their raw emissive so silhouette pixels where some
    // samples fell outside geometry still anti-alias correctly against
    // the sky / background emissive.
    // Screen-space AO for the ambient term (issue #1336): one value per PIXEL,
    // shared by every sample, because the AO pass that wrote it is single-sample.
    float screenAO = 1.0;
    if (u_ScreenAOParams.x > 0.5)
        screenAO = oloScreenSpaceAOVisibility(
            oloSampleScreenSpaceAO(u_ScreenSpaceAO, v_TexCoord, u_ScreenAOParams.z, u_ScreenAOParams.w),
            u_ScreenAOParams.y);

    // Contact shadows for the primary directional light (issue #1336), one
    // march per PIXEL from sample 0's surface, shared by every sample — the
    // same resolution the post pass it replaced marched at.
    float sunContactVisibility = 1.0;
    if (u_LightingFlags.x > 0.5)
    {
        float contactDepth = texelFetch(u_GBufferDepth, pixel, 0).r;
        vec3 contactN = OctDecodeGB(texelFetch(u_GBufferNormal, pixel, 0).xy);
        sunContactVisibility = oloContactShadowVisibility(v_TexCoord, contactDepth, contactN, gl_FragCoord.xy);
    }

    vec3 accum = vec3(0.0);
    // The diffusion hand-off accumulates per sample exactly as the colour does,
    // so a silhouette pixel hands over the average of the samples that ARE skin
    // rather than whichever sample happened to be last (issue #1241).
    vec4 skinDiffuseAccum = vec4(0.0);
    for (int s = 0; s < sampleCount; ++s)
    {
        float depth = texelFetch(u_GBufferDepth, pixel, s).r;
        vec3 emissiveSample = texelFetch(u_GBufferEmissive, pixel, s).rgb;
        if (depth >= 0.999999)
        {
            accum += emissiveSample;
            continue;
        }

        vec4 gAlbedo = texelFetch(u_GBufferAlbedo, pixel, s);
        vec4 gNormal = texelFetch(u_GBufferNormal, pixel, s);
        // Re-fetch emissive with alpha so the unlit-flag path can trigger
        // per-sample. (We sampled RGB above for the skybox fast-path but
        // need .a for PBR-vs-unlit selection inside ComputeDeferredLit.)
        vec4 emissiveFlags = texelFetch(u_GBufferEmissive, pixel, s);

        vec3 albedo     = gAlbedo.rgb;
        float metallic  = gAlbedo.a;
        vec3 N          = OctDecodeGB(gNormal.xy);
        float roughness = max(gNormal.z, MIN_ROUGHNESS);
        float ao        = gNormal.w;

        // Reconstruct world position using per-sample depth + pixel-center
        // UV. Sub-pixel positional jitter is absorbed into the shading —
        // per-sample depth is what disambiguates near-silhouette samples.
        // ReconstructWorldPosGB returns an ABSOLUTE world position (world
        // inverse-VP); bring it into the render-RELATIVE space the lit inputs
        // use (issue #429). No-op near origin (u_RenderOrigin == 0).
        vec3 worldPos = ReconstructWorldPosGB(v_TexCoord, depth) - u_RenderOrigin;

        vec4 bakedGI = texelFetch(u_GBufferBakedGI, pixel, s);

        vec4 sampleSkinDiffuse;
        accum += ComputeDeferredLitSplit(albedo, metallic, N, roughness, ao, screenAO, sunContactVisibility,
                                         emissiveFlags, worldPos, bakedGI, sampleSkinDiffuse);
        skinDiffuseAccum += sampleSkinDiffuse;
    }

    vec3 color = accum / float(sampleCount);
    o_Color = vec4(color, 1.0);
    // The RADIANCE averages, but the SLOT lane does not: it is an identity, and
    // the mean of two slot codes is a third code nobody wrote. Take the code the
    // accumulated lane rounds to -- that is the slot a majority of the samples
    // carried, and "no diffusion" when none did.
    skinDiffuseAccum /= float(sampleCount);
    int resolvedSlot = oloSkinDiffusionSlot(skinDiffuseAccum.a);
    o_SkinDiffuse = (resolvedSlot >= OLO_SKIN_DIFFUSE_SLOT_NONE)
                        ? vec4(0.0)
                        : vec4(skinDiffuseAccum.rgb, oloSkinDiffusionEncodeSlot(resolvedSlot));
}
