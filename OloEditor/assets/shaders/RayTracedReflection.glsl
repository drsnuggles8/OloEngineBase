#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED — binding 57
// is the engine-wide vertex-pull binding; the root struct carries this buffer's
// device address, so the SAME 20-byte {vec3 position, vec2 uv} stream the
// attribute path consumes is read by index instead.
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

// =============================================================================
// RayTracedReflection.glsl — the RAY-QUERY tier of the reflection hierarchy.
// Issue #1057 (#979 Phase 2). The contract this implements is ADR 0020.
//
// WHERE THIS SITS, AND WHY THAT IS THE WHOLE DESIGN. ADR 0020 evaluates the
// hierarchy BOTTOM-UP: every tier replaces the indirect specular term already in
// the colour it was handed, so no tier ever has to know the confidence of a tier
// ABOVE it. This pass therefore runs AFTER DeferredLighting (whose output
// already carries the probe-over-IBL term, the two tiers below this one) and
// BEFORE SSRRenderPass (the tier above), which replaces the term THIS pass hands
// on in its second output.
//
// THE TERM, NOT THE COLOUR (issue #1325). The colour is diffuse + direct +
// emission + the indirect specular term S = W * R. This pass reads S and W from
// IndirectSpecular (DeferredLighting writes both), and writes
// C + c * (W * L - S): a black hit removes c * S and nothing else. It used to
// write mix(C, L * tint, c * Fresnel), which dimmed the diffuse, the emission and
// the direct light by the same factor. Fresnel is now inside W, where the
// lighting put it, and the confidence is the roughness gate alone.
//
// That ordering is what makes the tier cheap to add. The alternative reading —
// let the better tier claim its share and hand the residual down — is the same
// algebra and needs SSR's per-pixel confidence transported through a denoiser
// chain whose only spare lane already carries view depth, deliberately, on
// every path including the early-outs.
//
// WHAT IT FILLS: exactly the gap SSR cannot cover — OFF-SCREEN and OCCLUDED
// hits. A miss contributes NOTHING (confidence 0), so the tier below answers;
// the sky is the probe/IBL tier's job and reflecting it here would double-count
// it.
//
// ZERO NEW BINDINGS. The TLAS arrives as a DEVICE ADDRESS inside the one UBO at
// UBO_RAY_TRACING (65) — accelerationStructureEXT(uvec2) needs no descriptor at
// all. Binding 65 is shared with RayTracedShadow.glsl's and
// RayTracingProbe.comp's own blocks, which is the established idiom here: each
// pass rebinds its own buffer before its own draws, and the buffer-binding
// namespace has been full since #978. THE WITHIN-SHADER RULE HOLDS: 65 is also
// TEX_VSM_PHYSICAL, and this shader declares no VSM sampler and must not start
// to.
//
// The three GPU Scene tables come from their CANONICAL SSBO bindings (15, 16,
// 17) rather than as device addresses, which is reuse rather than a fourth
// mechanism. It is also the SAFER of the two: a table is filled by a CPU
// SetData every frame, and a mid-frame SetData snapshots into the frame arena,
// so a draw that resolved the PERSISTENT device address would read last
// frame's records (VulkanStorageBuffer::GetRootDataAddress's comment is the
// long version). The ordinary bind path already gets that right.
//
// THE WITHIN-SHADER ALIAS RULE HOLDS for all three: 15 aliases the per-draw
// InstanceData stream, 16 SSBO_INSTANCE_CULL_INPUT and 17
// SSBO_INSTANCE_DRAW_INDIRECT, and this shader declares none of them — it is a
// fullscreen post-process draw with no instance stream at all.
//
// ZERO NEW ATTACHMENTS. The tier debug view needs this pass's per-pixel
// confidence to reach PostProcess_SSRComposite.glsl, the only point in the
// frame where the whole hierarchy is known. It travels in the ALPHA of the
// colour output, which every consumer in the chain ignores (they all read
// .rgb) — and it is written ONLY when the debug view is on, so no production
// frame ever carries a non-1.0 alpha down the chain.
//
// Hit lighting uses the canonical material maps and a bounded local closure.
// Colour and data maps use a projected ray footprint. Masked traversal uses
// the existing albedo-alpha cutoff and explicit level-zero policy.
//
// THE RAY IS THE MIRROR DIRECTION, not a VNDF sample. That is defensible only
// because the roughness gate below is narrow: the tier is faded out well before
// the specular lobe is wide enough for one deterministic ray to misrepresent
// it, and rough surfaces stay on the probes, where a ray budget buys nothing.
// The consequence is that the top of the transition band is slightly over-sharp.
// A VNDF sample plus a denoiser is the follow-up, and ADR 0020's algebra means
// it lands by raising this tier's L, with no weight anywhere else changing.
// =============================================================================

#extension GL_EXT_ray_query : require
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(location = 0) out vec4 o_Color;
// The indirect specular term this tier hands on, for SSR to replace (#1325).
layout(location = 1) out vec4 o_Specular;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_IndirectSpecular OLO_HEAP_TEX_2D(1)       // TEX_SPECULAR
#define u_IndirectSpecularWeight OLO_HEAP_TEX_2D(2) // TEX_NORMAL
#define u_PrefilterMap OLO_HEAP_TEX_CUBE(11)
#define u_DepthTexture OLO_HEAP_TEX_2D(19)
#define u_GBufferNormal OLO_HEAP_TEX_2D(44)
#else
layout(binding = 0) uniform sampler2D u_SceneColor;      // upstream lit HDR colour (probe/IBL already in it)
layout(binding = 1) uniform sampler2D u_IndirectSpecular;       // the indirect specular term in it (#1325)
layout(binding = 2) uniform sampler2D u_IndirectSpecularWeight; // that term per unit of incident radiance
layout(binding = 11) uniform samplerCube u_PrefilterMap; // TEX_USER_1: specular pre-filter mip chain
layout(binding = 19) uniform sampler2D u_DepthTexture;   // scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal;  // RT1: rg = oct world normal, z = roughness, w = ao
#endif

#include "include/SkyDepth.glsl"
#include "include/GPUScene.glsl"

// The vertex/index stream references and the 32-byte Vertex layout live in the
// #978 helper; including it here reuses those types rather than declaring a
// second, drift-prone copy. Its include guard makes that safe.

#include "include/GPUSceneInstances.glsl"
#include "include/GPUSceneGeometries.glsl"
#include "include/GPUSceneMaterials.glsl"

// UBO_RAY_TRACING (65). Mirrored on the CPU by
// UBOStructures::RayTracingReflectionUBO.
layout(std140, binding = 65) uniform RayTracingReflectionParams
{
    mat4 u_InvProjection;
    mat4 u_InvView;
    mat4 u_View;
    uvec4 u_TlasAddress;         // xy = TLAS device address, z = instance mask, w = frame index
    uvec4 u_SlotCounts;          // x = instance slots, y = geometry slots, z = material slots, w = pad
    vec4 u_SunDirection;         // xyz = world direction TOWARD the sun, w = 1 when a sun exists
    vec4 u_SunColor;             // rgb = radiance, a = unused
    vec4 u_RayParams;            // x = maxRayDistance, y = normalBias, z = intensity, w = traceShadowRay (0/1)
    vec4 u_RoughnessGate;        // x = gateStart, y = gateEnd, z = skyAmbientLod, w = hasEnvironment (0/1)
    vec4 u_ScreenParams;         // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;                // x = tierDebugView (0/1), yzw = pad
    uvec4 u_MaterialHeapAddressAndSampler;
};

#define OLO_HYBRID_RT_SLOT_COUNTS u_SlotCounts
#define OLO_HYBRID_RT_HEAP_ADDRESS_AND_COUNT u_MaterialHeapAddressAndSampler.xyz
#define OLO_HYBRID_RT_SAMPLER u_MaterialHeapAddressAndSampler.w
#include "include/HybridRayTracingAlpha.glsl"
#include "include/PBRCommon.glsl"
#include "include/RayHitTriangle.glsl"
#include "include/HybridReflectionHit.glsl"
#include "include/ReflectionTierComposite.glsl"

// Every ray starts this far along its own direction, on top of the normal
// offset. The normal offset alone cannot fix a ray leaving a surface at a
// grazing angle — the offset is perpendicular to the error, not along it.
// Metres. Matches RT_SHADOW_RAY_TMIN.
const float RT_REFLECTION_RAY_TMIN = 0.005;

// Octahedral decode — matches octEncodeGB() in PBR_GBuffer.glsl.
vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

vec3 ViewPosFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = u_InvProjection * ndc;
    return view.xyz / view.w;
}

vec4 oloReflectSampleMap(uint offset, vec2 uv, mat2 footprint)
{
#ifdef OLO_VULKAN
    return textureGrad(OLO_HEAP_MATERIAL_TEX_2D(offset, OLO_HYBRID_RT_SAMPLER), uv,
                       footprint[0], footprint[1]);
#else
    return vec4(1.0);
#endif
}

// True when nothing blocks the sun at this world position. A second, cheap
// visibility ray: without it every reflected surface is lit as if unshadowed,
// which reads as reflected objects glowing in a shadowed courtyard.
bool SunVisible(vec3 worldPos, vec3 normal)
{
    if (u_RayParams.w < 0.5 || u_SunDirection.w < 0.5)
        return true;

    rayQueryEXT shadowQuery;
    rayQueryInitializeEXT(shadowQuery, accelerationStructureEXT(u_TlasAddress.xy),
                          gl_RayFlagsTerminateOnFirstHitEXT,
                          u_TlasAddress.z & 0xFFu,
                          worldPos + normal * u_RayParams.y, RT_REFLECTION_RAY_TMIN,
                          normalize(u_SunDirection.xyz), max(u_RayParams.x, 1.0));
    oloHybridRayTracingProceed(shadowQuery);
    return rayQueryGetIntersectionTypeEXT(shadowQuery, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

void main()
{
    const vec3 baseColor = texture(u_SceneColor, v_TexCoord).rgb;
    const vec3 baseSpecular = texture(u_IndirectSpecular, v_TexCoord).rgb;

    // Everything below can early-out, and every early-out must leave the colour
    // EXACTLY as it found it. That is not politeness: ADR 0020 §5 says the
    // raster-only output is byte-identical when this tier is off, and the
    // reason it is byte-identical is that a zero-confidence pixel is a copy,
    // not a blend by a small number.
    //
    // Alpha is 0 under the tier debug view and 1 otherwise, and the ZERO is the
    // load-bearing half: every early-out below leaves this value in place, and
    // each of them means "this tier answered none of this pixel". Writing 1.0
    // here would make the debug view paint every missed ray as though the ray
    // tier owned it — the exact opposite of the truth, and invisible in any
    // production frame because alpha is unused there.
    o_Color = vec4(baseColor, (u_Flags.x > 0.5) ? 0.0 : 1.0);
    // And the specular term handed on unchanged, on every early-out alike.
    o_Specular = vec4(baseSpecular, 1.0);

    // The TLAS address is zero when no acceleration structure has been built.
    // Tracing against it is undefined behaviour at rayQueryInitializeEXT, not a
    // miss, so this is a guard rather than an optimisation.
    if ((u_TlasAddress.x | u_TlasAddress.y) == 0u)
        return;

    const float depth = texture(u_DepthTexture, v_TexCoord).r;
    // oloDepthIsSky, not a literal threshold: an AMD cleared depth buffer reads
    // one ulp under 1.0, so a >= 1.0 test misses the sky on that vendor.
    if (oloDepthIsSky(depth)) // sky / background — nothing to reflect FROM
        return;

    const vec4 gN = texture(u_GBufferNormal, v_TexCoord);
    const float roughness = gN.z;

    // The roughness gate, ADR 0020 §4: rays are spent only where the lobe is
    // narrow enough for one deterministic sample to mean something. Above the
    // gate the confidence is exactly 0 and the probes answer.
    const float gateStart = u_RoughnessGate.x;
    const float gateEnd = max(u_RoughnessGate.y, gateStart + 1e-4);
    const float roughnessGate = 1.0 - smoothstep(gateStart, gateEnd, roughness);
    if (roughnessGate <= 0.0)
        return;

    const vec3 N = OctDecode(gN.xy);
    const vec3 viewPos = ViewPosFromDepth(v_TexCoord, depth);
    const vec3 worldPos = (u_InvView * vec4(viewPos, 1.0)).xyz;
    const vec3 cameraPos = (u_InvView * vec4(0.0, 0.0, 0.0, 1.0)).xyz;

    const vec3 V = normalize(worldPos - cameraPos); // toward the surface
    const vec3 R = reflect(V, N);

    // No Fresnel here (issue #1325): the reflectance is in the weight W the
    // lighting pass exported, the same (F * A + B) its own probe/IBL term used,
    // so this tier's estimate lands in the units of the term it replaces.

    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(rayQuery, accelerationStructureEXT(u_TlasAddress.xy),
                          gl_RayFlagsNoneEXT, u_TlasAddress.z & 0xFFu,
                          worldPos + N * u_RayParams.y, RT_REFLECTION_RAY_TMIN,
                          R, max(u_RayParams.x, 1.0));
    oloHybridRayTracingProceed(rayQuery);

    if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return; // a MISS contributes nothing — the tier below owns the sky

    const uint instanceSlot = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true));
    if (instanceSlot >= u_SlotCounts.x)
        return;

    const GPUSceneInstance instance = g_GPUSceneInstances[instanceSlot];
    // A tombstoned slot keeps its generation when the counter saturates, so the
    // Active bit is the only safe test — GPUSceneTypes.h's rule.
    if ((instance.Flags & OLO_GPU_SCENE_INSTANCE_ACTIVE) == 0u)
        return;
    if (instance.MaterialIndex >= u_SlotCounts.z || instance.GeometryIndex >= u_SlotCounts.y)
        return;

    const GPUSceneGeometry geometry = g_GPUSceneGeometries[instance.GeometryIndex];
    const GPUSceneMaterial material = g_GPUSceneMaterials[instance.MaterialIndex];
    const uint primitiveIndex = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true));
    if ((geometry.Flags & OLO_GPU_SCENE_GEOMETRY_ACTIVE) == 0u ||
        (material.Flags & OLO_GPU_SCENE_MATERIAL_ACTIVE) == 0u ||
        geometry.Generation != instance.GeometryGeneration || material.Generation != instance.MaterialGeneration ||
        primitiveIndex >= geometry.IndexCount / 3u ||
        (material.Flags & OLO_GPU_SCENE_MATERIAL_TRANSMISSION) != 0u)
        return;

    OloMaterialShaderHeapRecord maps;
    if (!oloMaterialShaderHeapRecord(uvec4(0u, 0u, instance.MaterialIndex, instance.MaterialGeneration),
                                     OLO_HYBRID_RT_HEAP_ADDRESS_AND_COUNT, maps))
        return;

    const float hitT = rayQueryGetIntersectionTEXT(rayQuery, true);
    const vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
    const float b0 = 1.0 - bary.x - bary.y;
    const mat4x3 objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rayQuery, true);
    const mat4x3 worldToObject = rayQueryGetIntersectionWorldToObjectEXT(rayQuery, true);
    const vec3 hitPos = worldPos + N * u_RayParams.y + R * hitT;
    vec3 p0, p1, p2, n0, n1, n2;
    vec2 uv0, uv1, uv2;
    OloRtFetchTriangle(geometry, primitiveIndex, objectToWorld, worldToObject,
                       p0, p1, p2, n0, n1, n2, uv0, uv1, uv2);

    const vec3 e1 = p1 - p0;
    const vec3 e2 = p2 - p0;
    vec3 geometricNormal;
    if (!oloRtNormalizeHitNormal(cross(e1, e2) * oloRtInstanceWindingSign(objectToWorld), geometricNormal))
        return;
    const bool frontFace = dot(geometricNormal, R) <= 0.0;
    if (!frontFace && (material.Flags & OLO_GPU_SCENE_MATERIAL_TWO_SIDED) == 0u)
        return;

    vec3 hitNormal;
    if (!oloRtNormalizeHitNormal(n0 * b0 + n1 * bary.x + n2 * bary.y, hitNormal) ||
        dot(hitNormal, geometricNormal) <= 0.0)
        hitNormal = geometricNormal;

    const vec2 uv = uv0 * b0 + uv1 * bary.x + uv2 * bary.y;
    const vec2 pixelAngle = 2.0 * abs(vec2(u_InvProjection[0][0], u_InvProjection[1][1])) * u_ScreenParams.zw;
    const float diameter = (length(viewPos) + hitT) * max(pixelAngle.x, pixelAngle.y);
    const mat2 footprint = oloReflectHitFootprint(e1, e2, uv1 - uv0, uv2 - uv0, R, diameter);

    vec3 albedo = material.BaseColorFactor.rgb * instance.Tint.rgb;
    vec3 emissive = material.EmissiveFactor.rgb;
    float metallic = material.MetallicFactor;
    float hitRoughness = material.RoughnessFactor;
    float occlusion = 1.0;
    if ((maps.Flags & OLO_GPU_SCENE_MATERIAL_ALBEDO_MAP) != 0u)
        albedo *= oloReflectSampleMap(maps.Textures.x, uv, footprint).rgb;
    if ((maps.Flags & OLO_GPU_SCENE_MATERIAL_METALLIC_ROUGHNESS_MAP) != 0u)
    {
        const vec2 metalRough = oloReflectSampleMap(maps.Textures.y, uv, footprint).bg;
        metallic *= metalRough.x;
        hitRoughness *= metalRough.y;
    }
    if ((maps.Flags & OLO_GPU_SCENE_MATERIAL_NORMAL_MAP) != 0u)
    {
        const vec2 mappedNormal = oloReflectSampleMap(maps.Textures.z, uv, footprint).xy;
        hitNormal = OloRtApplyNormalMap(hitNormal, p0, p1, p2, uv0, uv1, uv2, mappedNormal, material.NormalScale);
        if (dot(hitNormal, geometricNormal) <= 0.0)
            hitNormal = geometricNormal;
    }
    if ((maps.Flags & OLO_GPU_SCENE_MATERIAL_OCCLUSION_MAP) != 0u)
        occlusion = mix(1.0, oloReflectSampleMap(maps.Textures.w, uv, footprint).r,
                        clamp(material.OcclusionStrength, 0.0, 1.0));
    if ((maps.Flags & OLO_GPU_SCENE_MATERIAL_EMISSIVE_MAP) != 0u)
        emissive *= oloReflectSampleMap(maps.Emissive, uv, footprint).rgb;

    if (!frontFace)
    {
        geometricNormal = -geometricNormal;
        hitNormal = -hitNormal;
    }
    if (!(dot(hitNormal, R) < 0.0))
        hitNormal = geometricNormal;

    // A single directional light and at most one shadow ray. Keep a finite
    // minimum lobe width at this secondary vertex: it has no finite sun disk
    // or specular antialiasing, so a sharper punctual highlight would alias.
    metallic = clamp(metallic, 0.0, 1.0);
    hitRoughness = clamp(hitRoughness, 0.2, 1.0);
    vec3 hitRadiance = emissive;
    if (u_SunDirection.w > 0.5)
    {
        const vec3 L = normalize(u_SunDirection.xyz);
        const float NdotL = dot(hitNormal, L);
        if (NdotL > 0.0 && dot(geometricNormal, L) > 0.0 && SunVisible(hitPos, geometricNormal))
        {
            const vec3 brdf = evaluatePBRClosure(int(material.ClosureVersion), hitNormal, -R, L,
                                                 albedo, metallic, hitRoughness);
            hitRadiance += brdf * u_SunColor.rgb * NdotL;
        }
    }

    // The existing high-mip diffuse environment approximation. No extra
    // indirect bounce or specular environment lookup at the secondary hit.
    if (u_RoughnessGate.w > 0.5)
        hitRadiance += albedo * ((1.0 - metallic) * occlusion) *
                       textureLod(u_PrefilterMap, hitNormal, u_RoughnessGate.z).rgb;

    if (any(isnan(hitRadiance)) || any(isinf(hitRadiance)))
        return;

    // ADR 0020 §4: c_ray = hitValid * roughnessGate, times the artist
    // intensity. Clamped to [0,1] because a confidence outside it is what the
    // contract's Sigma(w) = 1 identity forbids. It says how much of the lobe
    // this estimate may answer for; how much light the lobe reflects is W's.
    const float confidence = clamp(roughnessGate * u_RayParams.z, 0.0, 1.0);

    // The "over", on the specular TERM (issue #1325): the probe/IBL term S the
    // colour already holds is replaced by W * L, the same weight times this
    // tier's radiance. Not an add -- that is the double-count #979's non-goal
    // names -- and not a mix of the whole colour, which dimmed everything else.
    const vec3 weight = texture(u_IndirectSpecularWeight, v_TexCoord).rgb;
    const vec3 delta = oloSpecularTierDelta(baseSpecular, weight, hitRadiance, confidence);

    // Alpha carries this tier's confidence to PostProcess_SSRComposite.glsl for
    // the tier debug view, and ONLY when that view is on — see the header.
    o_Color = vec4(max(baseColor + delta, vec3(0.0)),
                   (u_Flags.x > 0.5) ? confidence : 1.0);
    o_Specular = vec4(max(baseSpecular + delta, vec3(0.0)), 1.0);
}
