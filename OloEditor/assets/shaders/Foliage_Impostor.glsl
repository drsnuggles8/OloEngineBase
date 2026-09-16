// =============================================================================
// Foliage_Impostor.glsl — octahedral impostor card for distant foliage (issue
// #433), FORWARD variant. Replaces the flat Y-rotated billboard with a fully
// camera-facing card that samples a baked octahedral atlas (albedo +
// object-normal + depth), blending the 3 lattice frames around the current view
// direction (no slice pop), applying single-step depth parallax (kills the
// flat-card look), and relighting from the baked object normal. A
// distance-driven detail ramp across [ImpostorStart, ImpostorStart+Band] fades
// parallax + cross-frame blend in with range so there is no visible transition.
// Forward pass: composites into SceneColor after opaque.
//
// The card's placement (vertex stage) and its atlas sampling + discard rule
// (fragment body) live in shared includes with the deferred sibling,
// Foliage_Impostor_GBuffer.glsl (#1225): the two paths must place and sample
// the same card or it jumps at the Forward/Deferred seam. This file owns only
// its OUTPUT — the self-relight and SceneColor write.
// =============================================================================

#type vertex
#version 460 core

// Nothing in this fragment stage reads the instance index — declare no
// varying for it (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning). The deferred sibling omits this define.
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/FoliageImpostorVertexStage.glsl"

#type fragment
#version 460 core

layout(location = 0) out vec4 FragColor;
layout(location = 3) out vec2 o_Velocity;
// Scene FB RT4: the diffuse half of a SKIN pixel's lighting, for the screen-space
// diffusion pass (issue #1241). This surface never shades skin, so it writes the
// "no diffusion here" code -- but it must WRITE it: an MRT output a shader leaves
// alone is undefined, not zero, and SkinDiffusion.glsl would blur the garbage
// into scene colour. See include/PBRCommon.glsl, "THE DIFFUSION HAND-OFF".
layout(location = 4) out vec4 o_SkinDiffuse;


layout(location = 0) in vec3 v_CardWorld;
layout(location = 1) in vec3 v_PivotWorld;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Rotation;
layout(location = 6) in vec3 v_PrevCardWorld;
layout(location = 7) in float v_Radius;
layout(location = 2) in float v_MeshCoverage; // WORLD-space card radius

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

// PBRCommon first — LightData, MAX_LIGHTS and the BRDF come from it, and the
// light block below is declared in terms of its LightData struct.
#include "include/PBRCommon.glsl"
#include "include/VirtualShadowSampling.glsl"
// The vegetation material's LOBE half (issue #1234). No sampling half: an
// impostor has no leaf maps — its atlas baked them in — so it takes the lobe
// parameters and a constant thickness from the UBO, exactly as its deferred
// sibling does.
#include "include/FoliageSurface.glsl"

// Multi-Light UBO (binding 5) — THE FULL BLOCK, matching PBR_MultiLight.glsl and
// Foliage_Instance.glsl. It used to be a four-int header plus Light[0]: a
// truncated view of the same buffer that made the other 255 lights unreachable
// on the impostor card while the near-field card could see them, which is a
// per-distance divergence #1234's third and fourth criteria both rule out.
layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int u_MaxLights;
    int u_ShadowCasterCount;
    int u_DirectionalLightCount;
    LightData u_Lights[MAX_LIGHTS];
};

// Shadow UBO (binding 6) — declared exactly as PBR_MultiLight.glsl declares it.
layout(std140, binding = 6) uniform ShadowData {
    mat4 u_DirectionalLightSpaceMatrices[4];
    vec4 u_CascadePlaneDistances;
    vec4 u_ShadowParams;
    mat4 u_AtlasEntryMatrices[48];
    vec4 u_AtlasEntryScaleOffset[48];
    int u_DirectionalShadowEnabled;
    int u_AtlasEntryCount;
    int u_ShadowMapResolution;
    int u_AtlasResolution;
    int u_CascadeDebugEnabled;
    int u_SoftShadowMode;
    float u_AtlasDepthBias;
    int _shadowPad2;
};

#ifdef OLO_BINDLESS
#define u_ShadowMapCSM OLO_HEAP_TEX_2D_ARRAY_SHADOW(8)
#define u_ShadowAtlas OLO_HEAP_TEX_2D_ARRAY_SHADOW(13)
#define u_ShadowMapCSMRaw OLO_HEAP_TEX_2D_ARRAY(33)
#define u_ShadowAtlasRaw OLO_HEAP_TEX_2D_ARRAY(34)
#define u_IrradianceMap OLO_HEAP_TEX_CUBE(10)  // TEX_USER_0
#define u_PrefilterMap OLO_HEAP_TEX_CUBE(11)   // TEX_USER_1
#define u_BRDFLutMap OLO_HEAP_TEX_2D(12)       // TEX_USER_2
#else
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM;  // TEX_SHADOW
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowAtlas;  // TEX_SHADOW_ATLAS
layout(binding = 33) uniform sampler2DArray u_ShadowMapCSMRaw;    // TEX_SHADOW_CSM_RAW
layout(binding = 34) uniform sampler2DArray u_ShadowAtlasRaw;     // TEX_SHADOW_ATLAS_RAW
layout(binding = 10) uniform samplerCube u_IrradianceMap;  // TEX_USER_0
layout(binding = 11) uniform samplerCube u_PrefilterMap;   // TEX_USER_1
layout(binding = 12) uniform sampler2D u_BRDFLutMap;       // TEX_USER_2
#endif

layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float u_PrevTime;
    float _foliagePad1;
    vec3 u_FoliageBaseColor;
    float _foliagePad2;
    vec4 u_ImpostorParams0; // x=framesPerAxis, y=hemi, z=startDistance, w=transitionBand
    vec4 u_ImpostorParams1;
    vec4 u_MeshParams; // issue #1233 — see FoliageInstanceGeometry.glsl
    vec4 u_MeshViewPos; // see ShaderBindingLayout::FoliageUBO // x=enabled, y=meshRadius, z=parallaxScale, w=unused
    // Leaf material (issue #1234) — see ShaderBindingLayout::FoliageUBO. The
    // block is declared identically in every stage of every foliage program:
    // std140 blocks must match across the stages of one program, so a lane
    // appended to one declaration and not the others is a LINK failure, not a
    // wrong pixel.
    vec4 u_LeafSurface;   // x=roughness y=normalStrength z=thicknessScale w=mapFlags
    vec4 u_LeafTransmit;  // rgb=tint*strength w=strength (0 == not a leaf material)
    vec4 u_LeafLobe;      // x=distortion y=power z=wrap w=environment scale
    vec4 u_LeafIds;       // x = leaf-profile slot for the deferred lighting pass
};

#include "include/FoliageImpostorSampling.glsl"

void main()
{
    ImpostorSample card = SampleImpostorCard();

    // Relight from the baked object-space normal (dynamic sun direction),
    // through the SAME evaluation the near-field card and the authored mesh use
    // (issue #1234): the full light loop, real shadows, image-based ambient and
    // the two-sided transmission lobe. The `* 0.5` back-face hack this replaced
    // was the impostor's own private idea of two-sidedness, and the near card
    // had a different one.
    vec3 geometricN = normalize(rotateY(card.LocalNormal, v_Rotation));
    vec3 V = normalize(u_CameraPosition - v_CardWorld);
    // The one shared rule for which way a leaf faces. Same function, same
    // argument order, same answer as the instance shaders.
    vec3 N = oloFoliageFaceNormal(geometricN, V);

    const float metallic = 0.0;
    const float ao = 1.0;
    float roughness = clamp(u_LeafSurface.x, 0.02, 1.0);

    bool isLeaf = u_LeafTransmit.w > 0.0;
    vec3 leafTint = u_LeafTransmit.rgb; // ALREADY tint * strength
    // Constant across the card — an impostor atlas has no thickness channel;
    // see Foliage_Impostor_GBuffer.glsl for why that is the representation's
    // property rather than a gap.
    float leafThickness = isLeaf ? clamp(u_LeafSurface.z, 0.0, 1.0) : 0.0;

    float viewDepth = (u_View * vec4(v_CardWorld, 1.0)).z;

    OloSurfaceLighting Lo = oloSurfaceLightingZero();
    vec3 transmitted = vec3(0.0);

    int lightCount = min(u_LightCount, MAX_LIGHTS);
    for (int i = 0; i < lightCount; ++i)
    {
        int lightType = int(u_Lights[i].position.w);

        vec3 L;
        vec3 radiance;
        bool hasDirection = oloLightSample(u_Lights[i], v_CardWorld, L, radiance);

        // ONE visibility factor, biased along the LIT-SIDE normal, serving both
        // lobes — see oloFoliageShadowNormal.
        vec3 Ns = hasDirection ? oloFoliageShadowNormal(N, L) : N;
        float shadow = 1.0;

        if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
            if (VSM_ENABLED != 0)
            {
                shadow = vsmShadowFactor(v_CardWorld, Ns);
            }
            else
            {
                shadow = calculateCascadedShadowFactorCSM(
                    u_ShadowMapCSM, u_ShadowMapCSMRaw, v_CardWorld, Ns, viewDepth,
                    u_DirectionalLightSpaceMatrices, u_CascadePlaneDistances,
                    u_ShadowParams, u_ShadowMapResolution, u_SoftShadowMode);
            }
        }
        else if (lightType == SPOT_LIGHT)
        {
            int atlasEntry = int(u_Lights[i].direction.w);
            float localShadow;
            if (vsmLocalShadow(v_CardWorld, Ns, atlasEntry, false, localShadow))
            {
                shadow = localShadow;
            }
            else if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
            {
                shadow = calculateAtlasEntryShadow(
                    v_CardWorld, u_AtlasEntryMatrices[atlasEntry], u_AtlasEntryScaleOffset[atlasEntry],
                    u_ShadowAtlas, u_ShadowAtlasRaw, u_AtlasDepthBias, u_AtlasResolution,
                    u_SoftShadowMode, u_ShadowParams.z);
            }
        }
        else if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
        {
            int baseEntry = int(u_Lights[i].direction.w);
            float localShadow;
            if (vsmLocalShadow(v_CardWorld, Ns, baseEntry, true, localShadow))
            {
                shadow = localShadow;
            }
            else if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
            {
                int entry = baseEntry + atlasCubeFace(v_CardWorld - u_Lights[i].position.xyz);
                shadow = calculateAtlasEntryShadow(
                    v_CardWorld, u_AtlasEntryMatrices[entry], u_AtlasEntryScaleOffset[entry],
                    u_ShadowAtlas, u_ShadowAtlasRaw, u_AtlasDepthBias, u_AtlasResolution,
                    0, u_ShadowParams.z);
            }
        }

        OloSurfaceLighting contrib = calculateLightContributionSplit(
            u_Lights[i], N, V, card.Albedo, metallic, roughness, v_CardWorld, OLO_PBR_MODEL_LEGACY);
        Lo = oloSurfaceLightingAdd(Lo, oloSurfaceLightingScale(contrib, vec3(shadow)));

        if (isLeaf && hasDirection)
        {
            transmitted += oloFoliageTransmissionDirect(N, V, L, radiance, shadow, leafThickness,
                                                        leafTint, u_LeafLobe);
        }
    }

    // Same IBL-bound test and same fallback as Foliage_Instance.glsl — see the
    // comment there for why a typed-null cubemap read as black is not an
    // acceptable substitute for "there is no environment".
    vec3 ambient;
    if (u_LeafIds.y > 0.5)
    {
        vec3 prefilteredColor = textureLod(u_PrefilterMap, reflect(-V, N),
                                           roughness * MAX_REFLECTION_LOD).rgb;
        ambient = calculateCombinedAmbientPrefiltered(
                      texture(u_IrradianceMap, N).rgb, N, V, card.Albedo, metallic, roughness,
                      u_BRDFLutMap, prefilteredColor) *
                  u_LeafIds.z;
    }
    else
    {
        ambient = calculateSimpleAmbient(card.Albedo, metallic, ao);
    }

    if (isLeaf && u_LeafIds.y > 0.5)
    {
        transmitted += oloFoliageTransmissionAmbient(
            leafThickness, leafTint, texture(u_IrradianceMap, -N).rgb * u_LeafIds.z, u_LeafLobe);
    }

    vec3 litColor = ambient * ao + oloSurfaceLightingSum(Lo) + transmitted;

    // Foliage blends are OFF (opaque alpha-tested), so this alpha is never
    // seen; the visible fade is the discard SampleImpostorCard applies.
    FragColor = vec4(litColor, card.Coverage * card.DistFade);

    // Camera-motion velocity (impostor has no per-instance prev history).
    vec4 clipCurr = u_ViewProjection * vec4(v_CardWorld, 1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevCardWorld, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
    o_SkinDiffuse = vec4(0.0); // not skin -- see the declaration above (#1241)
}
