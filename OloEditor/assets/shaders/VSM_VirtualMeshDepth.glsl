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

// STRUCT MIRRORS + THE SHARED POSE, in include/VirtualSkinnedVertexFetch.glsl
// (which pulls in VirtualGeometryGpuStructs.glsl and declares bindings 39/35).
//
// This file carried its own copies until issue #1150, which grew
// VirtualInstance from 240 to 256 bytes. FIVE hand-written copies had to move
// together, and a std430 stride mismatch does not error: every instance past
// the first reads the previous one's transform. The copies were already
// recorded as follow-up work by the shared header; a change that has to touch
// all of them is when that debt comes due.
#include "include/VirtualSkinnedVertexFetch.glsl"

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
    // The SAME pose the G-Buffer draws (issue #1150), through the one
    // SkinVirtualVertex in include/VirtualSkinnedVertexFetch.glsl. A clip level
    // rasterized from the rest pose while the surface is animated bakes a
    // rest-pose silhouette into a CACHED page, so it survives until something
    // invalidates the page — far longer than a wrong frame.
    vec3 posed = SkinVirtualPosition(inst, uint(gl_VertexIndex), vert.PositionU.xyz);
    gl_Position = u_VSMClips[clipLevel].ViewProjectionRaster *
                  (inst.Transform * vec4(posed, 1.0));
}

#type fragment
#version 460 core

#include "include/VirtualShadowRasterStage.glsl"
