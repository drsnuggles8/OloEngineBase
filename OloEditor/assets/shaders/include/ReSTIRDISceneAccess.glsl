// The scene access every ReSTIR DI draw needs, in the ONE order that compiles:
// the GPU Scene tables, the parameter block, the alpha-MASK test the visibility
// ray needs, the shared light sampling, and the shared estimator. Issue #1140.
//
// WHY IT IS A FILE AND NOT FOUR COPIES. The order is load-bearing and not
// obvious: RayTracingAlphaTest.glsl needs OLO_RT_SAMPLE_ALPHA defined, which
// needs the material-texture table, which lives in the parameter block, which
// must therefore precede it; LightSampling.glsl needs the emissive-texture
// macros, which need the same heap offsets; and ReSTIRDICommon.glsl needs all of
// it. Four shaders each re-deriving that order is four chances to get it subtly
// wrong — and "subtly wrong" here means one draw's visibility ray tests alpha
// and another's does not, which is a light leak on masked geometry that only
// shows up in a scene with foliage.
//
// The caller must have declared its own attachments and included
// include/BindlessHeap.glsl and include/PBRCommon.glsl first, and must have
// emitted the ray-query / buffer-reference / descriptor-heap #extension
// directives before any other token.
#ifndef OLO_RESTIR_DI_SCENE_ACCESS_GLSL
#define OLO_RESTIR_DI_SCENE_ACCESS_GLSL

#include "GPUScene.glsl"
#include "GPUSceneInstances.glsl"
#include "GPUSceneGeometries.glsl"
#include "GPUSceneMaterials.glsl"
#include "GPUSceneLights.glsl"
#include "PathTracerSampler.glsl"
#include "DescriptorHeapTextures.glsl"
#include "Reservoir.glsl"
#include "ReSTIRDIParams.glsl"

// ---------------------------------------------------------------------------
// Material textures and the alpha-MASK test
// ---------------------------------------------------------------------------

// The heap offsets of one material's maps — MaterialTextureTable's record, the
// same one GpuPathTracer.glsl reads. Only the albedo alpha matters to a
// visibility ray, but the record is uploaded whole.
struct OloReSTIRMaterialTextures
{
    uint Albedo;
    uint MetallicRoughness;
    uint Normal;
    uint Emissive;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OloReSTIRMaterialTextureTable
{
    OloReSTIRMaterialTextures Records[];
};

bool OloReSTIRTexturesEnabled()
{
    return (u_EmissiveTable.w & OLO_RESTIR_FLAG_TEXTURES) != 0u &&
           (u_MaterialTable.x | u_MaterialTable.y) != 0u && u_MaterialTable.w != OLO_HEAP_OFFSET_INVALID;
}

// glTF MASK alpha: baseColorFactor.a times the albedo map's alpha — the raster
// path's own definition (PBR_GBuffer.glsl), and the path tracer's.
float OloReSTIRSampleAlpha(uint materialIndex, vec2 uv)
{
    const GPUSceneMaterial material = g_GPUSceneMaterials[materialIndex];
    float alpha = material.BaseColorFactor.a;
    if (!OloReSTIRTexturesEnabled() || materialIndex >= u_MaterialTable.z)
        return alpha;
    const OloReSTIRMaterialTextures maps = OloReSTIRMaterialTextureTable(u_MaterialTable.xy).Records[materialIndex];
    if (maps.Albedo != OLO_HEAP_OFFSET_INVALID)
        alpha *= oloHeapSampleLod(maps.Albedo, u_MaterialTable.w, uv, 0.0).a;
    return alpha;
}
#define OLO_RT_SAMPLE_ALPHA(materialIndex, uv) OloReSTIRSampleAlpha(materialIndex, uv)
#include "RayTracingAlphaTest.glsl"

// A candidate (non-opaque) intersection: SOLID when the GPU Scene cannot
// describe it — geometry the TLAS still holds but the tables no longer do —
// because reporting it as a miss would light a surface straight through an
// occluder. The same unshadeable-is-opaque rule PtCandidateIsSolid applies, and
// for the same reason: the frame it happens in is one the GPU Scene commit also
// reported dirty, so the invalidation discards these pixels anyway.
bool OloReSTIRCandidateIsSolid(uint instanceSlot, uint primitiveIndex, vec2 barycentrics)
{
    if (instanceSlot >= u_SlotCounts.x)
        return true;
    const GPUSceneInstance instance = g_GPUSceneInstances[instanceSlot];
    if ((instance.Flags & OLO_GPU_SCENE_INSTANCE_ACTIVE) == 0u)
        return true;
    if (instance.MaterialIndex >= u_SlotCounts.z || instance.GeometryIndex >= u_SlotCounts.y)
        return true;
    const GPUSceneGeometry geometry = g_GPUSceneGeometries[instance.GeometryIndex];
    const GPUSceneMaterial material = g_GPUSceneMaterials[instance.MaterialIndex];
    if ((geometry.Flags & OLO_GPU_SCENE_GEOMETRY_ACTIVE) == 0u ||
        (material.Flags & OLO_GPU_SCENE_MATERIAL_ACTIVE) == 0u)
        return true;
    return oloRayTracingConfirmCandidate(geometry, material, instance.MaterialIndex, primitiveIndex, barycentrics);
}
#define OLO_RESTIR_CANDIDATE_IS_SOLID(instanceSlot, primitive, barycentrics)                                        \
    OloReSTIRCandidateIsSolid(instanceSlot, primitive, barycentrics)

// The emissive sampler sees the emitter's map through the same heap, so the
// NEE-side radiance and an emitter hit agree — the oracle's arrangement.
#define OLO_LIGHT_SAMPLE_EMISSIVE_TEXTURE(byteOffset, uv) oloHeapSampleLod(byteOffset, u_MaterialTable.w, uv, 0.0)
#define OLO_LIGHT_EMISSIVE_TEXTURES_ENABLED OloReSTIRTexturesEnabled()
#include "LightSampling.glsl"
#include "ReSTIRDICommon.glsl"

#endif // OLO_RESTIR_DI_SCENE_ACCESS_GLSL
