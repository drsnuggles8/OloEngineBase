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

// Struct mirrors, the skin-binding layout, the vertex/instance SSBOs (39/35)
// and the shared pose function all arrive through this one include, which
// the SHADOW depth stages include too — that is what makes "the shadow
// rasterizes the pose the G-Buffer drew" structural rather than a thing to
// keep checking.
#include "VirtualSkinnedVertexFetch.glsl"

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

// `globalVertexIndex` is the same index the vertex fetch used — it addresses
// the skin tail, so a caller that has a VirtualGpuVertex but not its index
// cannot skin it. Both raster routes already have it (gl_VertexIndex on the MDI
// arm, cluster.VertexBase + local on the mesh arm).
VirtualVertexOutputs TransformVirtualVertex(VirtualInstance inst, VirtualGpuVertex vert, uint globalVertexIndex)
{
    VirtualVertexOutputs o;
    // Skinning happens in OBJECT space, before the instance transform, exactly
    // as it does on the classic path (PBR_GBuffer_Skinned.glsl) — the palette
    // is a model-space pose and the instance transform places the posed model
    // in the world. Doing it the other way round would apply the entity's
    // scale to the bone translations.
    VirtualSkinnedVertex skinned = SkinVirtualVertex(inst, globalVertexIndex, vert.PositionU.xyz, vert.NormalV.xyz);

    o.WorldPos = vec3(inst.Transform * vec4(skinned.Position, 1.0));
    o.Normal = mat3(inst.NormalMatrix) * skinned.Normal;
    o.TexCoord = vec2(vert.PositionU.w, vert.NormalV.w);

    o.ClipPosCurr = u_ViewProjection * vec4(o.WorldPos, 1.0);
    // Per-bone previous pose as well as per-entity: a stationary character
    // playing an animation has motion the entity transform knows nothing about,
    // and TAA / motion blur read this to resolve it.
    vec4 prevWorldPos = inst.PrevTransform * vec4(skinned.PrevPosition, 1.0);
    o.ClipPosPrev = u_PrevViewProjection * prevWorldPos;
    return o;
}

#endif // VIRTUAL_GBUFFER_VERTEX_STAGE_GLSL
