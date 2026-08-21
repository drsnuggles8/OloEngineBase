#ifndef VIRTUAL_GBUFFER_VERTEX_STAGE_GLSL
#define VIRTUAL_GBUFFER_VERTEX_STAGE_GLSL

// =============================================================================
// VirtualGBufferVertexStage.glsl — the ONE spelling of the hardware-raster
// geometry-stage inputs and per-vertex transform math (issue #813), consumed by
// BOTH:
//   * VirtualMeshGBuffer.glsl     — the MDI vertex stage
//   * VirtualMeshletGBuffer.glsl  — the VK_EXT_mesh_shader mesh stage
// so the two pipelines cannot drift in position/velocity math (the fragment
// twin of this arrangement is VirtualGBufferFragment.glsl). The mesh-vs-MDI
// image parity contract (VulkanPassSuite.VirtualGeometryMeshTasksMatchTheMdiPath,
// zero differing pixels) rests on this file being the only spelling.
//
// CameraMatrices here is the 5-member PREFIX of the full CameraUBO (std140
// permits a prefix declaration). include/CameraCommon.glsl deliberately CANNOT
// be used instead: it also declares u_PrevViewProjection, which would collide
// with MotionBlurMatrices' member of the same name — and these paths read the
// MotionBlurMatrices flavour for velocity, the same pairing PBR_GBuffer.glsl
// uses.
// =============================================================================

#include "VirtualGeometryGpuStructs.glsl"

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(std140, binding = 8) uniform MotionBlurMatrices {
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

layout(std430, binding = 39) readonly buffer VirtualVertices { VirtualGpuVertex vertices[]; };
layout(std430, binding = 35) readonly buffer VirtualInstances { VirtualInstance instances[]; };

// Per-draw info (binding 49 = UBO_VIRTUAL_DRAW), uploaded identically for both
// hardware raster routes:
//   .x  instance every draw/workgroup of this call belongs to
//   .y  the instance's global visible-slot base for the current phase region
//       (what the cull wrote into each MDI command's BaseInstance)
//   .z  the instance's VirtualDrawArgs slot for the current phase (read by the
//       TASK stage as its launch count; the MDI stages never read it)
//   .w  the instance's ClusterCount — the task stage's launch clamp, mirroring
//       the MDI arm's maxDrawCount bound against a corrupt GPU count
layout(std140, binding = 49) uniform VirtualDrawInfo {
    uint u_VirtualInstanceIndex;
    uint u_VirtualCommandBase;
    uint u_VirtualArgsSlot;
    uint u_VirtualMaxClusters;
};

struct VirtualVertexOutputs {
    vec3 WorldPos;
    vec3 Normal;
    vec2 TexCoord;
    vec4 ClipPosCurr;
    vec4 ClipPosPrev;
};

VirtualVertexOutputs TransformVirtualVertex(VirtualInstance inst, VirtualGpuVertex vert)
{
    VirtualVertexOutputs o;
    vec3 localPosition = vert.PositionU.xyz;
    vec3 localNormal = vert.NormalV.xyz;

    o.WorldPos = vec3(inst.Transform * vec4(localPosition, 1.0));
    o.Normal = mat3(inst.NormalMatrix) * localNormal;
    o.TexCoord = vec2(vert.PositionU.w, vert.NormalV.w);

    o.ClipPosCurr = u_ViewProjection * vec4(o.WorldPos, 1.0);
    vec4 prevWorldPos = inst.PrevTransform * vec4(localPosition, 1.0);
    o.ClipPosPrev = u_PrevViewProjection * prevWorldPos;
    return o;
}

#endif // VIRTUAL_GBUFFER_VERTEX_STAGE_GLSL
