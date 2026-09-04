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
// hardware raster routes. One shared spelling — see the include for the field
// contract and for why it is not declared here.
#include "VirtualDrawInfo.glsl"

struct VirtualVertexOutputs {
    vec3 WorldPos;
    vec3 Normal;
    vec2 TexCoord;
    vec4 ClipPosCurr;
    vec4 ClipPosPrev;
};

// The instance's baked lightmap uv2 for one vertex, or (0,0) (issue #867).
//
// TWO guards, and both are load-bearing rather than optimisations:
//  * `u_VirtualLightmapUVBase != 0u` — this arena has no uv2 tail at all, so
//    the element index would land past the buffer. On Vulkan that is a
//    buffer-device-address read with no bounds, i.e. device loss, which is the
//    exact incident ADR 0011 amendment (89) records for the classic path.
//  * `LightmapScaleOffset.x > 0.0` — this INSTANCE has no baked region. Its
//    mesh may well be a cook that predates its unwrap, so the tail holds some
//    other mesh's charts at these indices; the C++ side refuses to publish a
//    region in that case, and this is the shader half of the same contract.
//
// Kept next to TransformVirtualVertex because both raster pipelines must agree
// on it — the mesh-vs-MDI zero-differing-pixels parity test rests on this file
// being the only spelling.
vec2 FetchVirtualLightmapUV(VirtualInstance inst, uint globalVertexIndex)
{
    if (u_VirtualLightmapUVBase == 0u || inst.LightmapScaleOffset.x <= 0.0)
    {
        return vec2(0.0);
    }
    uint element = oloVirtualLightmapUVElement(globalVertexIndex);
    return oloVirtualLightmapUVLane(vertices[element].PositionU, vertices[element].NormalV,
                                    globalVertexIndex);
}

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
