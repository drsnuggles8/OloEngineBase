#ifndef INSTANCE_BLOCK_GLSL
#define INSTANCE_BLOCK_GLSL

// Per-draw instance data SSBO. Layout mirrors OloEngine::InstanceData
// (OloEngine/Renderer/Instancing/InstanceData.h, 256 B std430). Indexed by
// gl_InstanceIndex — for non-instanced draws gl_InstanceIndex is 0 and the
// C++ side uploads a length-1 InstanceBuffer.
//
// Fragment stages read their draw's entry as instances[v_InstanceIndex]
// (.Transform, .Normal, .PrevTransform, .EntityID, .Color, ...).
struct InstanceData {
    mat4 Transform;
    mat4 Normal;
    mat4 PrevTransform;
    vec4 Color;
    int  EntityID;
    float Custom;
    uvec2 StableID;
    // Lightmap atlas region (issue #439): uv2 * xy + zw addresses this
    // instance's charts in the scene lightmap atlas. xy == 0 means "no
    // lightmap" — the ambient ladder falls through to probes/IBL.
    vec4 LightmapScaleOffset;
    // Canonical GPU Scene reference (issue #994): x = instance slot, y =
    // instance generation, z = material slot, w = material generation. A zero
    // generation means "this draw is not linked to a GPU Scene record" — the
    // shader then reads the per-draw UBO exactly as it did before. Validate
    // with oloGPUSceneMaterialLink() / oloGPUSceneInstanceLink() rather than
    // by hand: an index without its generation is a stale-slot read.
    uvec4 GPUSceneRef;
};

layout(std430, binding = 15) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

// Flat-int varying routed from the vertex stage (InstanceBlock_Vertex.glsl
// declares the matching `out`). Carries the gl_InstanceIndex that produced
// this fragment so per-instance EntityID / Color / Custom resolve correctly
// even after CommandBucket auto-batching collapses N draws into one
// glDrawElementsInstanced call. For tess_eval / geometry stages that include
// this file as a fall-through (terrain), `v_InstanceIndex` is unused —
// terrain is single-instance so instances[v_InstanceIndex] == instances[0].
layout(location = 14) flat in int v_InstanceIndex;

#endif // INSTANCE_BLOCK_GLSL
