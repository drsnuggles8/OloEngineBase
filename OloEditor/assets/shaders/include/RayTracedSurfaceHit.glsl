// RayTracedSurfaceHit.glsl — what the scene looks like along a ray. Extracted
// verbatim from GpuPathTracer.glsl for issue #1169, so the ReSTIR GI tier's
// bounce and the reference path tracer it is VALIDATED AGAINST cannot disagree
// about it.
//
// WHY IT IS A FILE AND NOT A SECOND COPY. #979's non-negotiable is "the same
// canonical instance / material / light identities as raster and PT — no
// parallel scene", and #1140 already established what that means in practice:
// the light-sampling densities moved to include/LightSampling.glsl rather than
// being re-derived, because a tier that carries its own copy of the oracle's
// measure makes the comparison against that oracle prove nothing. A ray hit is
// the same kind of thing one level up. If ReSTIR GI's bounce interpolated its
// normals, snapped them to the geometric side, applied its normal maps or
// handled an unshadeable slot even slightly differently from the path tracer's,
// the two would shade the same vertex differently and the convergence test would
// be measuring the difference rather than the estimator.
//
// WHAT THE CALLER MUST DEFINE BEFORE INCLUDING (all are expressions, evaluated
// where used):
//
//   OLO_RT_HIT_TLAS_ADDRESS        uvec2  — the acceleration structure
//   OLO_RT_HIT_INSTANCE_MASK       uint   — the cull mask (low 8 bits used)
//   OLO_RT_HIT_SLOT_COUNTS         uvec4  — x instances, y geometries, z materials, w LIVE light slots
//   OLO_RT_HIT_MATERIAL_TABLE      uvec2  — MaterialTextureTable device address
//   OLO_RT_HIT_MATERIAL_COUNT      uint   — records in that table
//   OLO_RT_HIT_SAMPLER_HEAP        uint   — sampler heap byte offset
//   OLO_RT_HIT_TEXTURES_REQUESTED  bool   — the caller's "textures are bound" flag bit
//
// AND MUST HAVE INCLUDED FIRST: include/PBRCommon.glsl, include/GPUScene.glsl
// and the GPUScene table includes, include/DescriptorHeapTextures.glsl, and
// emitted the ray-query / buffer-reference / descriptor-heap #extension
// directives before any other token.
//
// THIS FILE INCLUDES include/LightSampling.glsl AND
// include/RayTracingAlphaTest.glsl ITSELF, and that is deliberate rather than a
// convenience. Both need macros that can only be written once the material
// texture accessors below exist, and the ORDER is load-bearing in a way that is
// not obvious — the same trap include/ReSTIRDISceneAccess.glsl exists to close.
// A caller that included them earlier would get a redefinition; a caller that
// included them later would get an undefined macro. So the order lives here.
#ifndef OLO_RAY_TRACED_SURFACE_HIT_GLSL
#define OLO_RAY_TRACED_SURFACE_HIT_GLSL

#ifndef OLO_RT_HIT_TLAS_ADDRESS
#error "RayTracedSurfaceHit.glsl: define OLO_RT_HIT_TLAS_ADDRESS before including it"
#endif

// ---------------------------------------------------------------------------
// Material textures (ADR 0011 amendment (95); the #805 capability). The
// MaterialTextureTable hands every material SLOT the heap byte offsets of its
// maps; the shader indexes the descriptor heap with them directly. Level 0
// everywhere, as the CPU reference samples: a ray hit has no screen-space
// derivatives, and a reference does not filter.
// ---------------------------------------------------------------------------
// GPUSceneMaterial.ClosureVersion - PBRModel (Legacy 0, ClosureV2 1). A
// property of the SCENE DATA rather than of any one tier, so it lives with the
// hit that carries it. Each hit is shaded with ITS closure, as the CPU
// reference dispatches (PBRClosureBSDF.h): ClosureV2 when the version says so,
// Legacy otherwise - the CPU's default arm, so an unknown version lands on the
// same closure on both. GpuPathTracer.glsl's OLO_PT_CLOSURE_* are the same two
// numbers under its own names, and GpuPathTracerContractTest pins them.
#define OLO_RT_CLOSURE_LEGACY 0u
#define OLO_RT_CLOSURE_V2 1u

struct OloRtMaterialTextures
{
    uint Albedo;
    uint MetallicRoughness;
    uint Normal;
    uint Emissive;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OloRtMaterialTextureTable
{
    OloRtMaterialTextures Records[];
};

bool OloRtTexturesEnabled()
{
    return OLO_RT_HIT_TEXTURES_REQUESTED &&
           (OLO_RT_HIT_MATERIAL_TABLE.x | OLO_RT_HIT_MATERIAL_TABLE.y) != 0u &&
           OLO_RT_HIT_SAMPLER_HEAP != OLO_HEAP_OFFSET_INVALID;
}

OloRtMaterialTextures OloRtMaterialMaps(uint materialIndex)
{
    OloRtMaterialTextures none;
    none.Albedo = OLO_HEAP_OFFSET_INVALID;
    none.MetallicRoughness = OLO_HEAP_OFFSET_INVALID;
    none.Normal = OLO_HEAP_OFFSET_INVALID;
    none.Emissive = OLO_HEAP_OFFSET_INVALID;
    if (!OloRtTexturesEnabled() || materialIndex >= OLO_RT_HIT_MATERIAL_COUNT)
        return none;
    return OloRtMaterialTextureTable(OLO_RT_HIT_MATERIAL_TABLE).Records[materialIndex];
}

vec4 OloRtSampleMaterialTexture(uint textureByteOffset, vec2 uv)
{
    return oloHeapSampleLod(textureByteOffset, OLO_RT_HIT_SAMPLER_HEAP, uv, 0.0);
}

// Light sampling and its densities (issue #1140): the emissive triangle table
// and its area sampler, the sphere-light view / cone pdf / point distance, the
// punctual view, and the sphere-light ray intersection. The heap uniforms are
// handed to it as macros, the way RayTracingAlphaTest.glsl takes
// OLO_RT_SAMPLE_ALPHA.
#define OLO_LIGHT_SAMPLE_EMISSIVE_TEXTURE(byteOffset, uv) OloRtSampleMaterialTexture(byteOffset, uv)
#define OLO_LIGHT_EMISSIVE_TEXTURES_ENABLED OloRtTexturesEnabled()
#include "LightSampling.glsl"

// glTF MASK alpha: baseColorFactor.a times the albedo map's alpha — the raster
// path's own definition (PBR_GBuffer.glsl).
float OloRtSampleAlpha(uint materialIndex, vec2 uv)
{
    const GPUSceneMaterial material = g_GPUSceneMaterials[materialIndex];
    float alpha = material.BaseColorFactor.a;
    const OloRtMaterialTextures maps = OloRtMaterialMaps(materialIndex);
    if (maps.Albedo != OLO_HEAP_OFFSET_INVALID)
        alpha *= OloRtSampleMaterialTexture(maps.Albedo, uv).a;
    return alpha;
}
#define OLO_RT_SAMPLE_ALPHA(materialIndex, uv) OloRtSampleAlpha(materialIndex, uv)
#include "RayTracingAlphaTest.glsl"

// A candidate (non-opaque) intersection the ray query reports: solid when the
// GPU Scene cannot describe it (the same unshadeable-is-opaque rule as the
// committed path) or when the alpha test confirms it. Only masked instances are
// built non-opaque (RayTracingScene's geometry classes), so this runs for masked
// geometry alone.
bool OloRtCandidateIsSolid(uint instanceSlot, uint primitiveIndex, vec2 barycentrics)
{
    if (instanceSlot >= OLO_RT_HIT_SLOT_COUNTS.x)
        return true;
    const GPUSceneInstance instance = g_GPUSceneInstances[instanceSlot];
    if ((instance.Flags & OLO_GPU_SCENE_INSTANCE_ACTIVE) == 0u)
        return true;
    if (instance.MaterialIndex >= OLO_RT_HIT_SLOT_COUNTS.z ||
        instance.GeometryIndex >= OLO_RT_HIT_SLOT_COUNTS.y)
        return true;
    const GPUSceneGeometry geometry = g_GPUSceneGeometries[instance.GeometryIndex];
    const GPUSceneMaterial material = g_GPUSceneMaterials[instance.MaterialIndex];
    if ((geometry.Flags & OLO_GPU_SCENE_GEOMETRY_ACTIVE) == 0u ||
        (material.Flags & OLO_GPU_SCENE_MATERIAL_ACTIVE) == 0u)
        return true;
    return oloRayTracingConfirmCandidate(geometry, material, instance.MaterialIndex, primitiveIndex,
                                         barycentrics);
}

// The ray flags: masked geometry is confirmed per candidate only when the
// textures it needs are reachable; otherwise everything traces as opaque and the
// caller counts MaskedGeometryTracedAsSolid.
uint OloRtRayFlags(uint extra)
{
    return (OloRtTexturesEnabled() ? gl_RayFlagsNoneEXT : gl_RayFlagsOpaqueEXT) | extra;
}

// The analytic tangent frame of the hit triangle from its UV gradients — the
// derivative-free twin of PBRCommon's applyNormalMapTBN, and the SAME formula
// the CPU reference uses (ReferenceScene::ApplyNormalMap), so a normal-mapped
// hit shades identically on both. Degenerate UVs (a constant texcoord over the
// triangle) leave the interpolated normal alone.
vec3 OloRtApplyNormalMap(vec3 n, vec3 p0, vec3 p1, vec3 p2, vec2 uv0, vec2 uv1, vec2 uv2, vec2 sampledXY,
                         float normalScale)
{
    const vec3 e1 = p1 - p0;
    const vec3 e2 = p2 - p0;
    const vec2 d1 = uv1 - uv0;
    const vec2 d2 = uv2 - uv0;
    const float det = d1.x * d2.y - d2.x * d1.y;
    if (abs(det) < 1e-12)
        return n;
    const float inv = 1.0 / det;
    vec3 t = (e1 * d2.y - e2 * d1.y) * inv;
    const vec3 bRaw = (e2 * d1.x - e1 * d2.x) * inv;
    t -= n * dot(n, t);
    const float tLen = length(t);
    if (!(tLen > 1e-12))
        return n;
    t /= tLen;
    const vec3 nCrossT = cross(n, t);
    const vec3 b = dot(nCrossT, bRaw) < 0.0 ? -nCrossT : nCrossT;
    const vec3 tn = decodeTangentNormal(sampledXY, normalScale);
    const vec3 result = t * tn.x + b * tn.y + n * tn.z;
    const float len = length(result);
    return (len > 1e-12) ? result / len : n;
}

// ---------------------------------------------------------------------------
// Scene queries
// ---------------------------------------------------------------------------

struct OloRtHit
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
    // OloRtUnshadeableHit). The path ends there with no contribution.
    bool Unshadeable;
    // The light slot of the sphere light the ray stopped at, or -1. Sphere
    // lights are not in the TLAS; OloRtTraceClosest intersects them
    // analytically.
    int SphereLight;
    // GPUSceneMaterial.ClosureVersion (OLO_PT_CLOSURE_*): which closure the hit
    // is shaded and sampled with.
    uint ClosureVersion;
};

// A committed intersection whose instance, geometry or material record is out of
// range, tombstoned or inactive: geometry the TLAS still holds but the tables no
// longer describe (a slot retired the same frame the structure was built). It IS
// in the way of the ray, so reporting it as a miss would credit the path with
// environment radiance through an occluder — a furnace scene would brighten
// behind it. It is reported as a hit on a black, opaque surface instead: the
// path ends there. The frame it happens in is one the GPU Scene commit also
// reported dirty, so the SceneMutated invalidation discards its samples.
bool OloRtUnshadeableHit(vec3 origin, vec3 direction, float t, inout OloRtHit hit)
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
// HitWorldNormal uses, for the same reason: the ray is already in the space the
// TLAS was built in, and deriving the basis from a second source is how a
// transpose bug gets in.
void OloRtFetchTriangle(GPUSceneGeometry geometry, uint primitiveIndex, mat4x3 objectToWorld,
                        out vec3 p0, out vec3 p1, out vec3 p2, out vec3 n0, out vec3 n1, out vec3 n2,
                        out vec2 uv0, out vec2 uv1, out vec2 uv2)
{
    OloRtIndexStream indices = OloRtIndexStream(geometry.IndexAddress);
    OloRtVertexUVStream vertices = OloRtVertexUVStream(geometry.VertexAddress);

    const uint indexBase = geometry.FirstIndex + primitiveIndex * 3u;
    const uint i0 = uint(int(indices.Indices[indexBase + 0u]) + geometry.BaseVertex);
    const uint i1 = uint(int(indices.Indices[indexBase + 1u]) + geometry.BaseVertex);
    const uint i2 = uint(int(indices.Indices[indexBase + 2u]) + geometry.BaseVertex);

    const uint stride = OLO_RT_VERTEX_STRIDE / 4u;
    const uint normalOffset = OLO_RT_VERTEX_NORMAL_OFFSET / 4u;
    const uint uvOffset = OLO_RT_VERTEX_TEXCOORD_OFFSET / 4u;

    const vec3 lp0 = vec3(vertices.Floats[i0 * stride + 0u], vertices.Floats[i0 * stride + 1u], vertices.Floats[i0 * stride + 2u]);
    const vec3 lp1 = vec3(vertices.Floats[i1 * stride + 0u], vertices.Floats[i1 * stride + 1u], vertices.Floats[i1 * stride + 2u]);
    const vec3 lp2 = vec3(vertices.Floats[i2 * stride + 0u], vertices.Floats[i2 * stride + 1u], vertices.Floats[i2 * stride + 2u]);
    const vec3 ln0 = vec3(vertices.Floats[i0 * stride + normalOffset + 0u], vertices.Floats[i0 * stride + normalOffset + 1u], vertices.Floats[i0 * stride + normalOffset + 2u]);
    const vec3 ln1 = vec3(vertices.Floats[i1 * stride + normalOffset + 0u], vertices.Floats[i1 * stride + normalOffset + 1u], vertices.Floats[i1 * stride + normalOffset + 2u]);
    const vec3 ln2 = vec3(vertices.Floats[i2 * stride + normalOffset + 0u], vertices.Floats[i2 * stride + normalOffset + 1u], vertices.Floats[i2 * stride + normalOffset + 2u]);

    uv0 = vec2(vertices.Floats[i0 * stride + uvOffset + 0u], vertices.Floats[i0 * stride + uvOffset + 1u]);
    uv1 = vec2(vertices.Floats[i1 * stride + uvOffset + 0u], vertices.Floats[i1 * stride + uvOffset + 1u]);
    uv2 = vec2(vertices.Floats[i2 * stride + uvOffset + 0u], vertices.Floats[i2 * stride + uvOffset + 1u]);

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

// Closest hit along the ray, resolved through the GPU Scene records. A hit on a
// slot the records cannot vouch for (out of range, tombstoned, a dead material)
// is an OloRtUnshadeableHit: the ray did stop there, so the path ends with no
// contribution rather than collecting the environment through it.
bool OloRtTraceClosestGeometry(vec3 origin, vec3 direction, float tMax, out OloRtHit hit)
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
    hit.SphereLight = -1;
    hit.ClosureVersion = OLO_RT_CLOSURE_V2;

    rayQueryEXT rayQuery;
    // Opaque unless textures are reachable (see OloRtRayFlags). Every
    // non-Masked class is already flagged opaque by the builder.
    rayQueryInitializeEXT(rayQuery, accelerationStructureEXT(OLO_RT_HIT_TLAS_ADDRESS), OloRtRayFlags(0u),
                          OLO_RT_HIT_INSTANCE_MASK & 0xFFu, origin, 0.0, direction, tMax);
    // Opaque instances commit inside the traversal; a masked instance surfaces
    // as a candidate and is confirmed by its alpha test.
    while (rayQueryProceedEXT(rayQuery))
    {
        if (rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            OloRtCandidateIsSolid(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false)),
                                  uint(rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false)),
                                  rayQueryGetIntersectionBarycentricsEXT(rayQuery, false)))
        {
            rayQueryConfirmIntersectionEXT(rayQuery);
        }
    }

    if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return false;

    const float t = rayQueryGetIntersectionTEXT(rayQuery, true);

    const uint instanceSlot = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true));
    if (instanceSlot >= OLO_RT_HIT_SLOT_COUNTS.x)
        return OloRtUnshadeableHit(origin, direction, t, hit);
    const GPUSceneInstance instance = g_GPUSceneInstances[instanceSlot];
    // A tombstoned slot keeps its generation when the counter saturates, so the
    // Active bit is the only safe test — GPUSceneTypes.h's rule.
    if ((instance.Flags & OLO_GPU_SCENE_INSTANCE_ACTIVE) == 0u)
        return OloRtUnshadeableHit(origin, direction, t, hit);
    if (instance.MaterialIndex >= OLO_RT_HIT_SLOT_COUNTS.z ||
        instance.GeometryIndex >= OLO_RT_HIT_SLOT_COUNTS.y)
        return OloRtUnshadeableHit(origin, direction, t, hit);
    const GPUSceneGeometry geometry = g_GPUSceneGeometries[instance.GeometryIndex];
    const GPUSceneMaterial material = g_GPUSceneMaterials[instance.MaterialIndex];
    if ((geometry.Flags & OLO_GPU_SCENE_GEOMETRY_ACTIVE) == 0u ||
        (material.Flags & OLO_GPU_SCENE_MATERIAL_ACTIVE) == 0u)
        return OloRtUnshadeableHit(origin, direction, t, hit);
    const vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
    const uint primitiveIndex = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true));
    const mat4x3 objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rayQuery, true);

    vec3 p0, p1, p2, n0, n1, n2;
    vec2 uv0, uv1, uv2;
    OloRtFetchTriangle(geometry, primitiveIndex, objectToWorld, p0, p1, p2, n0, n1, n2, uv0, uv1, uv2);

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

    // The material's factors, then its maps where they are reachable: the raster
    // conventions (albedo rgb, metallic = blue, roughness = green, emissive rgb,
    // normal xy), level 0, the CPU reference's definitions.
    vec3 albedo = material.BaseColorFactor.rgb;
    float metallic = material.MetallicFactor;
    float roughness = material.RoughnessFactor;
    vec3 emissive = material.EmissiveFactor.rgb;
    const OloRtMaterialTextures maps = OloRtMaterialMaps(instance.MaterialIndex);
    if (maps.Albedo != OLO_HEAP_OFFSET_INVALID || maps.MetallicRoughness != OLO_HEAP_OFFSET_INVALID ||
        maps.Normal != OLO_HEAP_OFFSET_INVALID || maps.Emissive != OLO_HEAP_OFFSET_INVALID)
    {
        const vec2 uv = uv0 * b0 + uv1 * barycentrics.x + uv2 * barycentrics.y;
        if (maps.Albedo != OLO_HEAP_OFFSET_INVALID)
            albedo *= OloRtSampleMaterialTexture(maps.Albedo, uv).rgb;
        if (maps.MetallicRoughness != OLO_HEAP_OFFSET_INVALID)
        {
            const vec3 metallicRoughness = OloRtSampleMaterialTexture(maps.MetallicRoughness, uv).rgb;
            metallic *= metallicRoughness.b;
            roughness *= metallicRoughness.g;
        }
        if (maps.Emissive != OLO_HEAP_OFFSET_INVALID)
            emissive *= OloRtSampleMaterialTexture(maps.Emissive, uv).rgb;
        if (maps.Normal != OLO_HEAP_OFFSET_INVALID)
        {
            shadingNormal = OloRtApplyNormalMap(shadingNormal, p0, p1, p2, uv0, uv1, uv2,
                                                OloRtSampleMaterialTexture(maps.Normal, uv).xy,
                                                material.NormalScale);
            // Re-snapped: a map can tilt the normal past the geometric plane.
            if (dot(shadingNormal, geometricNormal) < 0.0)
                shadingNormal = geometricNormal;
        }
    }

    hit.Hit = true;
    hit.Distance = t;
    hit.Position = origin + direction * t;
    hit.GeometricNormal = geometricNormal;
    hit.ShadingNormal = shadingNormal;
    hit.Albedo = albedo;
    hit.Metallic = metallic;
    hit.Roughness = roughness;
    hit.Emissive = emissive;
    hit.TwoSidedEmission = (material.Flags & OLO_GPU_SCENE_MATERIAL_TWO_SIDED) != 0u;
    hit.ClosureVersion = material.ClosureVersion;
    return true;
}

// The geometry, then the sphere lights in front of it: a sphere light nearer
// than the committed hit (or a miss) is where the path stops.
bool OloRtTraceClosest(vec3 origin, vec3 direction, float tMax, out OloRtHit hit)
{
    const bool hitGeometry = OloRtTraceClosestGeometry(origin, direction, tMax, hit);
    float sphereT;
    const int sphereLight = OloIntersectSphereLights(origin, direction, hitGeometry ? hit.Distance : tMax,
                                                     OLO_RT_HIT_SLOT_COUNTS.w, sphereT);
    if (sphereLight < 0)
        return hitGeometry;
    hit.Hit = true;
    hit.Unshadeable = false;
    hit.SphereLight = sphereLight;
    hit.Distance = sphereT;
    hit.Position = origin + direction * sphereT;
    return true;
}

// ReferenceScene::IsOccluded — a segment query with BOTH ends inset by epsilon;
// a segment shorter than 2 * epsilon is reported unoccluded.
//
// This is also the ray ReSTIR GI's RECONNECTION test needs (design note §6.2):
// the segment x0' -> x1 never existed in the source path, so nothing in a reused
// reservoir knows whether a wall crosses it. Sharing the query rather than
// writing a second one is what stops the two tiers from disagreeing about what
// is in the way, which is the same rule the light densities follow.
bool OloRtIsOccluded(vec3 from, vec3 to, float epsilon)
{
    const vec3 delta = to - from;
    const float dist = length(delta);
    if (!(dist > 2.0 * epsilon))
        return false;

    rayQueryEXT shadowQuery;
    rayQueryInitializeEXT(shadowQuery, accelerationStructureEXT(OLO_RT_HIT_TLAS_ADDRESS),
                          OloRtRayFlags(gl_RayFlagsTerminateOnFirstHitEXT),
                          OLO_RT_HIT_INSTANCE_MASK & 0xFFu, from, epsilon, delta / dist, dist - epsilon);
    while (rayQueryProceedEXT(shadowQuery))
    {
        if (rayQueryGetIntersectionTypeEXT(shadowQuery, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            OloRtCandidateIsSolid(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(shadowQuery, false)),
                                  uint(rayQueryGetIntersectionPrimitiveIndexEXT(shadowQuery, false)),
                                  rayQueryGetIntersectionBarycentricsEXT(shadowQuery, false)))
        {
            rayQueryConfirmIntersectionEXT(shadowQuery);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(shadowQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT)
        return true;
    // A sphere light in the way blocks the ray too (PathTracer.cpp's
    // ShadowRayBlocked): closest-hit rays stop at spheres, so shadow rays must,
    // or the two MIS strategies disagree about what is visible. The segment ends
    // epsilon short of its target, so a ray aimed AT a sphere light's surface
    // does not count that sphere.
    if (!(dist > 2.0 * epsilon))
        return false;
    float sphereT;
    return OloIntersectSphereLights(from + delta / dist * epsilon, delta / dist, dist - 2.0 * epsilon,
                                    OLO_RT_HIT_SLOT_COUNTS.w, sphereT) >= 0;
}

#endif // OLO_RAY_TRACED_SURFACE_HIT_GLSL
