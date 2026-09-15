#ifndef VIRTUAL_SKINNED_VERTEX_FETCH_GLSL
#define VIRTUAL_SKINNED_VERTEX_FETCH_GLSL

// =============================================================================
// VirtualSkinnedVertexFetch.glsl — how virtualized geometry reaches the ONE
// skeletal deformation producer (issue #1150, consuming #1226).
//
// Shared by every stage that turns a cooked rest-pose virtual vertex into the
// position it is drawn at:
//   * VirtualGBufferVertexStage.glsl  — the MDI and mesh-shader G-Buffer arms
//   * VirtualMeshShadowDepth.glsl     — the CSM / atlas shadow cascades
//   * VSM_VirtualMeshDepth.glsl       — the virtual shadow map clip levels
//
// THE SKINNING ITSELF IS NOT HERE. It is in include/SkeletalDeformation.glsl
// with every other skinned consumer's, which is rule 1 of
// docs/agent-rules/skeletal-deformation-shared-output.md: a pass that reaches
// the bone palette on its own is free to drift from the pass it is
// depth-tested and shadow-matched against, and that had already happened seven
// ways before #1226 collapsed it.
//
// WHAT THIS FILE ADDS, AND WHY IT IS A HOOK RATHER THAN A COPY
//
// The producer's default palette is a per-draw UBO (binding 4): one mesh, one
// skeleton, one draw. Virtual geometry cannot use it — ONE multi-draw or mesh
// dispatch covers many instances, each with its own palette — so the palette
// rides the instance buffer's tail instead (VirtualInstanceGpuRecord's
// SkinBoneBase). The math is identical; only the STORAGE differs. So this file
// redirects the producer's palette accessors with
// OLO_DEFORM_EXTERNAL_PALETTE and calls the same OloDeformSkinnedVertex()
// everything else calls.
//
// The two globals below are what make that possible: the producer's accessor
// macros expand inside its own functions, where an instance's palette base is
// not a parameter, so the base and length are published per invocation before
// the call. They are written in exactly one place — SkinVirtualVertex — and a
// caller must not set them itself.
//
// UNLIKE VirtualSkinning.glsl this file DOES declare bindings — 39 (the vertex
// arena, which carries the packed skin tail) and 35 (the instance buffer, whose
// tail carries the bone palettes). Every consumer needs both anyway; a stage
// that includes this must not declare them again.
// =============================================================================

#include "VirtualGeometryGpuStructs.glsl"
#include "VirtualSkinning.glsl"
#include "VirtualDrawInfo.glsl"

layout(std430, binding = 39) readonly buffer VirtualVertices { VirtualGpuVertex vertices[]; };
layout(std430, binding = 35) readonly buffer VirtualInstances { VirtualInstance instances[]; };

// This invocation's palette window inside the instance buffer's tail. Set by
// SkinVirtualVertex immediately before it calls the producer; read only by the
// accessor macros below.
uint oloVgBoneBase = 0u;
uint oloVgBoneCount = 0u;

// A palette entry IS a VirtualInstance record, reusing two of its matrices with
// their own meanings — Transform is the bone's current skinning matrix,
// PrevTransform last frame's. (Its NormalMatrix lane is unused: the producer
// derives the deformed normal from the blended skin matrix, as every other
// skinned consumer does, so nothing needs a per-bone inverse-transpose.)
#define OLO_DEFORM_EXTERNAL_PALETTE
#define OLO_DEFORM_BONE(i) instances[oloVgBoneBase + uint(i)].Transform
#define OLO_DEFORM_BONE_COUNT oloVgBoneCount
// Virtual geometry emits velocity from its G-Buffer arms, and the depth arms
// dead-code the previous pose away — the producer's own note on this define.
#define OLO_DEFORM_WANT_PREV
#define OLO_DEFORM_PREV_BONE(i) instances[oloVgBoneBase + uint(i)].PrevTransform
#include "SkeletalDeformation.glsl"

struct VirtualSkinnedVertex {
    vec3 Position;
    vec3 Normal;
    vec3 PrevPosition;
};

// The instance's bone palette applied to one rest-pose vertex, in OBJECT space
// — before the instance transform, exactly as every classic skinned pass does
// it. The palette is a model-space pose and the instance transform places the
// posed model in the world; doing it the other way round would apply the
// entity's scale to the bone translations.
//
// Returns the vertex UNCHANGED for a rigid instance, an arena with no skin
// tail, or a binding with no usable influence — the last of which is a rigid
// vertex inside a skinned mesh, and returning it unchanged rather than
// collapsed onto the model origin is the producer's own zero-weight rule (and
// the defect #1226 found in three shadow shaders).
//
// `globalVertexIndex` is the SAME index the vertex fetch used (gl_VertexIndex
// on the MDI arm, cluster.VertexBase + local on the mesh arm): it addresses the
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

    // Publish this instance's palette window, then hand the rest to the shared
    // producer. The sentinel id (OLO_NO_SKIN_BONE) survives the ivec4 cast as a
    // value past SkinBoneCount, which the producer's own bounds test drops —
    // and its slot already carries weight 0, so it contributes nothing twice
    // over.
    oloVgBoneBase = inst.SkinBoneBase;
    oloVgBoneCount = inst.SkinBoneCount;

    OloDeformedSurface surface =
        OloDeformSkinnedVertex(restPosition, restNormal, ivec4(binding.BoneIDs), binding.Weights);

    // Position is HOMOGENEOUS by the producer's contract (w is the total
    // influence weight). The cook normalizes every binding's weights to sum to
    // one, so w is 1 here — but the division is what keeps this honest if that
    // ever stops being true, and it is the only place virtual geometry differs
    // from the classic consumers, which multiply the vec4 straight into their
    // model matrix. Virtual geometry cannot: its callers need a vec3 to feed
    // both the instance transform and the cluster bounds.
    float invW = (abs(surface.Position.w) > 1e-6) ? (1.0 / surface.Position.w) : 1.0;
    o.Position = surface.Position.xyz * invW;
    o.Normal = surface.Normal;
    float invPrevW = (abs(surface.PrevPosition.w) > 1e-6) ? (1.0 / surface.PrevPosition.w) : 1.0;
    o.PrevPosition = surface.PrevPosition.xyz * invPrevW;
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
