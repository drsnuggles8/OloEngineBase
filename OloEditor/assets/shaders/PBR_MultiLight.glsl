// =============================================================================
// PBR_MultiLight.glsl - Physically Based Rendering Shader with Multi-Light Support
// Part of OloEngine Enhanced PBR System
// Supports metallic-roughness workflow (glTF 2.0 standard) with multiple lights
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5): V1 engine-vertex pull. On the Vulkan route the
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
    // The lightmap UV stream is not pulled on the Vulkan route yet (issue #439
    // follow-up — it is a second stream, not part of the 8-float engine Vertex).
    vec2 a_TexCoord2 = vec2(0.0);
#endif
    OLO_INSTANCE_FORWARD();
    v_WorldPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_Normal = mat3(u_Normal) * a_Normal;
    v_TexCoord = a_TexCoord;

    vec4 clipCurr = u_ViewProjection * vec4(v_WorldPos, 1.0);
    vec4 prevWorldPos = u_PrevModel * vec4(a_Position, 1.0);
    vec4 clipPrev = u_PrevViewProjection * prevWorldPos;

    v_ClipPosCurr = clipCurr;
    v_ClipPosPrev = clipPrev;

    gl_Position = clipCurr;
}

#type fragment
#version 460 core

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
    int _pbrPad2;
#ifdef OLO_BINDLESS
    // The per-material offset lanes, declared ONLY on the bindless build. The
    // C++ PBRMaterialUBO always uploads them (sizeof == 144); a std140 block may
    // declare a PREFIX of what the CPU writes, which is why the slot-based build
    // can stop at _pbrPad2 and stay correct. Must be LAST — the lane layout in
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
#ifdef OLO_BINDLESS
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

#define u_EnvironmentMap OLO_HEAP_TEX_CUBE(9)
#define u_IrradianceMap OLO_HEAP_TEX_CUBE(10)
#define u_PrefilterMap OLO_HEAP_TEX_CUBE(11)
#define u_BRDFLutMap OLO_HEAP_TEX_2D(12)
#else
// Texture bindings following ShaderBindingLayout
layout(binding = 0) uniform sampler2D u_AlbedoMap;          // TEX_DIFFUSE
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap; // TEX_SPECULAR (repurposed)
layout(binding = 2) uniform sampler2D u_NormalMap;          // TEX_NORMAL
layout(binding = 4) uniform sampler2D u_AOMap;              // TEX_AMBIENT
layout(binding = 5) uniform sampler2D u_EmissiveMap;        // TEX_EMISSIVE
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
    int _shadowPad1;
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

    vec3 albedo = sampleAlbedo(u_AlbedoMap, v_TexCoord, u_BaseColorFactor.rgb, bool(u_UseAlbedoMap));
    vec2 metallicRoughness = sampleMetallicRoughness(u_MetallicRoughnessMap, v_TexCoord,
                                                     u_MetallicFactor, u_RoughnessFactor,
                                                     bool(u_UseMetallicRoughnessMap));
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;

    float ao = sampleAO(u_AOMap, v_TexCoord, u_OcclusionStrength, bool(u_UseAOMap));
    vec3 emissive = sampleEmissive(u_EmissiveMap, v_TexCoord, u_EmissiveFactor.rgb, bool(u_UseEmissiveMap));

    // Calculate normal
    vec3 N = normalize(v_Normal);
    if (u_UseNormalMap == 1)
    {
        N = getNormalFromMap(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale);
    }
    vec3 V = normalize(u_CameraPosition - v_WorldPos);

    // Weather response (issue #633): rain-wet surfaces darken and gloss up
    // before any lighting reads albedo/roughness.
    atmosphereApplyWetness(albedo, roughness, N);

    // Cloud shadow (issue #633): the cloudscape occludes the directional body
    // regardless of the CSM gate below — evaluated once, applied per
    // directional light inside the loop.
    float cloudShadow = atmosphereCloudShadow(v_WorldPos);

    // Calculate direct lighting from all lights
    vec3 Lo = vec3(0.0);

    // Forward+ path: use per-cluster culled light lists for point/spot lights
    bool fplusActive = (fplus_Params.z != 0u);
    if (fplusActive)
    {
        float fplusViewDepth = -(u_View * vec4(v_WorldPos, 1.0)).z;
        Lo += fplusEvaluateTileLights(N, V, v_WorldPos, albedo, metallic, roughness, fplusViewDepth);
    }

    // UBO light loop: when Forward+ is active, only evaluate directional lights
    // (stored at the start of the array). When Forward+ is off, evaluate all lights.
    int loopCount = fplusActive ? min(u_DirectionalLightCount, MAX_LIGHTS)
                                : min(u_LightCount, MAX_LIGHTS);
    for (int i = 0; i < loopCount; ++i)
    {
        int lightType = int(u_Lights[i].position.w);

        vec3 lightContrib = calculateLightContribution(u_Lights[i], N, V, albedo, metallic, roughness, v_WorldPos);
        if (lightType == DIRECTIONAL_LIGHT)
        {
            lightContrib *= cloudShadow;
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
                shadow = vsmShadowFactor(v_WorldPos, N);
            }
            else
            {
                shadow = calculateCascadedShadowFactorCSM(
                    u_ShadowMapCSM,
                    u_ShadowMapCSMRaw,
                    v_WorldPos,
                    viewDepth,
                    u_DirectionalLightSpaceMatrices,
                    u_CascadePlaneDistances,
                    u_ShadowParams,
                    u_ShadowMapResolution,
                    u_SoftShadowMode
                );
            }
            lightContrib *= shadow;
        }
        // Apply spot light shadows (atlas entry, issue #435)
        else if (lightType == SPOT_LIGHT)
        {
            // Atlas entry or VSM layer base, decided by vsmLocalShadow (#703).
            int atlasEntry = int(u_Lights[i].direction.w);
            float localShadow;
            if (vsmLocalShadow(v_WorldPos, N, atlasEntry, false, localShadow))
            {
                lightContrib *= localShadow;
            }
            else if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
            {
                float shadow = calculateAtlasEntryShadow(
                    v_WorldPos,
                    u_AtlasEntryMatrices[atlasEntry],
                    u_AtlasEntryScaleOffset[atlasEntry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_ShadowParams.x,
                    u_AtlasResolution,
                    u_SoftShadowMode,
                    u_ShadowParams.z
                );
                lightContrib *= shadow;
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
            if (vsmLocalShadow(v_WorldPos, N, baseEntry, true, localShadow))
            {
                lightContrib *= localShadow;
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
                    u_ShadowParams.x,
                    u_AtlasResolution,
                    0, // PCF only on cube faces (matches the old cubemap path)
                    u_ShadowParams.z
                );
                lightContrib *= shadow;
            }
        }

        Lo += lightContrib;
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

    // Calculate ambient lighting
    vec3 ambient = vec3(0.0);

    // Baked lightmap first (issue #439): a lightmapped static surface REPLACES
    // its probe/IBL indirect diffuse with the baked irradiance — the same
    // replacement semantics the probe ladder already uses, through the same
    // helpers, so the two sources stay photometrically interchangeable. IBL
    // specular is kept (the bake is diffuse irradiance only). Deliberately not
    // gated on u_EnableLightProbes: baked GI is its own source, and the scene
    // kill switch lives in u_LightmapEnabled (stale bakes upload 0).
#ifdef OLO_VULKAN
    // Not wired on the Vulkan route yet: nothing uploads UBO 1 / TEX 16 there,
    // so the flag would read garbage (issue #439 follow-up).
    vec3 lightmapIrradiance = vec3(0.0);
#else
    vec3 lightmapIrradiance = sampleLightmapIrradiance(v_TexCoord2, instances[v_InstanceIndex].LightmapScaleOffset);
#endif
    if (dot(lightmapIrradiance, lightmapIrradiance) > 0.0)
    {
        if (u_EnableIBL == 1)
        {
            ambient = calculateCombinedAmbientPrefiltered(lightmapIrradiance, N, V, albedo,
                                                          metallic, roughness,
                                                          u_BRDFLutMap, prefilteredColor);
            ambient *= u_IBLIntensity;
        }
        else
        {
            ambient = calculateLightProbeAmbient(lightmapIrradiance, albedo, metallic, roughness, N, V);
        }
    }
    else if (u_EnableLightProbes == 1 && u_EnableIBL == 1)
    {
        // Combined: probe diffuse + IBL specular. Issue #632: unified probe
        // sampling — realtime DDGI atlases when a Realtime/Hybrid volume is
        // bound, baked SH otherwise.
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(v_WorldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
        {
            ambient = calculateCombinedAmbientPrefiltered(probeIrradiance, N, V, albedo,
                                                          metallic, roughness,
                                                          u_BRDFLutMap, prefilteredColor);
            ambient *= u_IBLIntensity;
        }
        else
        {
            // Outside probe volume — fall back to IBL
            ambient = calculateIBLPrefiltered(N, V, albedo, metallic, roughness,
                                              u_IrradianceMap, u_BRDFLutMap, prefilteredColor);
            ambient *= u_IBLIntensity;
        }
    }
    else if (u_EnableLightProbes == 1)
    {
        // Probes only, no IBL specular
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(v_WorldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
        {
            ambient = calculateLightProbeAmbient(probeIrradiance, albedo, metallic, roughness, N, V);
        }
        else
        {
            ambient = calculateSimpleAmbient(albedo, metallic, ao);
        }
    }
    else if (u_EnableIBL == 1)
    {
        ambient = calculateIBLPrefiltered(N, V, albedo, metallic, roughness,
                                          u_IrradianceMap, u_BRDFLutMap, prefilteredColor);
        ambient *= u_IBLIntensity;
    }
    else
    {
        ambient = calculateSimpleAmbient(albedo, metallic, ao);
    }

    // Combine lighting — AO attenuates ambient only
    vec3 color = ambient * ao + Lo + emissive;

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
