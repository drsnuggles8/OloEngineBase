// =============================================================================
// Foliage_Instance.glsl - Instanced foliage rendering with wind animation
// Uses per-instance data for position, scale, rotation, and tint
// Supports alpha-to-coverage for grass/vegetation cutouts
//
// Draws the layer's flat card AND, up close, its authored plant mesh (issue
// #1233) — both from the shared vertex stage below, so the forward, deferred
// and shadow programs cannot place the same plant differently.
//
// LIGHTING, SINCE ISSUE #1234. This used to be one directional light, a
// half-strength back-face hack and a flat `albedo * 0.3` ambient — the "basic
// forward foliage lighting" the issue replaces. It is now the real thing: the
// full multi-light loop, CSM / virtual / atlas shadows, image-based ambient,
// and the two-sided leaf transmission lobe. Every surface attribute and the
// lobe itself come from include/FoliageSurface.glsl, which the DEFERRED
// programs call with the same arguments — that shared call, not a convention,
// is what makes the issue's third criterion (same material meaning, same normal
// orientation across forward / forward+ / deferred) hold.
//
// FORWARD AND FORWARD+ ARE THE SAME PROGRAM HERE. SelectFoliageRenderStream
// routes both to FoliageRenderPass with this shader
// (Renderer3DSpecializedDraws.cpp), so "forward+ preserves the same material
// meaning as forward" is not a claim about two implementations agreeing — there
// is one. Foliage does not read the clustered light list; it walks the same
// multi-light UBO on both, which is why the two cells are identical by
// construction.
// =============================================================================

#type vertex
#version 460 core

// This shader's consuming stage never reads v_InstanceIndex — declare no
// varying (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/FoliageInstanceVertexStage.glsl"

#type fragment
#version 460 core

layout(location = 0) out vec4 FragColor;
// Scene FB RT3 velocity. Captures camera, per-instance motion, AND the
// per-fragment wind-sway reprojection (via v_PrevWorldPos from the vertex
// stage, which re-evaluates the wind function at `t - dt`).
layout(location = 3) out vec4 o_Velocity;
// Scene FB RT4: the diffuse half of a SKIN pixel's lighting, for the screen-space
// diffusion pass (issue #1241). This surface never shades skin, so it writes the
// "no diffusion here" code -- but it must WRITE it: an MRT output a shader leaves
// alone is undefined, not zero, and SkinDiffusion.glsl would blur the garbage
// into scene colour. See include/PBRCommon.glsl, "THE DIFFUSION HAND-OFF".
layout(location = 4) out vec4 o_SkinDiffuse;


// Inputs
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_Color;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Fade;
layout(location = 6) in vec3 v_PrevWorldPos;
layout(location = 7) in float v_MeshCoverage;
layout(location = 8) in float v_InstanceSeed; // this plant's own draw (issue #1237)

// Camera UBO (binding 0)
// The shared camera block (include/CameraCommon.glsl), identical in every
// stage of every program that includes this — GL links a program only if
// its stages agree on the block — and carrying the forward screen-space AO
// lane (issue #1452).
#include "include/CameraCommon.glsl"

#include "include/BindlessHeap.glsl"

// PBRCommon first — LightData, MAX_LIGHTS, the BRDF, the CSM sampler and the
// G-Buffer flag constants all come from it, and the light UBO below is declared
// in terms of its LightData struct.
#include "include/PBRCommon.glsl"
// The ambient ladder (issue #1336) — the SAME function the mesh shaders and the
// deferred pass shade with. Forward foliage used to take the irradiance cube or
// the flat fill only, with no probe volume and no reflection probes, while the
// deferred pass lit the same leaves through the whole ladder.
#define OLO_REFLECTION_PROBE_SAMPLERS
#include "include/ReflectionProbes.glsl"
#include "include/LightProbeSampling.glsl"
#define OLO_AMBIENT_LADDER_EXPLICIT_CONTROLS
#include "include/AmbientLadder.glsl"
// Virtual Shadow Maps (issue #702) — self-contained (UBO 79/80, page-table SSBO
// 54, sampler 65), and CommandDispatch::BindShadowTextures publishes a DISABLED
// globals block when VSM is off, so this costs one runtime branch and needs no
// second program. Included because the DEFERRED path takes the VSM branch for
// the directional light; a forward foliage shader that only knew about CSM
// would disagree with it in exactly the frames VSM is on.
#include "include/VirtualShadowSampling.glsl"

// Multi-Light UBO (binding 5) — THE FULL BLOCK, matching PBR_MultiLight.glsl.
// It used to be declared here as a four-int header plus Light[0] only: a
// truncated view of the same buffer, which read the first light correctly and
// made the other 255 unreachable. That truncation IS the "basic forward foliage
// lighting" of the issue title.
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
    vec4 u_ShadowParams;  // x=bias, y=normalBias, z=softness, w=maxShadowDistance
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
#define u_DiffuseTexture OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
// Leaf maps (issue #1234). TEX_METALLIC carries THICKNESS — foliage is never
// metallic, so the slot is definitionally free on this surface, and the engine
// already repurposes a semantic slot per shader this way (PBR_MultiLight's
// u_MetallicRoughnessMap sits on TEX_SPECULAR).
#define u_LeafNormalMap OLO_HEAP_TEX_2D(2)     // TEX_NORMAL
#define u_LeafRoughnessMap OLO_HEAP_TEX_2D(6)  // TEX_ROUGHNESS
#define u_LeafThicknessMap OLO_HEAP_TEX_2D(7)  // TEX_METALLIC (repurposed)
#define u_ShadowMapCSM OLO_HEAP_TEX_2D_ARRAY_SHADOW(8)
#define u_ShadowAtlas OLO_HEAP_TEX_2D_ARRAY_SHADOW(13)
#define u_ShadowMapCSMRaw OLO_HEAP_TEX_2D_ARRAY(33)
#define u_ShadowAtlasRaw OLO_HEAP_TEX_2D_ARRAY(34)
#define u_IrradianceMap OLO_HEAP_TEX_CUBE(10)  // TEX_USER_0
#define u_PrefilterMap OLO_HEAP_TEX_CUBE(11)   // TEX_USER_1
#define u_BRDFLutMap OLO_HEAP_TEX_2D(12)       // TEX_USER_2
#else
layout(binding = 0) uniform sampler2D u_DiffuseTexture;
layout(binding = 2) uniform sampler2D u_LeafNormalMap;     // TEX_NORMAL
layout(binding = 6) uniform sampler2D u_LeafRoughnessMap;  // TEX_ROUGHNESS
layout(binding = 7) uniform sampler2D u_LeafThicknessMap;  // TEX_METALLIC (repurposed: thickness)
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM;  // TEX_SHADOW
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowAtlas;  // TEX_SHADOW_ATLAS
layout(binding = 33) uniform sampler2DArray u_ShadowMapCSMRaw;    // TEX_SHADOW_CSM_RAW
layout(binding = 34) uniform sampler2DArray u_ShadowAtlasRaw;     // TEX_SHADOW_ATLAS_RAW
layout(binding = 10) uniform samplerCube u_IrradianceMap;  // TEX_USER_0
layout(binding = 11) uniform samplerCube u_PrefilterMap;   // TEX_USER_1
layout(binding = 12) uniform sampler2D u_BRDFLutMap;       // TEX_USER_2
#endif

// Foliage UBO (binding 12) — shared with vertex stage
#include "include/FoliageParams.glsl"

#include "include/FoliageInstanceGeometry.glsl"

// The shared vegetation material, sampling half included: this program owns the
// foliage UBO and the leaf samplers it needs.
#define OLO_FOLIAGE_SURFACE_SAMPLING 1
#include "include/FoliageSurface.glsl"

void main()
{
    // Mesh-to-card hand-over (issue #1233). The layer's authored-mesh draw and
    // its card draw run this with the same coverage and the same dither, so
    // between them they cover each pixel exactly once — no stretch where a pine
    // and a billboard of that pine are both on screen.
    if (!foliageLodKeep(u_MeshParams.x > 0.5, v_MeshCoverage, gl_FragCoord.xy, v_InstanceSeed,
                        foliageStochasticCoverage(u_LodTransition0)))
        discard;

    // Sample albedo
    vec4 texColor = texture(u_DiffuseTexture, v_TexCoord);
    vec4 color = vec4(texColor.rgb * v_Color, texColor.a);

    // Alpha test
    if (color.a < v_AlphaCutoff)
        discard;

    // Distance fade
    float dist = distance(v_WorldPos, u_CameraPosition);
    float fadeFactor = 1.0 - smoothstep(u_FadeStart, u_ViewDistance, dist);
    if (fadeFactor <= 0.0)
        discard;

    color.a *= fadeFactor * v_Fade;

    // ── THE SURFACE (issue #1234) ────────────────────────────────────────
    // From the SAME function Foliage_Instance_GBuffer.glsl calls, with the same
    // arguments: albedo, the viewer-facing normal, the mapped roughness and the
    // per-pixel thickness. `leaf.Normal` is already flipped to face the viewer
    // by oloFoliageFaceNormal — that shared call is the whole of the "same
    // normal orientation" criterion, and it is a direction test rather than
    // gl_FrontFacing so the culled card, the two-sided mesh and the impostor
    // all answer alike.
    vec3 V = normalize(u_CameraPosition - v_WorldPos);
    OloFoliageSurface leaf = oloFoliageSampleSurface(v_WorldPos, v_Normal, v_TexCoord, V, v_Color, texColor);

    // Foliage is never metallic and carries no baked AO map. Matching the
    // G-Buffer writer, which writes exactly these two constants.
    const float metallic = 0.0;
    const float ao = 1.0;

    bool isLeaf = oloLeafEnabled();
    vec3 leafTint = u_LeafTransmit.rgb; // ALREADY tint * strength; see the UBO
    // The irradiance arriving on the FAR face, for the transmission's indirect
    // half. Sampled along -N so it is the environment BEHIND the leaf, which is
    // what actually shines through it.
    // Zero when no environment is bound, and that is the honest answer rather
    // than a gap: with no environment there IS nothing behind the leaf to shine
    // through it. The DIRECT half of the transmission is unaffected, so a
    // backlit canopy still glows in an IBL-less scene.
    vec3 backEnvIrradiance = (isLeaf && u_LeafIds.y > 0.5)
                                 ? texture(u_IrradianceMap, -leaf.Normal).rgb * u_LeafIds.z
                                 : vec3(0.0);

    float viewDepth = (u_View * vec4(v_WorldPos, 1.0)).z;

    OloSurfaceLighting Lo = oloSurfaceLightingZero();
    vec3 transmitted = vec3(0.0);

    int lightCount = min(u_LightCount, MAX_LIGHTS);
    for (int i = 0; i < lightCount; ++i)
    {
        int lightType = int(u_Lights[i].position.w);

        // The light's GEOMETRY, from the shared helper the reflected lobe also
        // uses (oloLightSample, PBRCommon.glsl). It returns false for a sphere
        // area light — that type has no single L — so those contribute to the
        // reflected lobe below and to no transmission, which is stated rather
        // than approximated with a direction to the centre.
        vec3 L;
        vec3 radiance;
        bool hasDirection = oloLightSample(u_Lights[i], v_WorldPos, L, radiance);

        // ONE shadow factor, biased along the LIT-SIDE normal, serving BOTH
        // lobes. See oloFoliageShadowNormal for why the shading normal is the
        // wrong normal to offset a backlit leaf's shadow lookup along, and why
        // this is a no-op wherever the reflected lobe is non-zero.
        vec3 Ns = hasDirection ? oloFoliageShadowNormal(leaf.Normal, L) : leaf.Normal;
        float shadow = 1.0;

        if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
            if (VSM_ENABLED != 0)
            {
                shadow = vsmShadowFactor(v_WorldPos, Ns);
            }
            else
            {
                shadow = calculateCascadedShadowFactorCSM(
                    u_ShadowMapCSM, u_ShadowMapCSMRaw, v_WorldPos, Ns, viewDepth,
                    u_DirectionalLightSpaceMatrices, u_CascadePlaneDistances,
                    u_ShadowParams, u_ShadowMapResolution, u_SoftShadowMode);
            }
        }
        else if (lightType == SPOT_LIGHT)
        {
            int atlasEntry = int(u_Lights[i].direction.w);
            float localShadow;
            if (vsmLocalShadow(v_WorldPos, Ns, atlasEntry, false, localShadow))
            {
                shadow = localShadow;
            }
            else if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
            {
                shadow = calculateAtlasEntryShadow(
                    v_WorldPos, u_AtlasEntryMatrices[atlasEntry], u_AtlasEntryScaleOffset[atlasEntry],
                    u_ShadowAtlas, u_ShadowAtlasRaw, u_AtlasDepthBias, u_AtlasResolution,
                    u_SoftShadowMode, u_ShadowParams.z);
            }
        }
        else if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
        {
            int baseEntry = int(u_Lights[i].direction.w);
            float localShadow;
            if (vsmLocalShadow(v_WorldPos, Ns, baseEntry, true, localShadow))
            {
                shadow = localShadow;
            }
            else if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
            {
                int entry = baseEntry + atlasCubeFace(v_WorldPos - u_Lights[i].position.xyz);
                shadow = calculateAtlasEntryShadow(
                    v_WorldPos, u_AtlasEntryMatrices[entry], u_AtlasEntryScaleOffset[entry],
                    u_ShadowAtlas, u_ShadowAtlasRaw, u_AtlasDepthBias, u_AtlasResolution,
                    0, u_ShadowParams.z);
            }
        }

        // The REFLECTED lobe — the front face's ordinary PBR response. Zero
        // where dot(N, L) <= 0, which is exactly the backlit case the
        // transmission lobe below exists to fill.
        OloSurfaceLighting contrib = calculateLightContributionSplit(
            u_Lights[i], leaf.Normal, V, leaf.Albedo, metallic, leaf.Roughness, v_WorldPos,
            OLO_PBR_MODEL_LEGACY);
        Lo = oloSurfaceLightingAdd(Lo, oloSurfaceLightingScale(contrib, vec3(shadow)));

        // The TRANSMITTED lobe, gated by the SAME shadow factor — which is what
        // makes it not an unshadowed constant (#1234's second criterion).
        if (isLeaf && hasDirection)
        {
            transmitted += oloFoliageTransmissionDirect(leaf.Normal, V, L, radiance, shadow,
                                                        leaf.Thickness, leafTint, u_LeafLobe);
        }
    }

    // Image-based ambient, replacing the flat `albedo * 0.3` this shader used to
    // apply. The prefiltered radiance at the mirror direction and the
    // irradiance cubemap are both bound per foliage draw from Renderer3D's
    // GLOBAL IBL (CommandDispatch::DrawFoliageLayer), so a canopy's ambient does
    // not depend on whether a lit mesh happened to draw first.
    //
    // WITH NO ENVIRONMENT BOUND it falls back to calculateSimpleAmbient — the
    // same fallback Terrain_PBR and the deferred ambient ladder's last rung
    // use. That is not a nicety: a scene with no EnvironmentMap binds no IBL
    // trio at all, the samplers would read the engine's typed-null cubemap as
    // black, and replacing the old flat ambient with that would have turned
    // every such canopy's unlit side black. u_LeafIds.y is what tells the two
    // cases apart, because a bound-and-black cubemap is a legitimate frame.
    // THE LADDER (issue #1336): u_LeafIds.y says whether the global IBL trio is
    // bound, .z is its intensity and .w is the probe switch — the same three
    // controls DeferredLightingPass shades foliage with. Unbound IBL falls
    // through to the flat fill inside the ladder, as it always did here.
    bool foliageEnableIBL = u_LeafIds.y > 0.5;
    vec3 foliageR = reflect(-V, leaf.Normal);
    vec3 prefilteredColor = vec3(0.0);
    if (foliageEnableIBL)
    {
        prefilteredColor = textureLod(u_PrefilterMap, foliageR, leaf.Roughness * MAX_REFLECTION_LOD).rgb;
        float probeViewDepth = -(u_View * vec4(v_WorldPos, 1.0)).z;
        vec4 probeSpecular =
            oloSampleReflectionProbes(v_WorldPos, leaf.Normal, foliageR, leaf.Roughness * MAX_REFLECTION_LOD, probeViewDepth);
        prefilteredColor = mix(prefilteredColor, probeSpecular.rgb, probeSpecular.a);
    }
    vec3 ambient = oloSurfaceLightingSum(evaluateAmbientLadderSplitEx(
        vec4(0.0), v_WorldPos, leaf.Normal, V, leaf.Albedo, metallic, leaf.Roughness, u_IrradianceMap, u_BRDFLutMap, prefilteredColor,
        foliageEnableIBL, u_LeafIds.w > 0.5, u_LeafIds.z));

    // The environment half of the transmission, added ONCE rather than per
    // light — see oloFoliageTransmissionAmbient.
    if (isLeaf)
    {
        transmitted += oloFoliageTransmissionAmbient(leaf.Thickness, leafTint, backEnvIrradiance,
                                                     u_LeafLobe);
    }

    // No screen-space AO (issue #1452): foliage is not in the forward
    // depth-normal prepass, so the AO buffer holds the occlusion of the ground
    // or wall BEHIND each leaf. The G-Buffer twin gets its own AO on Deferred.
    vec3 litColor = ambient * ao + oloSurfaceLightingSum(Lo) + transmitted;

    FragColor = vec4(u_WindWeights.w > 0.5 ? v_Color : litColor, color.a);

    // Camera-motion + wind-reprojection velocity. v_PrevWorldPos already
    // includes the prev-frame wind displacement (re-evaluated at u_PrevTime).
    vec4 clipCurr = u_ViewProjection     * vec4(v_WorldPos,     1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevWorldPos, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    // .b is this leaf's COVERAGE (#1256) — cutout alpha times the LOD fade,
    // the same quantity Foliage_Instance_GBuffer.glsl writes. The FORWARD
    // path needs it too: the editor runs forward by default, so wiring only
    // the deferred variant left the channel reading a flat 1.0 on every
    // foliage pixel a user actually sees.
    o_Velocity = vec4((ndcCurr - ndcPrev) * 0.5, clamp(color.a, 0.0, 1.0), 0.0);
    o_SkinDiffuse = vec4(0.0); // not skin -- see the declaration above (#1241)
}
