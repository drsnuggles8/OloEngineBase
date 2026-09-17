#ifndef OLO_HYBRID_RAY_TRACING_ALPHA_GLSL
#define OLO_HYBRID_RAY_TRACING_ALPHA_GLSL

// Both hybrid consumers use the canonical material and the raster material
// heap table. Callers define the slot counts, heap address/count and sampler;
// the shadow pass also requests command-ordered by-address scene tables.
#include "GPUScene.glsl"
#include "DescriptorHeapTextures.glsl"
#include "MaterialShaderHeapTable.glsl"

#ifdef OLO_HYBRID_RT_BUFFER_REFERENCES
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OloHybridRtInstanceTable
{
    GPUSceneInstance Records[];
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OloHybridRtGeometryTable
{
    GPUSceneGeometry Records[];
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OloHybridRtMaterialTable
{
    GPUSceneMaterial Records[];
};
#define g_GPUSceneInstances OloHybridRtInstanceTable(u_InstanceAndGeometryAddresses.xy).Records
#define g_GPUSceneGeometries OloHybridRtGeometryTable(u_InstanceAndGeometryAddresses.zw).Records
#define g_GPUSceneMaterials OloHybridRtMaterialTable(u_MaterialAndHeapAddresses.xy).Records
#endif

float oloHybridRayTracingAlpha(uint materialIndex, vec2 uv)
{
    GPUSceneMaterial material = g_GPUSceneMaterials[materialIndex];
    float alpha = material.BaseColorFactor.a;
    if ((material.Flags & OLO_GPU_SCENE_MATERIAL_ALBEDO_MAP) != 0u)
    {
        OloMaterialShaderHeapRecord maps;
        if (!oloMaterialShaderHeapRecord(uvec4(0u, 0u, materialIndex, material.Generation),
                                          OLO_HYBRID_RT_HEAP_ADDRESS_AND_COUNT, maps) ||
            (maps.Flags & OLO_GPU_SCENE_MATERIAL_ALBEDO_MAP) == 0u)
            return 0.0; // The CPU selects raster fallback for unresolved maps.
        alpha *= oloHeapSampleLod(maps.Textures.x, OLO_HYBRID_RT_SAMPLER, uv, 0.0).a;
    }
    return alpha;
}
#define OLO_RT_SAMPLE_ALPHA(materialIndex, uv) oloHybridRayTracingAlpha(materialIndex, uv)
#include "RayTracingAlphaTest.glsl"

bool oloHybridRayTracingCandidate(uint slot, uint primitiveIndex, vec2 barycentrics)
{
    if (slot >= OLO_HYBRID_RT_SLOT_COUNTS.x)
        return false;
    GPUSceneInstance instance = g_GPUSceneInstances[slot];
    if ((instance.Flags & OLO_GPU_SCENE_INSTANCE_ACTIVE) == 0u ||
        instance.GeometryIndex >= OLO_HYBRID_RT_SLOT_COUNTS.y ||
        instance.MaterialIndex >= OLO_HYBRID_RT_SLOT_COUNTS.z)
        return false;
    GPUSceneGeometry geometry = g_GPUSceneGeometries[instance.GeometryIndex];
    GPUSceneMaterial material = g_GPUSceneMaterials[instance.MaterialIndex];
    if ((geometry.Flags & OLO_GPU_SCENE_GEOMETRY_ACTIVE) == 0u ||
        (material.Flags & OLO_GPU_SCENE_MATERIAL_ACTIVE) == 0u ||
        geometry.Generation != instance.GeometryGeneration || material.Generation != instance.MaterialGeneration ||
        primitiveIndex >= geometry.IndexCount / 3u)
        return false;
    return oloRayTracingConfirmCandidate(geometry, material, instance.MaterialIndex, primitiveIndex, barycentrics);
}

// A MACRO, NOT A FUNCTION. rayQueryEXT is an opaque type: GLSL refuses it as
// an out/inout parameter, so the traversal loop has to be expanded at the
// call site against the caller's own local query object.
#define oloHybridRayTracingProceed(query)                                                                   \
    do                                                                                                      \
    {                                                                                                       \
        while (rayQueryProceedEXT(query))                                                                    \
        {                                                                                                   \
            if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionTriangleEXT && \
                oloHybridRayTracingCandidate(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false)), \
                                             uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false)),   \
                                             rayQueryGetIntersectionBarycentricsEXT(query, false)))          \
                rayQueryConfirmIntersectionEXT(query);                                                      \
        }                                                                                                   \
    } while (false)

#endif
