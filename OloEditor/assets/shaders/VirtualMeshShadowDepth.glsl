// =============================================================================
// VirtualMeshShadowDepth.glsl — depth-only shadow-caster path for virtualized
// geometry (issue #629). Same SSBO vertex pulling as VirtualMeshGBuffer.glsl,
// but the CameraMatrices UBO (binding 0) carries the SHADOW camera the shadow
// pass uploads per cascade, and the fragment stage is empty (depth-only;
// exempt from the fragment-output contract via the *Depth* name).
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

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection; // shadow cascade light view-projection (render-origin-relative)
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Per-draw info (binding 49 = UBO_VIRTUAL_DRAW). This stage reads ONLY
// u_VirtualInstanceIndex; it never reads the viewport dimensions, and
// VirtualGeometryShadow.cpp uploads zero for every field it does not use.
//
// That "unused here" fact used to be expressed by naming the two slots
// `_vdPad0`/`_vdPad1` locally (#856), which kept the disagreement with
// VirtualVisibilityResolve.glsl — where the same bytes are real, populated
// viewport fields — from tripping the cross-shader layout test. #813 removed
// the disagreement itself instead: there is now one block, declared once in
// the include, so a stage that ignores a field simply ignores it and no local
// renaming is needed to keep the mirrors apart.
#include "include/VirtualDrawInfo.glsl"

void main()
{
    VirtualInstance inst = instances[u_VirtualInstanceIndex];
    VirtualGpuVertex vert = vertices[gl_VertexIndex];
    // The SAME pose the G-Buffer draws (issue #1150): both stages call
    // SkinVirtualVertex out of include/VirtualSkinnedVertexFetch.glsl. A
    // shadow rasterized from the rest pose while the surface is animated is a
    // character shadowing itself in stripes, and nothing about it reads as a
    // shader error.
    vec3 posed = SkinVirtualPosition(inst, uint(gl_VertexIndex), vert.PositionU.xyz);
    gl_Position = u_ViewProjection * (inst.Transform * vec4(posed, 1.0));
}

#type fragment
#version 460 core

void main()
{
    // Depth-only
}
