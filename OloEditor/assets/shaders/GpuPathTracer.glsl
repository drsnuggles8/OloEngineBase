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
// GpuPathTracer.glsl — the GPU reference path tracer. Issue #1055 (#979
// Phase 2).
//
// WHAT THIS IS. A conventional, progressive, unidirectional path tracer that
// traces ray queries against the #978 acceleration structures and shades from
// the #977 GPU Scene records. It is the ORACLE the rest of the #979 roadmap is
// measured against, so it is structured to match the CPU reference tracer
// (Renderer/PathTracing/PathTracer.cpp) TERM BY TERM rather than only in the
// converged image:
//
//   * the same Owen-scrambled Sobol' sampler (include/PathTracerSampler.glsl,
//     the bit-exact twin of PathSampler.h), consuming dimensions in the same
//     order: pixel jitter, then per bounce [emissive NEE select + point],
//     [BSDF lobe + shape], [Russian roulette];
//   * next-event estimation against every punctual light (delta, no MIS, no
//     sampler dimension) plus ONE area-uniform sample of the emissive triangle
//     set, MIS-weighted with the power heuristic against the BSDF density;
//   * a BSDF-sampled emitter hit weighted with the same two densities, where
//     the light density is the GLOBAL 1 / total emissive area — constant over
//     the set because selection is area-proportional, which is what lets the
//     hit side use it without knowing which triangle it struck;
//   * Russian roulette on the max throughput channel after a configurable
//     bounce, clamped to [0.05, 0.95];
//   * a uniform environment (plus, optionally, the frame's prefiltered
//     environment cube) collected at full weight on escape.
//
// THE CLOSURE IS SHARED, NOT RE-DERIVED. Every surface shades with the v2
// closure's Evaluate / Sample / Pdf triple from PBRCommon.glsl, whose C++ twins
// the CPU tracer integrates; ClosureV2GpuParityTest and
// ClosureV2SampleGpuParityTest pin all three on the device. A Legacy material
// is therefore shaded as ClosureV2 here — counted on the CPU side
// (GpuPathTracerStats::LegacyMaterialsShadedAsClosureV2), never silent.
//
// PROGRESSIVE ACCUMULATION. Attachment 1 carries the running radiance SUM with
// the sample COUNT in alpha, extracted into a registry-owned history every
// frame and handed back as u_History the next. The sample index each pixel
// draws is (count + s), so a run of N frames at a fixed seed reproduces the
// same sequence whatever frame index it started on — the determinism the
// issue asks for. Invalidation is not this shader's job: an invalid history
// arrives as u_Flags bit 2 clear, and the count restarts at zero.
//
// WHY A FRAGMENT SHADER. GL_EXT_ray_query is legal in any stage, and a
// fullscreen draw is the idiom RayTracedReflection.glsl and RayTracedShadow.glsl
// established: the history contract copies framebuffer attachments, so the
// accumulation lands as an MRT attachment with no resolve pass in between.
//
// BINDINGS. The TLAS and the emissive triangle table arrive as DEVICE
// ADDRESSES in the one UBO at UBO_RAY_TRACING (65) — the address model
// RayTracingProbe.comp established, because the buffer-binding namespace has
// been full since #978. The GPU Scene tables come from their canonical SSBO
// bindings (15/16/17/9), the safer of the two for an in-frame consumer (see
// RayTracedReflection.glsl's note on mid-frame SetData snapshots). THE
// WITHIN-SHADER RULES HOLD: 65 is also TEX_VSM_PHYSICAL and this shader
// declares no VSM sampler; 9 is also SSBO_FPLUS_POINT_LIGHTS and this shader
// includes nothing from ForwardPlusCommon.glsl.
//
// TWO STANDING LIMITS, both deliberate (issue #1055 scope decisions):
//   * UNTEXTURED (#805). A hit is shaded from BaseColorFactor / MetallicFactor
//     / RoughnessFactor / EmissiveFactor; sampling a material's textures needs
//     the shader-visible sampler heap. The parity scenes are untextured by
//     construction, so the oracle is reachable without it.
//   * MASKED GEOMETRY TRACES AS SOLID, the same trade the shadow and
//     reflection tiers make: an alpha test needs the texture fetch #805 gates.
// =============================================================================

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require

// Attachment 0: the colour the post chain consumes (RGBA16F). The debug view
// selects what lands here; radiance by default.
layout(location = 0) out vec4 o_Color;
// Attachment 1: rgb = radiance SUM, a = sample COUNT. RGBA32F; extracted into
// the PathTracerHistory plane every frame.
layout(location = 1) out vec4 o_Accum;
// Attachment 2: rgb = sum of squared radiance per channel, a = sum of squared
// luminance. RGBA32F; the variance AOV's raw material.
layout(location = 2) out vec4 o_Moments;
// Attachment 3: rgb = sum of first-hit albedo, a = sum of first-hit flags
// (1 per sample that hit anything). RGBA32F.
layout(location = 3) out vec4 o_Albedo;
// Attachment 4: xyz = sum of first-hit world normals (render-relative frame,
// direction only). RGBA32F.
layout(location = 4) out vec4 o_Normal;
// Attachment 5: rgb = the variance of the per-pixel MEAN, per channel; a = 1.
// RGBA16F, a derived channel written every frame for the AOV manifest.
layout(location = 5) out vec4 o_Variance;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_History OLO_HEAP_TEX_2D(1)
#define u_NormalHistory OLO_HEAP_TEX_2D(2)
#define u_PathTracerAlbedoHistory OLO_HEAP_TEX_2D(3)
#define u_SecondMomentsHistory OLO_HEAP_TEX_2D(4)
#define u_PrefilterMap OLO_HEAP_TEX_CUBE(11)
#else
layout(binding = 0) uniform sampler2D u_SceneColor;               // upstream lit colour, passed through when the tracer stands down
layout(binding = 1) uniform sampler2D u_History;                  // last frame's accumulation (attachment 1)
layout(binding = 2) uniform sampler2D u_NormalHistory;            // last frame's normal sum (attachment 4)
layout(binding = 3) uniform sampler2D u_PathTracerAlbedoHistory;  // last frame's albedo sum (attachment 3)
layout(binding = 4) uniform sampler2D u_SecondMomentsHistory;     // last frame's squared sums (attachment 2)
layout(binding = 11) uniform samplerCube u_PrefilterMap;          // TEX_USER_1: the environment, at LOD 0
#endif

#include "include/PBRCommon.glsl"
#include "include/GPUScene.glsl"
// The vertex/index stream references and the 32-byte Vertex layout live in the
// #978 helper; including it reuses those types rather than declaring a second,
// drift-prone copy. Its include guard makes that safe.
#include "include/RayTracingAlphaTest.glsl"
#include "include/GPUSceneInstances.glsl"
#include "include/GPUSceneGeometries.glsl"
#include "include/GPUSceneMaterials.glsl"
#include "include/GPUSceneLights.glsl"
#include "include/PathTracerSampler.glsl"

// One emissive triangle, in the render-relative frame the TLAS is built in.
// Mirrors OloEngine::EmissiveTriangleRecord (EmissiveTriangleTable.h): five
// vec4 so the C++ side uploads the struct verbatim with no packing step.
struct OloPtEmissiveTriangle
{
    vec4 V0;              // xyz vertex 0, w = area
    vec4 V1;              // xyz vertex 1, w unused
    vec4 V2;              // xyz vertex 2, w unused
    vec4 NormalAndCdf;    // xyz winding normal, w = cumulative area fraction (last entry exactly 1)
    vec4 RadianceAndFlags; // rgb emitted radiance, w = 1 when the emitter is two-sided
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OloPtEmissiveTable
{
    OloPtEmissiveTriangle Triangles[];
};

// UBO_RAY_TRACING (65). Mirrored on the CPU by
// UBOStructures::RayTracingPathTracerUBO.
layout(std140, binding = 65) uniform RayTracingPathTracerParams
{
    mat4 u_InvViewProjection;  // clip -> render-relative world, GL clip convention (see GenerateRay)
    vec4 u_CameraPosition;     // xyz render-relative eye, w unused
    uvec4 u_TlasAddress;       // xy = TLAS device address, z = instance mask, w = sampler seed
    uvec4 u_SlotCounts;        // x = instance slots, y = geometry slots, z = material slots, w = light slots
    uvec4 u_EmissiveTable;     // xy = emissive table device address, z = triangle count, w = flags (OLO_PT_FLAG_*)
    uvec4 u_PathParams;        // x = max bounces, y = RR start bounce (0 = off), z = samples per frame, w = max samples (0 = unbounded)
    vec4 u_RayParams;          // x = ray epsilon, y = max radiance clamp (<= 0 off), z = max ray distance, w = environment cube intensity
    vec4 u_Environment;        // rgb = uniform environment radiance, a = emissive area pdf (1 / total area)
    vec4 u_ScreenParams;       // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_DebugParams;        // x = debug view, y = sample-count display scale, z = variance display scale, w unused
};

#define OLO_PT_FLAG_NEE 1u
#define OLO_PT_FLAG_ENVIRONMENT_CUBE 2u
#define OLO_PT_FLAG_HISTORY_VALID 4u

// Debug views — mirror GpuPathTracerDebugView (GpuPathTracerTypes.h).
#define OLO_PT_VIEW_RADIANCE 0
#define OLO_PT_VIEW_ALBEDO 1
#define OLO_PT_VIEW_NORMAL 2
#define OLO_PT_VIEW_VARIANCE 3
#define OLO_PT_VIEW_SAMPLE_COUNT 4

// Compile-time loop bounds with runtime breaks. A uniform-driven loop bound
// cannot be unrolled and a bad upload could hang the GPU (RayTracedShadow.glsl's
// rule); the CPU side clamps the settings to these same limits.
#define OLO_PT_MAX_BOUNCES 32u
#define OLO_PT_MAX_SAMPLES_PER_FRAME 64u
#define OLO_PT_MAX_LIGHTS 256u
#define OLO_PT_MAX_EMISSIVE_SEARCH 32u

// PathTracer.cpp's kEpsilon (ReferenceBRDF.h) — the punctual-light early-outs
// mirror calculateLightContribution's, so the two agree on which lights
// contribute at all.
#define OLO_PT_LIGHT_EPSILON 0.0001

// ---------------------------------------------------------------------------
// Camera — ReferenceCamera::GenerateRay, transcribed.
//
// Screen UV is [0,1]^2 with y DOWN (row 0 == the top row of the image), the
// convention the CPU film uses. Clip space has y UP. The unprojection is at the
// MIDDLE of the depth range rather than at a clip plane, then aims from the eye
// through it: robust to the depth convention, where a near/far difference
// vector would point backwards under one of them.
//
// u_InvViewProjection is inverse(P * V) in the engine's GL clip convention —
// deliberately NOT the backend-adjusted inverse RHIProjectionSeam.h hands
// uv/depth reconstructions. This shader never derives a ray from a backend
// texture coordinate; it derives it from the CPU film's pixel index, so the
// CPU camera's matrix is the right one on both backends.
// ---------------------------------------------------------------------------
vec3 GenerateRay(vec2 screenUV)
{
    const float ndcX = screenUV.x * 2.0 - 1.0;
    const float ndcY = 1.0 - screenUV.y * 2.0;
    const vec4 unprojected = u_InvViewProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    if (!(abs(unprojected.w) > 0.0))
        return vec3(0.0, 0.0, -1.0);
    const vec3 target = unprojected.xyz / unprojected.w;
    const vec3 delta = target - u_CameraPosition.xyz;
    const float len = length(delta);
    if (!(len > 0.0))
        return vec3(0.0, 0.0, -1.0);
    return delta / len;
}

// Offset a ray origin off the surface along the GEOMETRIC normal, on
// whichever side the outgoing direction leaves — PathTracer.cpp's
// OffsetOrigin. The geometric normal (not the shading one) is what actually
// prevents self-hits: an interpolated normal can point into the triangle plane.
vec3 OffsetOrigin(vec3 position, vec3 geometricNormal, vec3 direction, float epsilon)
{
    const float s = dot(geometricNormal, direction) >= 0.0 ? 1.0 : -1.0;
    return position + geometricNormal * (s * epsilon);
}

// ReferenceBRDF.h's PowerHeuristic: (a^2) / (a^2 + b^2), and ZERO (not one)
// when both densities vanish.
float PowerHeuristic(float pdfA, float pdfB)
{
    const float a2 = pdfA * pdfA;
    const float b2 = pdfB * pdfB;
    const float denom = a2 + b2;
    if (!(denom > 0.0))
        return 0.0;
    return a2 / denom;
}

// ReferenceBRDF.h's CalculateSpotIntensity rather than PBRCommon's
// calculateSpotIntensity: the CPU twin guards a zero-width cone (inner ==
// outer) and PBRCommon's divides by it. The oracle has to agree with the
// oracle, so the guarded form is the one transcribed here.
float PtSpotIntensity(vec3 l, vec3 spotDir, vec4 spotParams)
{
    const float innerCutoff = spotParams.x;
    const float outerCutoff = spotParams.y;
    const float theta = dot(l, normalize(-spotDir));
    const float epsilon = innerCutoff - outerCutoff;
    if (!(abs(epsilon) > 0.0))
        return theta >= innerCutoff ? 1.0 : 0.0;
    const float intensity = clamp((theta - outerCutoff) / epsilon, 0.0, 1.0);
    return intensity * intensity;
}

// ---------------------------------------------------------------------------
// Scene queries
// ---------------------------------------------------------------------------

struct PtHit
{
    bool Hit;
    float Distance;
    vec3 Position;
    vec3 GeometricNormal; // winding orientation, UNFLIPPED — the emitter face test needs it
    vec3 ShadingNormal;   // interpolated, snapped to the geometric side
    vec3 Albedo;
    float Metallic;
    float Roughness;
    vec3 Emissive;
    bool TwoSidedEmission;
    // A committed hit on geometry the GPU Scene cannot shade (see
    // UnshadeableHit). The path ends there with no contribution.
    bool Unshadeable;
};

// A committed intersection whose instance, geometry or material record is
// out of range, tombstoned or inactive: geometry the TLAS still holds but the
// tables no longer describe (a slot retired the same frame the structure was
// built). It IS in the way of the ray, so reporting it as a miss would credit
// the path with environment radiance through an occluder — a furnace scene
// would brighten behind it. It is reported as a hit on a black, opaque
// surface instead: the path ends there. The frame it happens in is one the
// GPU Scene commit also reported dirty, so the SceneMutated invalidation
// discards its samples.
bool UnshadeableHit(vec3 origin, vec3 direction, float t, inout PtHit hit)
{
    hit.Hit = true;
    hit.Unshadeable = true;
    hit.Distance = t;
    hit.Position = origin + direction * t;
    return true;
}

// The hit triangle's vertices, in WORLD (render-relative) space, from the
// geometry record's device addresses through the ray query's own
// object-to-world matrix — the same source RayTracedReflection.glsl's
// HitWorldNormal uses, for the same reason: the ray is already in the space
// the TLAS was built in, and deriving the basis from a second source is how a
// transpose bug gets in.
void FetchTriangle(GPUSceneGeometry geometry, uint primitiveIndex, mat4x3 objectToWorld,
                   out vec3 p0, out vec3 p1, out vec3 p2, out vec3 n0, out vec3 n1, out vec3 n2)
{
    OloRtIndexStream indices = OloRtIndexStream(geometry.IndexAddress);
    OloRtVertexUVStream vertices = OloRtVertexUVStream(geometry.VertexAddress);

    const uint indexBase = geometry.FirstIndex + primitiveIndex * 3u;
    const uint i0 = uint(int(indices.Indices[indexBase + 0u]) + geometry.BaseVertex);
    const uint i1 = uint(int(indices.Indices[indexBase + 1u]) + geometry.BaseVertex);
    const uint i2 = uint(int(indices.Indices[indexBase + 2u]) + geometry.BaseVertex);

    const uint stride = OLO_RT_VERTEX_STRIDE / 4u;
    const uint normalOffset = OLO_RT_VERTEX_NORMAL_OFFSET / 4u;

    const vec3 lp0 = vec3(vertices.Floats[i0 * stride + 0u], vertices.Floats[i0 * stride + 1u], vertices.Floats[i0 * stride + 2u]);
    const vec3 lp1 = vec3(vertices.Floats[i1 * stride + 0u], vertices.Floats[i1 * stride + 1u], vertices.Floats[i1 * stride + 2u]);
    const vec3 lp2 = vec3(vertices.Floats[i2 * stride + 0u], vertices.Floats[i2 * stride + 1u], vertices.Floats[i2 * stride + 2u]);
    const vec3 ln0 = vec3(vertices.Floats[i0 * stride + normalOffset + 0u], vertices.Floats[i0 * stride + normalOffset + 1u], vertices.Floats[i0 * stride + normalOffset + 2u]);
    const vec3 ln1 = vec3(vertices.Floats[i1 * stride + normalOffset + 0u], vertices.Floats[i1 * stride + normalOffset + 1u], vertices.Floats[i1 * stride + normalOffset + 2u]);
    const vec3 ln2 = vec3(vertices.Floats[i2 * stride + normalOffset + 0u], vertices.Floats[i2 * stride + normalOffset + 1u], vertices.Floats[i2 * stride + normalOffset + 2u]);

    p0 = objectToWorld * vec4(lp0, 1.0);
    p1 = objectToWorld * vec4(lp1, 1.0);
    p2 = objectToWorld * vec4(lp2, 1.0);

    // The 3x3 applied directly rather than as an inverse transpose: exact for
    // rigid and uniformly-scaled instances, which is the only class the CPU
    // reference accepts (ReferenceScene::AddInstance rejects the rest), and a
    // skewed shading normal under non-uniform scale elsewhere.
    const mat3 basis = mat3(objectToWorld[0], objectToWorld[1], objectToWorld[2]);
    n0 = basis * ln0;
    n1 = basis * ln1;
    n2 = basis * ln2;
}

// Closest hit along the ray, resolved through the GPU Scene records. A hit on
// a slot the records cannot vouch for (out of range, tombstoned, a dead
// material) is reported as NO hit: the path then collects the environment,
// which is a defined value rather than whatever the stale record held.
bool TraceClosest(vec3 origin, vec3 direction, float tMax, out PtHit hit)
{
    hit.Hit = false;
    hit.Distance = 0.0;
    hit.Position = vec3(0.0);
    hit.GeometricNormal = vec3(0.0, 1.0, 0.0);
    hit.ShadingNormal = vec3(0.0, 1.0, 0.0);
    hit.Albedo = vec3(0.0);
    hit.Metallic = 0.0;
    hit.Roughness = 1.0;
    hit.Emissive = vec3(0.0);
    hit.TwoSidedEmission = false;
    hit.Unshadeable = false;

    rayQueryEXT rayQuery;
    // Opaque: masked geometry traces as solid (see the header). Every
    // non-Masked class is already flagged opaque by the builder.
    rayQueryInitializeEXT(rayQuery, accelerationStructureEXT(u_TlasAddress.xy), gl_RayFlagsOpaqueEXT,
                          u_TlasAddress.z & 0xFFu, origin, 0.0, direction, tMax);
    rayQueryProceedEXT(rayQuery);

    if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return false;

    const float t = rayQueryGetIntersectionTEXT(rayQuery, true);

    const uint instanceSlot = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true));
    if (instanceSlot >= u_SlotCounts.x)
        return UnshadeableHit(origin, direction, t, hit);
    const GPUSceneInstance instance = g_GPUSceneInstances[instanceSlot];
    // A tombstoned slot keeps its generation when the counter saturates, so
    // the Active bit is the only safe test — GPUSceneTypes.h's rule.
    if ((instance.Flags & OLO_GPU_SCENE_INSTANCE_ACTIVE) == 0u)
        return UnshadeableHit(origin, direction, t, hit);
    if (instance.MaterialIndex >= u_SlotCounts.z || instance.GeometryIndex >= u_SlotCounts.y)
        return UnshadeableHit(origin, direction, t, hit);
    const GPUSceneGeometry geometry = g_GPUSceneGeometries[instance.GeometryIndex];
    const GPUSceneMaterial material = g_GPUSceneMaterials[instance.MaterialIndex];
    if ((geometry.Flags & OLO_GPU_SCENE_GEOMETRY_ACTIVE) == 0u ||
        (material.Flags & OLO_GPU_SCENE_MATERIAL_ACTIVE) == 0u)
        return UnshadeableHit(origin, direction, t, hit);
    const vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
    const uint primitiveIndex = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true));
    const mat4x3 objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rayQuery, true);

    vec3 p0, p1, p2, n0, n1, n2;
    FetchTriangle(geometry, primitiveIndex, objectToWorld, p0, p1, p2, n0, n1, n2);

    // The geometric normal from the WORLD-space winding, so it is exact under
    // any affine instance transform; the CPU derives it from the same cross
    // product on the same three vertices.
    const vec3 windingCross = cross(p1 - p0, p2 - p0);
    const float windingLen = length(windingCross);
    vec3 geometricNormal = (windingLen > 1e-12) ? (windingCross / windingLen) : vec3(0.0, 1.0, 0.0);

    const float b0 = 1.0 - barycentrics.x - barycentrics.y;
    vec3 shadingNormal = n0 * b0 + n1 * barycentrics.x + n2 * barycentrics.y;
    const float shadingLen = length(shadingNormal);
    shadingNormal = (shadingLen > 1e-6) ? (shadingNormal / shadingLen) : geometricNormal;
    // Snapped to the geometric side, like ReferenceScene::Intersect: an
    // interpolated normal facing away from its own triangle is a mesh bug the
    // integrator must not turn into a NaN.
    if (dot(shadingNormal, geometricNormal) < 0.0)
        shadingNormal = geometricNormal;

    hit.Hit = true;
    hit.Distance = t;
    hit.Position = origin + direction * t;
    hit.GeometricNormal = geometricNormal;
    hit.ShadingNormal = shadingNormal;
    // Untextured, per #805: these are the material's FACTORS. See the header.
    hit.Albedo = material.BaseColorFactor.rgb;
    hit.Metallic = material.MetallicFactor;
    hit.Roughness = material.RoughnessFactor;
    hit.Emissive = material.EmissiveFactor.rgb;
    hit.TwoSidedEmission = (material.Flags & OLO_GPU_SCENE_MATERIAL_TWO_SIDED) != 0u;
    return true;
}

// ReferenceScene::IsOccluded — a segment query with BOTH ends inset by
// epsilon; a segment shorter than 2 * epsilon is reported unoccluded.
bool IsOccluded(vec3 from, vec3 to, float epsilon)
{
    const vec3 delta = to - from;
    const float dist = length(delta);
    if (!(dist > 2.0 * epsilon))
        return false;

    rayQueryEXT shadowQuery;
    rayQueryInitializeEXT(shadowQuery, accelerationStructureEXT(u_TlasAddress.xy),
                          gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, u_TlasAddress.z & 0xFFu,
                          from, epsilon, delta / dist, dist - epsilon);
    rayQueryProceedEXT(shadowQuery);
    return rayQueryGetIntersectionTypeEXT(shadowQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// ---------------------------------------------------------------------------
// Emissive geometry — ReferenceScene::SampleEmissive.
//
// Area-proportional triangle selection through the table's cumulative area
// fraction, then a uniform barycentric point (Turk's square-root warp). Uniform
// over AREA rather than over triangles is what makes the density the single
// constant u_Environment.a, which the BSDF-hit MIS side reuses without knowing
// which triangle it hit.
// ---------------------------------------------------------------------------
bool SampleEmissive(float xiSelect, vec2 xiPoint, out vec3 position, out vec3 normal, out vec3 radiance,
                    out bool twoSided)
{
    const uint count = u_EmissiveTable.z;
    if (count == 0u || !(u_Environment.a > 0.0))
        return false;

    OloPtEmissiveTable table = OloPtEmissiveTable(u_EmissiveTable.xy);

    // std::lower_bound on the CDF: the first entry whose cumulative fraction
    // is >= xiSelect. A bounded binary search; 2^32 entries would need 32 steps.
    uint lo = 0u;
    uint hi = count;
    for (uint step = 0u; step < OLO_PT_MAX_EMISSIVE_SEARCH; ++step)
    {
        if (lo >= hi)
            break;
        const uint mid = lo + (hi - lo) / 2u;
        if (table.Triangles[mid].NormalAndCdf.w < xiSelect)
            lo = mid + 1u;
        else
            hi = mid;
    }
    const uint triangleIndex = min(lo, count - 1u);
    const OloPtEmissiveTriangle emitter = table.Triangles[triangleIndex];

    const float sqrtU = sqrt(clamp(xiPoint.x, 0.0, 1.0));
    const float b0 = 1.0 - sqrtU;
    const float b1 = clamp(xiPoint.y, 0.0, 1.0) * sqrtU;
    const float b2 = 1.0 - b0 - b1;

    position = emitter.V0.xyz * b0 + emitter.V1.xyz * b1 + emitter.V2.xyz * b2;
    normal = emitter.NormalAndCdf.xyz;
    radiance = emitter.RadianceAndFlags.rgb;
    twoSided = emitter.RadianceAndFlags.w > 0.5;
    return true;
}

// ---------------------------------------------------------------------------
// Direct lighting at a shading point — PathTracer.cpp's SampleDirectLighting.
// `geometricNormal` and `n` are the viewer-side (flipped) normals.
// ---------------------------------------------------------------------------
vec3 SampleDirectLighting(PtHit hit, vec3 geometricNormal, vec3 n, vec3 v, float rayEpsilon,
                          inout OloPathSampler pathSampler)
{
    vec3 direct = vec3(0.0);

    // Every punctual light, each with its own shadow ray: deterministic,
    // low-variance, and consuming NO sampler dimension. u_SlotCounts.w is
    // already clamped to OLO_PT_MAX_LIGHTS by the pass, which counts the live
    // lights the clamp cut off (GpuPathTracerStats::PunctualLightsBeyondShaderBound).
    const uint lightCount = u_SlotCounts.w;
    for (uint i = 0u; i < OLO_PT_MAX_LIGHTS; ++i)
    {
        if (i >= lightCount)
            break;
        const GPUSceneLight light = g_GPUSceneLights[i];
        if ((light.Flags & OLO_GPU_SCENE_LIGHT_ACTIVE) == 0u)
            continue;

        vec3 l = vec3(0.0);
        float attenuation = 1.0;
        vec3 shadowTarget = vec3(0.0);

        if (light.Type == OLO_GPU_SCENE_LIGHT_DIRECTIONAL)
        {
            l = normalize(-light.DirectionAndRadius.xyz);
            // Infinitely far away: probe far enough to leave the scene.
            shadowTarget = hit.Position + l * u_RayParams.z;
        }
        else if (light.Type == OLO_GPU_SCENE_LIGHT_POINT || light.Type == OLO_GPU_SCENE_LIGHT_SPOT)
        {
            const vec3 lightPos = light.PositionAndRange.xyz;
            const vec3 toLight = lightPos - hit.Position;
            const float dist = length(toLight);
            if (!(dist > 0.0))
                continue;
            l = toLight / dist;
            // (constant, linear, quadratic, range) packed the way Scene.cpp
            // fills the MultiLight UBO and ReferenceSceneBuilder fills
            // ReferenceLight::AttenuationParams: (1, 0, attenuation, range).
            attenuation = calculateAttenuation(lightPos, hit.Position,
                                               vec4(1.0, 0.0, light.ShapeParams.z, light.PositionAndRange.w));
            if (light.Type == OLO_GPU_SCENE_LIGHT_SPOT)
            {
                attenuation *= PtSpotIntensity(l, light.DirectionAndRadius.xyz,
                                               vec4(light.ShapeParams.x, light.ShapeParams.y, light.ShapeParams.w, 1.0));
            }
            shadowTarget = lightPos;
        }
        else
        {
            // Sphere-area lights have no reference twin (ReferenceLightType has
            // three rows), so neither oracle sees them. Counted on the CPU side.
            continue;
        }

        if (attenuation <= OLO_PT_LIGHT_EPSILON)
            continue;
        const float nDotL = max(dot(n, l), 0.0);
        if (nDotL <= OLO_PT_LIGHT_EPSILON)
            continue;

        const vec3 shadowOrigin = OffsetOrigin(hit.Position, geometricNormal, l, rayEpsilon);
        if (IsOccluded(shadowOrigin, shadowTarget, rayEpsilon))
            continue;

        const vec3 radiance = light.ColorAndIntensity.rgb * light.ColorAndIntensity.w * attenuation;
        const vec3 brdf = closureV2Evaluate(n, v, l, hit.Albedo, hit.Metallic, hit.Roughness);
        direct += brdf * radiance * nDotL;
    }

    // ---- emissive geometry, area-sampled, MIS-weighted --------------------
    //
    // The dimensions are consumed UNCONDITIONALLY when the scene has emitters,
    // even if the sample is then rejected, so the sampler dimension index stays
    // aligned across samples of the same pixel — and with the CPU tracer.
    if (u_EmissiveTable.z != 0u)
    {
        const float xiSelect = oloPtGet1D(pathSampler);
        const vec2 xiPoint = oloPtGet2D(pathSampler);

        vec3 lightPosition, lightNormal, lightRadiance;
        bool twoSided;
        if (SampleEmissive(xiSelect, xiPoint, lightPosition, lightNormal, lightRadiance, twoSided))
        {
            const vec3 toLight = lightPosition - hit.Position;
            const float distanceSq = dot(toLight, toLight);
            if (distanceSq > 1e-12)
            {
                const float dist = sqrt(distanceSq);
                const vec3 l = toLight / dist;
                const float nDotL = dot(n, l);
                const float cosLight = dot(lightNormal, -l);
                const float effectiveCosLight = twoSided ? abs(cosLight) : cosLight;

                if (nDotL > 0.0 && effectiveCosLight > 0.0)
                {
                    // Area -> solid-angle change of variables.
                    const float pdfSolidAngle = u_Environment.a * distanceSq / effectiveCosLight;
                    if (pdfSolidAngle > 0.0)
                    {
                        const vec3 shadowOrigin = OffsetOrigin(hit.Position, geometricNormal, l, rayEpsilon);
                        if (!IsOccluded(shadowOrigin, lightPosition, rayEpsilon))
                        {
                            const vec3 brdf = closureV2Evaluate(n, v, l, hit.Albedo, hit.Metallic, hit.Roughness);
                            const float pdfBsdf = closureV2Pdf(n, v, l, hit.Albedo, hit.Metallic, hit.Roughness);
                            const float misWeight = PowerHeuristic(pdfSolidAngle, pdfBsdf);
                            direct += brdf * nDotL * lightRadiance * (misWeight / pdfSolidAngle);
                        }
                    }
                }
            }
        }
    }

    return direct;
}

// The environment collected on escape: a uniform radiance (the furnace lever
// and the only environment the CPU reference has), plus the frame's
// environment cube at LOD 0 scaled by the intensity setting when the frame
// bound one. GUARDED by the flag, because sampling a sampler that was never
// bound is undefined behaviour rather than a zero read.
vec3 EnvironmentRadiance(vec3 direction)
{
    vec3 radiance = u_Environment.rgb;
    if ((u_EmissiveTable.w & OLO_PT_FLAG_ENVIRONMENT_CUBE) != 0u)
        radiance += textureLod(u_PrefilterMap, direction, 0.0).rgb * u_RayParams.w;
    return radiance;
}

// ---------------------------------------------------------------------------
// The integrator — PathTracer::TracePath, transcribed.
// ---------------------------------------------------------------------------
vec3 TracePath(vec3 origin, vec3 direction, inout OloPathSampler pathSampler, out vec3 firstHitAlbedo,
               out vec3 firstHitNormal, out float firstHitFlag)
{
    firstHitAlbedo = vec3(0.0);
    firstHitNormal = vec3(0.0);
    firstHitFlag = 0.0;

    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);

    // The camera ray is treated as a "delta" scatter: an emitter seen directly
    // is added at full weight because NEE never had a chance to sample it.
    bool previousScatterWasDelta = true;
    float previousBsdfPdf = 0.0;

    const bool nee = (u_EmissiveTable.w & OLO_PT_FLAG_NEE) != 0u;
    const bool neeSamplesEmitters = nee && u_EmissiveTable.z != 0u;
    const uint maxBounces = u_PathParams.x;
    const uint rrStart = u_PathParams.y;
    const float rayEpsilon = u_RayParams.x;
    const float maxRayDistance = u_RayParams.z;

    for (uint bounce = 0u; bounce < OLO_PT_MAX_BOUNCES; ++bounce)
    {
        if (bounce >= maxBounces)
            break;

        PtHit hit;
        if (!TraceClosest(origin, direction, maxRayDistance, hit))
        {
            // The environment is never NEE-sampled, so it arrives at full weight.
            radiance += throughput * EnvironmentRadiance(direction);
            break;
        }
        // Stopped by geometry the tables cannot shade: a black opaque surface.
        if (hit.Unshadeable)
            break;

        const vec3 v = -direction;

        if (bounce == 0u)
        {
            firstHitAlbedo = hit.Albedo;
            firstHitFlag = 1.0;
        }

        // ---- emitted radiance ------------------------------------------
        const float cosEmitter = dot(hit.GeometricNormal, v);
        const bool emitterFaceVisible = hit.TwoSidedEmission ? (abs(cosEmitter) > 0.0) : (cosEmitter > 0.0);
        if (emitterFaceVisible && max(hit.Emissive.r, max(hit.Emissive.g, hit.Emissive.b)) > 0.0)
        {
            float misWeight = 1.0;
            if (!previousScatterWasDelta && neeSamplesEmitters)
            {
                // This vertex could also have been reached by the NEE sample
                // taken at the PREVIOUS vertex; weight the two strategies with
                // the same densities NEE used.
                const float effectiveCos = hit.TwoSidedEmission ? abs(cosEmitter) : cosEmitter;
                if (effectiveCos > 0.0)
                {
                    const float pdfLightSolidAngle = u_Environment.a * (hit.Distance * hit.Distance) / effectiveCos;
                    misWeight = PowerHeuristic(previousBsdfPdf, pdfLightSolidAngle);
                }
            }
            radiance += throughput * hit.Emissive * misWeight;
        }

        // ---- shading frame ---------------------------------------------
        // Two-sided shading: flip the normal to the side the viewer is on.
        // Both normals flip together so the ray-offset side stays consistent
        // with shading.
        vec3 shadingNormal = hit.ShadingNormal;
        vec3 geometricNormal = hit.GeometricNormal;
        if (dot(geometricNormal, v) < 0.0)
        {
            shadingNormal = -shadingNormal;
            geometricNormal = -geometricNormal;
        }
        if (bounce == 0u)
            firstHitNormal = shadingNormal;

        // ---- next-event estimation -------------------------------------
        if (nee)
            radiance += throughput * SampleDirectLighting(hit, geometricNormal, shadingNormal, v, rayEpsilon, pathSampler);

        // Last allowed vertex: stop before scattering. (maxBounces == 1 is
        // therefore "direct lighting only".)
        if (bounce + 1u >= maxBounces)
            break;

        // ---- BSDF sample -----------------------------------------------
        // Draw order — Get1D then Get2D, unconditionally — is part of the
        // determinism contract (PathTracer.cpp's SampleBsdf).
        const float lobeXi = oloPtGet1D(pathSampler);
        const vec2 xi = oloPtGet2D(pathSampler);
        const ClosureV2Sample bsdf =
            closureV2SampleBRDF(shadingNormal, v, hit.Albedo, hit.Metallic, hit.Roughness, lobeXi, xi);
        // The documented failure convention: Pdf <= 0 is a terminated path,
        // and Value / Pdf must never be formed there (0/0 is a NaN that would
        // spread through the accumulation).
        if (!(bsdf.Pdf > 0.0))
            break;

        const float nDotL = dot(shadingNormal, bsdf.L);
        throughput *= bsdf.Value * nDotL / bsdf.Pdf;
        previousBsdfPdf = bsdf.Pdf;
        previousScatterWasDelta = false;

        if (!(max(throughput.r, max(throughput.g, throughput.b)) > 0.0))
            break;

        // ---- Russian roulette ------------------------------------------
        // The MAX CHANNEL of the throughput, clamped to [0.05, 0.95]; the draw
        // is taken only when the branch is reached, like the CPU.
        if (rrStart > 0u && bounce + 1u >= rrStart)
        {
            const float survival = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
            if (oloPtGet1D(pathSampler) >= survival)
                break;
            throughput /= survival;
        }

        origin = OffsetOrigin(hit.Position, geometricNormal, bsdf.L, rayEpsilon);
        direction = bsdf.L;
    }

    if (u_RayParams.y > 0.0)
        radiance = min(radiance, vec3(u_RayParams.y));

    // A NaN here would spread through the accumulation buffer and poison every
    // region mean read from it. Drop the sample instead, as the CPU does.
    if (any(isnan(radiance)) || any(isinf(radiance)))
        return vec3(0.0);
    return radiance;
}

void main()
{
    const ivec2 size = ivec2(u_ScreenParams.xy);
    const ivec2 frag = ivec2(gl_FragCoord.xy);

    // The CPU film's row 0 is the TOP of the image. On Vulkan the fullscreen
    // triangle lands row 0 at the top (framebuffer y points down and the draw
    // applies no projection, so no seam flip is involved); on OpenGL row 0 is
    // the bottom. Only the pixel SEED and the primary ray depend on this; the
    // history planes are fetched at the fragment's own position, which is the
    // same layout this pass wrote them in.
#ifdef OLO_VULKAN
    const ivec2 pixel = frag;
#else
    const ivec2 pixel = ivec2(frag.x, size.y - 1 - frag.y);
#endif

    // Every early-out below must leave a DEFINED value in every attachment:
    // the history extraction runs whether or not this fragment traced, and a
    // transient's leftovers would be published as valid history.
    const vec4 passThrough = vec4(texture(u_SceneColor, v_TexCoord).rgb, 1.0);
    o_Color = passThrough;
    o_Variance = vec4(0.0, 0.0, 0.0, 1.0);

    // The imported sums, or zero when the registry restarted them.
    vec4 accum = vec4(0.0);
    vec4 moments = vec4(0.0);
    vec4 albedo = vec4(0.0);
    vec4 normalSum = vec4(0.0);
    if ((u_EmissiveTable.w & OLO_PT_FLAG_HISTORY_VALID) != 0u)
    {
        accum = texelFetch(u_History, frag, 0);
        moments = texelFetch(u_SecondMomentsHistory, frag, 0);
        albedo = texelFetch(u_PathTracerAlbedoHistory, frag, 0);
        normalSum = texelFetch(u_NormalHistory, frag, 0);
        // A poisoned history (a NaN from a driver fault, a torn copy) must not
        // be carried forever: restart the pixel rather than propagate it.
        if (any(isnan(accum)) || any(isinf(accum)) || !(accum.a >= 0.0))
        {
            accum = vec4(0.0);
            moments = vec4(0.0);
            albedo = vec4(0.0);
            normalSum = vec4(0.0);
        }
    }
    // Carried forward UNCHANGED on a stood-down frame: the registry is the
    // only thing that restarts the sum, and the pass's per-pixel count
    // mirrors it. A frame that zeroed them here would restart the image
    // behind the registry's back and leave the count claiming a converged
    // image for a one-frame one.
    o_Accum = accum;
    o_Moments = moments;
    o_Albedo = albedo;
    o_Normal = normalSum;

    // The TLAS address is zero when no acceleration structure has been built.
    // Tracing against it is undefined behaviour at rayQueryInitializeEXT, not
    // a miss, so this is a guard rather than an optimisation — and the
    // pass-through above is the structural fallback: the raster frame shows
    // and the pass reports why (GpuPathTracerFallbackReason).
    if ((u_TlasAddress.x | u_TlasAddress.y) == 0u || size.x <= 0 || size.y <= 0)
        return;

    // The sample index each pixel draws next is its accumulated COUNT: a run
    // at a fixed seed reproduces the same sequence whatever frame it started
    // on. Stored as a float, exact up to 2^24 samples.
    const uint sampleBase = uint(accum.a + 0.5);
    const uint maxSamples = u_PathParams.w;
    uint samplesThisFrame = u_PathParams.z;
    if (maxSamples != 0u)
        samplesThisFrame = (sampleBase >= maxSamples) ? 0u : min(samplesThisFrame, maxSamples - sampleBase);

    const uint pixelSeed = oloPtMakePixelSeed(uint(pixel.x), uint(pixel.y), u_TlasAddress.w);

    for (uint s = 0u; s < OLO_PT_MAX_SAMPLES_PER_FRAME; ++s)
    {
        if (s >= samplesThisFrame)
            break;

        OloPathSampler pathSampler = oloPtMakeSampler(pixelSeed, sampleBase + s);

        // Jitter within the pixel footprint (box filter), dims 0 and 1.
        const vec2 jitter = oloPtGet2D(pathSampler);
        const vec2 screenUV = (vec2(pixel) + jitter) * u_ScreenParams.zw;

        vec3 firstHitAlbedo;
        vec3 firstHitNormal;
        float firstHitFlag;
        const vec3 L = TracePath(u_CameraPosition.xyz, GenerateRay(screenUV), pathSampler, firstHitAlbedo,
                                 firstHitNormal, firstHitFlag);

        accum += vec4(L, 1.0);
        moments += vec4(L * L, closureV2Luminance(L) * closureV2Luminance(L));
        albedo += vec4(firstHitAlbedo, firstHitFlag);
        normalSum += vec4(firstHitNormal, 0.0);
    }

    const float count = accum.a;
    const float invCount = (count > 0.0) ? (1.0 / count) : 0.0;
    const vec3 mean = accum.rgb * invCount;
    const vec3 meanSq = moments.rgb * invCount;
    // Variance of the per-pixel MEAN estimate: (E[x^2] - E[x]^2) / n. The
    // max() absorbs the negative rounding a converged pixel produces.
    const vec3 varianceOfMean = max(meanSq - mean * mean, vec3(0.0)) * invCount;

    o_Accum = accum;
    o_Moments = moments;
    o_Albedo = albedo;
    o_Normal = normalSum;
    o_Variance = vec4(varianceOfMean, 1.0);

    const int view = int(u_DebugParams.x + 0.5);
    vec3 shown = mean;
    if (view == OLO_PT_VIEW_ALBEDO)
        shown = albedo.rgb * invCount;
    else if (view == OLO_PT_VIEW_NORMAL)
    {
        const float len = length(normalSum.xyz);
        shown = (len > 1e-6) ? (normalSum.xyz / len) * 0.5 + 0.5 : vec3(0.0);
    }
    else if (view == OLO_PT_VIEW_VARIANCE)
        shown = varianceOfMean * u_DebugParams.z;
    else if (view == OLO_PT_VIEW_SAMPLE_COUNT)
        shown = vec3(count * u_DebugParams.y);

    o_Color = vec4(max(shown, vec3(0.0)), 1.0);
}
