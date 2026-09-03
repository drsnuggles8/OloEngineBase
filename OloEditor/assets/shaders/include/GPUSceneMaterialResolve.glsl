#ifndef OLO_GPU_SCENE_MATERIAL_RESOLVE_GLSL
#define OLO_GPU_SCENE_MATERIAL_RESOLVE_GLSL

// Resolving a draw's canonical material record (issue #994).
//
// A migrated raster fragment stage reads its material from the GPU Scene
// material table, addressed by the link the draw carries in
// InstanceData::GPUSceneRef. Include this AFTER InstanceBlock.glsl (the link
// lives in that struct); it pulls the material table in on binding 17.
//
// The generation is the whole point. A slot alone is a stale-slot read: the
// registry reuses a retired slot after two frames, so an index that was right
// when the draw was recorded can name a different material by the time the
// draw executes. GPUSceneTypes.h makes generation zero permanently invalid,
// which is why "no link" and "dead link" collapse into one test here and why
// an unlinked draw can never be mistaken for a linked one.
//
// A false return is not an error and must never drop the draw: the caller
// falls back to the per-draw material UBO, which is the same authored data
// reached the legacy way. Unsupported or unlinked geometry keeps rendering.
#include "GPUSceneMaterials.glsl"

bool oloGPUSceneMaterial(uvec4 gpuSceneRef, out GPUSceneMaterial material)
{
    // `material` is written ONLY on the true path, so a caller must not read it
    // when this returns false — and must not reach for it with a TERNARY
    // either: SPIR-V's OpSelect evaluates both operands. Guard the read with a
    // real `if`, as PBR_GBuffer.glsl does. Writing a "default" here instead
    // would mean indexing the table before the generation check.
    //
    // There is deliberately NO length() bounds check. `.length()` compiles to
    // OpArrayLength, and the Vulkan RHI maps this storage buffer through
    // VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT, where a buffer's
    // length is not knowable — vkCreateGraphicsPipelines rejects the shader
    // outright, and the editor then crashes on the null pipeline rather than
    // merely rendering wrong. The generation IS the bound: a non-zero
    // generation only ever comes from a link the registry resolved this frame,
    // and Upload grows the buffer to hold every committed record before any
    // draw runs, so the slot is inside the table by construction.
    //
    // w == 0 is the invalid generation: an unlinked draw, or one whose record
    // was retired between extraction and dispatch.
    if (gpuSceneRef.w == 0u)
        return false;

    GPUSceneMaterial record = g_GPUSceneMaterials[gpuSceneRef.z];
    if (record.Generation != gpuSceneRef.w)
        return false;
    if ((record.Flags & OLO_GPU_SCENE_MATERIAL_ACTIVE) == 0u)
        return false;

    material = record;
    return true;
}

#endif // OLO_GPU_SCENE_MATERIAL_RESOLVE_GLSL
