#ifndef OLO_MATERIAL_SHADER_HEAP_TABLE_GLSL
#define OLO_MATERIAL_SHADER_HEAP_TABLE_GLSL

// std430 twin of MaterialShaderHeapRecord (32 bytes). Included only on Vulkan.
struct OloMaterialShaderHeapRecord
{
    uvec4 Textures;
    uint Emissive;
    uint Generation;
    uint Flags;
    uint _padding0;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OloMaterialShaderHeapTable
{
    OloMaterialShaderHeapRecord Records[];
};

bool oloMaterialShaderHeapRecord(uvec4 reference, uvec3 addressAndCount, out OloMaterialShaderHeapRecord record)
{
    if (reference.w == 0u || reference.z >= addressAndCount.z || all(equal(addressAndCount.xy, uvec2(0u))))
        return false;
    OloMaterialShaderHeapTable table = OloMaterialShaderHeapTable(addressAndCount.xy);
    OloMaterialShaderHeapRecord candidate = table.Records[reference.z];
    if (candidate.Generation != reference.w || (candidate.Flags & OLO_GPU_SCENE_MATERIAL_ACTIVE) == 0u)
        return false;
    record = candidate;
    return true;
}

#endif
