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
// hierarchy BOTTOM-UP: every tier lerps over whatever is already in the colour
// it was handed, so no tier ever has to know the confidence of a tier ABOVE it.
// This pass therefore runs AFTER DeferredLighting (whose output already carries
// the probe-over-IBL blend, the two tiers below this one) and BEFORE
// SSRRenderPass (the tier above). SSR's existing delta composite,
// `base + (refl - base) * blend`, then lerps over THIS pass's output by its own
// confidence — which is exactly the "over" the contract asks for, with not one
// line of SSR's five-stage denoiser chain touched.
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
// Hit lighting retains the tier's material-factor approximation. Masked
// geometry uses actual albedo alpha from the raster material sampler heap,
// with the canonical material cutoff shared by shadows and reflections.
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

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_PrefilterMap OLO_HEAP_TEX_CUBE(11)
#define u_DepthTexture OLO_HEAP_TEX_2D(19)
#define u_GBufferAlbedo OLO_HEAP_TEX_2D(43)
#define u_GBufferNormal OLO_HEAP_TEX_2D(44)
#else
layout(binding = 0) uniform sampler2D u_SceneColor;      // upstream lit HDR colour (probe/IBL already in it)
layout(binding = 11) uniform samplerCube u_PrefilterMap; // TEX_USER_1: specular pre-filter mip chain
layout(binding = 19) uniform sampler2D u_DepthTexture;   // scene depth (nonlinear, [0,1])
layout(binding = 43) uniform sampler2D u_GBufferAlbedo;  // RT0: rgb = albedo, a = metallic
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
#include "include/RayHitNormalTransform.glsl"

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

// The hit triangle's interpolated shading normal, in WORLD space.
//
// The vertex stream is the same 32-byte OloEngine::Vertex the #978 helper
// fetches UVs from; the normal sits at byte offset 12. Object -> world uses the
// ray query's OWN world-to-object matrix rather than the GPU Scene instance
// transform: the ray is already in whatever space the TLAS was built in, and
// deriving the basis from a second source is how a transpose bug gets in.
//
// The INVERSE TRANSPOSE, via include/RayHitNormalTransform.glsl, which owns the
// algebra and the conventions (#1326). It used to be the object-to-world 3x3
// applied directly, with a comment claiming the correction "needs an inverse
// the ray query does not hand back" — rayQueryGetIntersectionWorldToObjectEXT
// hands exactly that back, and multiplying on the right supplies the transpose,
// so no per-hit inverse is computed.
vec3 HitWorldNormal(GPUSceneGeometry geometry, uint primitiveIndex, vec2 barycentrics, mat4x3 worldToObject)
{
    OloRtIndexStream indices = OloRtIndexStream(geometry.IndexAddress);
    OloRtVertexUVStream vertices = OloRtVertexUVStream(geometry.VertexAddress);

    const uint indexBase = geometry.FirstIndex + primitiveIndex * 3u;
    const int i0 = int(indices.Indices[indexBase + 0u]) + geometry.BaseVertex;
    const int i1 = int(indices.Indices[indexBase + 1u]) + geometry.BaseVertex;
    const int i2 = int(indices.Indices[indexBase + 2u]) + geometry.BaseVertex;

    const uint stride = OLO_RT_VERTEX_STRIDE / 4u;
    const uint normalOffset = 12u / 4u;

    const vec3 n0 = vec3(vertices.Floats[uint(i0) * stride + normalOffset + 0u],
                         vertices.Floats[uint(i0) * stride + normalOffset + 1u],
                         vertices.Floats[uint(i0) * stride + normalOffset + 2u]);
    const vec3 n1 = vec3(vertices.Floats[uint(i1) * stride + normalOffset + 0u],
                         vertices.Floats[uint(i1) * stride + normalOffset + 1u],
                         vertices.Floats[uint(i1) * stride + normalOffset + 2u]);
    const vec3 n2 = vec3(vertices.Floats[uint(i2) * stride + normalOffset + 0u],
                         vertices.Floats[uint(i2) * stride + normalOffset + 1u],
                         vertices.Floats[uint(i2) * stride + normalOffset + 2u]);

    const float b0 = 1.0 - barycentrics.x - barycentrics.y;
    const vec3 objectNormal = n0 * b0 + n1 * barycentrics.x + n2 * barycentrics.y;

    const vec3 worldNormal = oloRtObjectNormalToWorld(objectNormal, worldToObject);

    // A degenerate interpolated normal (a mesh with unnormalised or zero
    // normals), or a SINGULAR instance transform, would come back as a NaN from
    // normalize() and poison the whole composite through bloom. This tier has no
    // geometric normal to fall back on — it never fetches the hit triangle's
    // positions — so world up is the documented finite substitute, and the
    // caller's face-the-normal-back-along-the-ray step below makes it usable
    // rather than merely defined. The reflection is then wrong on that hit, and
    // visibly so: a flat horizontal shading normal on a surface that is not
    // horizontal reads as a hard patch, not as a plausible image.
    vec3 result;
    return oloRtNormalizeHitNormal(worldNormal, result) ? result : vec3(0.0, 1.0, 0.0);
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

    // Fresnel (Schlick) against the macrosurface normal — this slice traces the
    // mirror direction, so the microfacet normal IS N and the two agree.
    const vec4 gAlbedo = texture(u_GBufferAlbedo, v_TexCoord);
    const vec3 albedo = gAlbedo.rgb;
    const float metallic = gAlbedo.a;
    const vec3 F0 = mix(vec3(0.04), albedo, metallic);
    const float cosTheta = clamp(dot(-V, N), 0.0, 1.0);
    const vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
    const float fresnelScalar = dot(fresnel, vec3(0.299, 0.587, 0.114)); // perceptual reflectance

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
    if ((geometry.Flags & OLO_GPU_SCENE_GEOMETRY_ACTIVE) == 0u ||
        (material.Flags & OLO_GPU_SCENE_MATERIAL_ACTIVE) == 0u)
        return;

    const float hitT = rayQueryGetIntersectionTEXT(rayQuery, true);
    const vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
    const uint primitiveIndex = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true));
    const mat4x3 worldToObject = rayQueryGetIntersectionWorldToObjectEXT(rayQuery, true);

    const vec3 hitPos = worldPos + N * u_RayParams.y + R * hitT;
    vec3 hitNormal = HitWorldNormal(geometry, primitiveIndex, barycentrics, worldToObject);
    // Face the normal back along the incoming ray. A ray that hits the inside
    // of a closed mesh (or a single-sided wall from behind) would otherwise be
    // shaded by a normal pointing away from it and come back black.
    //
    // This is also this tier's MIRRORED-INSTANCE and two-sided answer, and it
    // needs no winding sign (#1326): every hit here is shaded two-sided by
    // construction, so the only thing the instance's handedness could change is
    // which of the two antipodes is picked — and the ray, not the winding,
    // picks that. A tier that snapped to a winding-derived geometric normal
    // instead would need oloRtInstanceWindingSign(); see RayTracedSurfaceHit.
    if (dot(hitNormal, R) > 0.0)
        hitNormal = -hitNormal;

    // ---- Shade the hit ----------------------------------------------------
    // Untextured, per #805: these are the material's FACTORS. See the header.
    const vec3 hitAlbedo = material.BaseColorFactor.rgb;
    const vec3 hitEmissive = material.EmissiveFactor.rgb;
    // The body lobe's reflectance (issue #1336): albedo * (1 - metallic), the
    // diffuse weight the raster ladder and the ReSTIR GI bounce vertex use, so
    // a metal hit reflects no diffuse light — and the Lambert 1/pi below, so a
    // reflected surface is exactly as bright as the same surface seen directly.
    const vec3 hitDiffuseReflectance = hitAlbedo * (1.0 - clamp(material.MetallicFactor, 0.0, 1.0));

    vec3 hitRadiance = hitEmissive;

    if (u_SunDirection.w > 0.5)
    {
        const vec3 L = normalize(u_SunDirection.xyz);
        const float NdotL = max(dot(hitNormal, L), 0.0);
        if (NdotL > 0.0 && SunVisible(hitPos, hitNormal))
        {
            // Lambertian only. A specular lobe at the hit would need the view
            // vector of a secondary bounce, which this slice does not trace.
            // u_SunColor is the directional light's colour * intensity — the
            // number the raster loop multiplies by albedo / pi — so the 1/pi is
            // not optional: without it a sunlit reflection read pi times
            // brighter than the surface it reflects (issue #1336).
            hitRadiance += hitDiffuseReflectance * (1.0 / 3.14159265359) * u_SunColor.rgb * NdotL;
        }
    }

    // Ambient at the hit, from the environment pre-filter at a high LOD — the
    // cheapest stand-in for irradiance that is already resident. It is the same
    // environment the probe/IBL tier below uses, so a reflected surface and the
    // surface itself agree about what the sky is.
    //
    // GUARDED, because the pass only binds the cube when the frame HAS one: the
    // blackboard's PrefilterMap is optional, and sampling a sampler that was
    // never bound is undefined behaviour rather than a zero read. Without an
    // environment a hit is lit by the sun alone — darker than it should be, but
    // a defined value rather than whatever the unit last held.
    if (u_RoughnessGate.w > 0.5)
        hitRadiance += hitDiffuseReflectance * textureLod(u_PrefilterMap, hitNormal, u_RoughnessGate.z).rgb;

    // Metals tint their reflection by albedo; dielectrics reflect untinted —
    // the same construction PostProcess_SSR.glsl uses, kept identical so the
    // two tiers hand off without a colour shift at the boundary.
    const vec3 reflTint = mix(vec3(1.0), albedo, metallic);
    const vec3 reflTarget = hitRadiance * reflTint;

    // ADR 0020 §4: c_ray = hitValid * fresnel * roughnessGate, times the artist
    // intensity. Clamped to [0,1] because a confidence outside it is what the
    // contract's Sigma(w) = 1 identity forbids.
    const float confidence = clamp(fresnelScalar * roughnessGate * u_RayParams.z, 0.0, 1.0);

    // The "over". Not an add: baseColor already contains the probe/IBL
    // reflection this tier is a better answer for, and adding would be exactly
    // the double-count #979's non-goal names.
    const vec3 composited = mix(baseColor, reflTarget, confidence);

    // Alpha carries this tier's confidence to PostProcess_SSRComposite.glsl for
    // the tier debug view, and ONLY when that view is on — see the header.
    o_Color = vec4(max(composited, vec3(0.0)),
                   (u_Flags.x > 0.5) ? confidence : 1.0);
}
