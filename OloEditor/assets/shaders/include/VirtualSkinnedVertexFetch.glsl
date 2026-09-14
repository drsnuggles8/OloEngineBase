#ifndef VIRTUAL_SKINNED_VERTEX_FETCH_GLSL
#define VIRTUAL_SKINNED_VERTEX_FETCH_GLSL

// =============================================================================
// VirtualSkinnedVertexFetch.glsl — the ONE spelling of "pose a virtual-geometry
// vertex" (issue #1150), shared by every stage that turns a cooked rest-pose
// vertex into the position it is drawn at:
//   * VirtualGBufferVertexStage.glsl  — the MDI and mesh-shader G-Buffer arms
//   * VirtualMeshShadowDepth.glsl     — the CSM / atlas shadow cascades
//   * VSM_VirtualMeshDepth.glsl       — the virtual shadow map clip levels
//
// They must agree EXACTLY or the depth a shadow was rasterized at stops
// matching the surface it was rasterized for, and a character self-shadows in
// stripes. The depth-prepass contract has the same requirement on the classic
// path, which is why DepthPrepass_Skinned.glsl replicates PBR_GBuffer_Skinned's
// position math verbatim; here the math is shared instead of replicated.
//
// UNLIKE VirtualSkinning.glsl this file DOES declare bindings — 39 (the vertex
// arena, which carries the packed skin tail) and 35 (the instance buffer, whose
// tail carries the bone palettes). Every consumer needs both anyway, so
// declaring them once here is what lets the fetch live in one place; a stage
// that includes this must not declare them again.
// =============================================================================

#include "VirtualGeometryGpuStructs.glsl"
#include "VirtualSkinning.glsl"
#include "VirtualDrawInfo.glsl"

layout(std430, binding = 39) readonly buffer VirtualVertices { VirtualGpuVertex vertices[]; };
layout(std430, binding = 35) readonly buffer VirtualInstances { VirtualInstance instances[]; };

struct VirtualSkinnedVertex {
    vec3 Position;
    vec3 Normal;
    vec3 PrevPosition;
};

// The instance's bone palette applied to one rest-pose vertex, in OBJECT space
// — before the instance transform, exactly as the classic path does it
// (PBR_GBuffer_Skinned.glsl). The palette is a model-space pose and the
// instance transform places the posed model in the world; doing it the other
// way round would apply the entity's scale to the bone translations.
//
// A palette entry IS a VirtualInstance record, reusing its three matrices with
// their own meanings — Transform is the bone's current skinning matrix,
// PrevTransform last frame's, NormalMatrix what its normals transform by. See
// VirtualInstanceGpuRecord::SkinBoneBase for why that is the shape it is.
//
// Returns the vertex UNCHANGED for a rigid instance, an arena with no skin tail,
// or a binding with no usable influence — that last case is a rigid vertex
// inside a skinned mesh, and returning it unchanged is what stops it collapsing
// to the model origin.
//
// `globalVertexIndex` is the SAME index the vertex fetch used (gl_VertexIndex on
// the MDI arm, cluster.VertexBase + local on the mesh arm): it addresses the
// skin tail, so a caller holding a VirtualGpuVertex but not its index cannot
// pose it.
VirtualSkinnedVertex SkinVirtualVertex(VirtualInstance inst, uint globalVertexIndex, vec3 restPosition,
                                       vec3 restNormal)
{
    VirtualSkinnedVertex o;
    o.Position = restPosition;
    o.Normal = restNormal;
    o.PrevPosition = restPosition;
    if ((inst.Flags & 8u) == 0u || u_VirtualSkinningBase == 0u || inst.SkinBoneCount == 0u)
    {
        return o;
    }

    uint element = oloVirtualSkinningElement(u_VirtualSkinningBase, globalVertexIndex);
    OloSkinBinding binding = oloUnpackSkinBinding(vertices[element].PositionU, vertices[element].NormalV,
                                                 globalVertexIndex);

    vec3 position = vec3(0.0);
    vec3 normal = vec3(0.0);
    vec3 prevPosition = vec3(0.0);
    float applied = 0.0;
    // Constant trip count over SSBO loads — deliberately NOT a data-dependent
    // loop over a dynamically indexed LOCAL array, which is the shape that took
    // Mesa's AMD compiler 200 s and 14 GB in
    // docs/agent-rules/amd-mesa-shader-compile-blowup.md.
    for (int i = 0; i < 4; ++i)
    {
        float weight = binding.Weights[i];
        uint boneId = binding.BoneIDs[i];
        if (weight <= 0.0 || boneId >= inst.SkinBoneCount)
        {
            continue;
        }
        // Member access rather than a struct copy: a VirtualInstance is 256
        // bytes and a copy of one per influence is real register pressure in a
        // stage that runs per vertex.
        uint slot = inst.SkinBoneBase + boneId;
        position += weight * vec3(instances[slot].Transform * vec4(restPosition, 1.0));
        prevPosition += weight * vec3(instances[slot].PrevTransform * vec4(restPosition, 1.0));
        normal += weight * (mat3(instances[slot].NormalMatrix) * restNormal);
        applied += weight;
    }
    if (applied <= 1e-3)
    {
        return o;
    }

    // Renormalized by the weight that ACTUALLY landed, not by the binding's
    // nominal sum. The two differ when a slot names a bone past SkinBoneCount,
    // and dividing by the nominal sum there would pull the vertex toward the
    // model origin instead of leaving it where its remaining bones put it. It
    // also absorbs the unorm16 quantization of the weights exactly.
    o.Position = position / applied;
    o.PrevPosition = prevPosition / applied;
    o.Normal = normal / applied;
    return o;
}

// Position only, for the depth-only stages. Named rather than open-coded at
// each call so "the shadow uses the same pose as the G-Buffer" is a fact about
// this file instead of a thing to check in three places.
vec3 SkinVirtualPosition(VirtualInstance inst, uint globalVertexIndex, vec3 restPosition)
{
    return SkinVirtualVertex(inst, globalVertexIndex, restPosition, vec3(0.0, 1.0, 0.0)).Position;
}

#endif // VIRTUAL_SKINNED_VERTEX_FETCH_GLSL
