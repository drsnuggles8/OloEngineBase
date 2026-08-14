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

#ifdef OLO_INSTANCE_SINGLE
// Single-entry contract: the draw's per-instance data lives in its OWN
// vertex stream (foliage's 48-byte instance VB) and the C++ side uploads
// exactly ONE InstanceData entry — a shared model matrix for all N
// instances. Indexing by gl_InstanceIndex here reads 224*gl_InstanceIndex
// bytes into a 224-byte upload: GL clamps that OOB read to garbage/zero
// (foliage silently collapsed), a Vulkan buffer-device-address pointer
// page-faults the whole device once the read crosses an unmapped page
// (the #691 Phase 8 foliage VK_ERROR_DEVICE_LOST — fault address 32 MB
// past a 12 MB buffer at 254k instances). Define OLO_INSTANCE_SINGLE
// before this include for any shader whose instancing rides its own
// stream; the varying interface stays identical so fragment-stage
// InstanceBlock.glsl consumers keep working (they resolve instances[0]).
#define u_Model        (instances[0].Transform)
#define u_Normal       (instances[0].Normal)
#define u_PrevModel    (instances[0].PrevTransform)
#define u_EntityID     (instances[0].EntityID)
#define u_NormalMatrix (instances[0].Normal)
#define OLO_INSTANCE_FORWARD() v_InstanceIndex = 0
#else
#define u_Model        (instances[gl_InstanceIndex].Transform)
#define u_Normal       (instances[gl_InstanceIndex].Normal)
#define u_PrevModel    (instances[gl_InstanceIndex].PrevTransform)
#define u_EntityID     (instances[gl_InstanceIndex].EntityID)

// Decal_*.glsl alias (same layout offset; legacy field name).
#define u_NormalMatrix (instances[gl_InstanceIndex].Normal)

// Forward the current gl_InstanceIndex to the fragment stage. Must be called
// at the top of every vertex shader's main() that includes this file.
#define OLO_INSTANCE_FORWARD() v_InstanceIndex = gl_InstanceIndex
#endif // OLO_INSTANCE_SINGLE

#endif // INSTANCE_BLOCK_VERTEX_GLSL
