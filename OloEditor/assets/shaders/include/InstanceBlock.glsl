#ifndef INSTANCE_BLOCK_GLSL
#define INSTANCE_BLOCK_GLSL

// Per-draw instance data SSBO. Layout mirrors OloEngine::InstanceData
// (OloEngine/Renderer/Instancing/InstanceData.h, 224 B std430). Indexed by
// gl_InstanceIndex — for non-instanced draws gl_InstanceIndex is 0 and the
// C++ side uploads a length-1 InstanceBuffer.
//
// Include this file in any vertex / tessellation / geometry stage that used
// to read the legacy `ModelMatrices` UBO at binding = 3. The `#define`s at
// the bottom keep shader bodies unchanged: u_Model still resolves to the
// world transform, just sourced from the SSBO instead of a UBO. New shaders
// can use the fully-qualified names (instances[gl_InstanceIndex].Color etc.)
// to access fields the legacy UBO didn't carry (Color, Custom).
struct InstanceData {
    mat4 Transform;
    mat4 Normal;
    mat4 PrevTransform;
    vec4 Color;
    int  EntityID;
    float Custom;
    int  _instancePad0;
    int  _instancePad1;
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

#define u_Model     (instances[v_InstanceIndex].Transform)
#define u_Normal    (instances[v_InstanceIndex].Normal)
#define u_PrevModel (instances[v_InstanceIndex].PrevTransform)
#define u_EntityID  (instances[v_InstanceIndex].EntityID)

// Decal_*.glsl historically named the normal matrix `u_NormalMatrix`. Same
// std430 offset as `u_Normal`; keeping the alias avoids touching those
// shader bodies during the binding-3 → SSBO migration.
#define u_NormalMatrix (instances[v_InstanceIndex].Normal)

#endif // INSTANCE_BLOCK_GLSL
