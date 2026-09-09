// =============================================================================
// VSM_VirtualMeshDepth.glsl — virtualized-geometry casters into the Virtual
// Shadow Map (issue #1149).
//
// The two halves of this file come from two different places, and that is the
// whole point of it:
//   * the VERTEX stage is VirtualMeshShadowDepth.glsl's SSBO vertex pull — the
//     cluster pipeline's compacted indirect draws address vertices through the
//     pooled arena, not through a VAO, so the VSM's own VSM_Depth.glsl vertex
//     stage cannot draw them;
//   * the FRAGMENT stage is the VSM's, unchanged — the page-table indirection
//     plus imageAtomicMin that every VSM caster resolves visibility through
//     (include/VirtualShadowRasterStage.glsl).
//
// WHY THE CLIP LEVEL IS A UNIFORM HERE and a per-instance record in VSM_Depth:
// the mesh path culls (caster x clip level) pairs and compacts them, so one
// indirect draw covers all sixteen levels with the level riding each record.
// Virtual geometry has no such record — its indirect command stream is written
// by the cluster cull, one command per surviving CLUSTER, and those commands
// carry cluster geometry, not a level. So the level travels per DRAW: the route
// runs one cull + one replay per clip level (the current per-view cull; #1143's
// multi-view dispatch is what would collapse it), and every draw of that replay
// belongs to exactly one level. u_VSMPassParams.x is that level.
//
// The depth-only name is what exempts this from the fragment-output contract.
// =============================================================================

#type vertex
#version 460 core

// Mirrors OloEngine::VirtualGpuVertex (32 B std430)
struct VirtualGpuVertex {
    vec4 PositionU;
    vec4 NormalV;
};

// Mirrors OloEngine::VirtualInstanceGpuRecord (240 B std430)
struct VirtualInstance {
    mat4 Transform;
    mat4 PrevTransform;
    mat4 NormalMatrix;
    uint ClusterBase;
    uint ClusterCount;
    uint GroupBase;
    int  EntityID;
    float MaxScale;
    float ErrorThresholdPixels;
    uint CommandBase;
    uint Flags;
    // Declared even though this stage ignores it: the std430 array stride IS the
    // struct size, so omitting it makes every instance after the first read the
    // previous one's transform.
    vec4 LightmapScaleOffset;
};

layout(std430, binding = 39) readonly buffer VirtualVertices { VirtualGpuVertex vertices[]; };
layout(std430, binding = 35) readonly buffer VirtualInstances { VirtualInstance instances[]; };

// u_VSMClips (the clip projections) and u_VSMPassParams (the level).
#include "include/VirtualShadowResources.glsl"

// Per-draw instance index (binding 49 = UBO_VIRTUAL_DRAW). This stage reads ONLY
// u_VirtualInstanceIndex; every other field is uploaded as zero.
#include "include/VirtualDrawInfo.glsl"

layout(location = 0) flat out uint v_VSMClipLevel;

void main()
{
    VirtualInstance inst = instances[u_VirtualInstanceIndex];
    VirtualGpuVertex vert = vertices[gl_VertexIndex];

    uint clipLevel = u_VSMPassParams.x;
    v_VSMClipLevel = clipLevel;
    // The RASTERIZER flavour — this is a gl_Position, so it must carry Vulkan's
    // y flip and z remap. The cull, which projects and then interprets the
    // result itself, reads the raw matrix instead (ADR 0011 (59)).
    gl_Position = u_VSMClips[clipLevel].ViewProjectionRaster *
                  (inst.Transform * vec4(vert.PositionU.xyz, 1.0));
}

#type fragment
#version 460 core

#include "include/VirtualShadowRasterStage.glsl"
