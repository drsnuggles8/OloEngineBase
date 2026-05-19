#ifndef INSTANCE_BLOCK_VERTEX_GLSL
#define INSTANCE_BLOCK_VERTEX_GLSL

// Vertex-stage variant of InstanceBlock.glsl — uses `gl_InstanceIndex` to read
// per-instance data, which is what GPU instancing actually needs. Include this
// in any vertex / tessellation / geometry stage that should support
// CommandBucket auto-batching or InstancedMeshComponent draws. Fragment stages
// should continue to include the regular InstanceBlock.glsl (which uses
// instances[0]) until per-instance fragment data is routed through a flat
// varying.
//
// Layout matches OloEngine::InstanceData (Renderer/Instancing/InstanceData.h,
// 224 B std430). For a single-instance / non-instanced draw the C++ side
// uploads a length-1 buffer; gl_InstanceIndex is 0 and the macros resolve to
// instances[0] just like the fragment-stage include.
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

// Flat-int varying that routes gl_InstanceIndex from the vertex stage to the
// fragment stage so per-instance fields (EntityID, Color, Custom) remain
// addressable after CommandBucket auto-batching collapses N draws into one
// glDrawElementsInstanced. The vertex `void main()` must call
// `OLO_INSTANCE_FORWARD();` for the varying to be written.
layout(location = 14) flat out int v_InstanceIndex;

#define u_Model        (instances[gl_InstanceIndex].Transform)
#define u_Normal       (instances[gl_InstanceIndex].Normal)
#define u_PrevModel    (instances[gl_InstanceIndex].PrevTransform)
#define u_EntityID     (instances[gl_InstanceIndex].EntityID)

// Decal_*.glsl alias (same layout offset; legacy field name).
#define u_NormalMatrix (instances[gl_InstanceIndex].Normal)

// Forward the current gl_InstanceIndex to the fragment stage. Must be called
// at the top of every vertex shader's main() that includes this file.
#define OLO_INSTANCE_FORWARD() v_InstanceIndex = gl_InstanceIndex

#endif // INSTANCE_BLOCK_VERTEX_GLSL
