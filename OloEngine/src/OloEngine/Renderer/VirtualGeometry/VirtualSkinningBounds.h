#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <span>

namespace OloEngine
{
    // =========================================================================
    // Conservative bounds for a DEFORMING cluster DAG (issue #1150).
    //
    // Everything the virtual-geometry cull does is decided from bounds that the
    // builder computed against the REST POSE: the cluster cull sphere, the group
    // LOD sphere, the group's object-space error. A skinned mesh moves away from
    // that pose every frame, and a bound that stops containing its geometry does
    // not fail loudly — the cull simply drops clusters that were on screen, which
    // looks like a character flickering apart rather than like an error. So every
    // bound here is derived, never estimated, and each one is pinned by a test
    // that samples real poses and checks containment vertex by vertex
    // (VirtualSkinnedBoundsTest).
    //
    // THE ONE INEQUALITY EVERYTHING RESTS ON
    //
    // Linear-blend skinning writes a vertex as
    //     p'(v) = SUM_b w_b * M_b * v ,  w_b >= 0 ,  SUM_b w_b = 1
    // so p'(v) is a CONVEX COMBINATION of the points { M_b * v : b influences v }.
    // A convex combination never leaves the convex hull of its terms, so ANY set
    // that contains every M_b * v also contains p'(v). That is what makes the
    // bounds below conservative without knowing the weights — only which bones
    // are involved.
    //
    // TWO BOUNDS, TWO JOBS, AND THEY ARE NOT INTERCHANGEABLE
    //
    //  * SkinnedClusterSphere is TIGHT and PER CLUSTER. It bounds one cluster
    //    under one pose from that cluster's own bone set. It feeds the frustum,
    //    Hi-Z and software-raster-routing tests, all of which only ever REJECT —
    //    a bound that is merely correct costs nothing but a wasted draw, and
    //    tightness is what keeps culling worth doing on a skinned instance.
    //
    //  * SkinDisplacementBound is ONE SCALAR PER INSTANCE, and it is deliberately
    //    coarser. It is added to the radius of every cluster AND every group
    //    sphere of the instance, uniformly. The LOD cut is not a rejection: it is
    //    a choice between DAG levels, and its correctness rests on two builder
    //    invariants — group errors are monotone along every DAG edge, and group
    //    LOD spheres are NESTED. Deform each group's sphere independently and
    //    both invariants are gone, the projected error stops being monotone, and
    //    the cut cracks: neighbouring clusters pick different levels and the
    //    surface splits along the seam. Adding the SAME scalar to every radius
    //    preserves containment exactly (if |c_p - c_c| + r_c <= r_p then
    //    |c_p - c_c| + (r_c + D) <= r_p + D), so the cut stays watertight.
    //
    // The error term takes the same treatment: SkinMaxBoneScale multiplies every
    // group error by one instance-wide factor, which is a monotone map over all
    // errors and therefore preserves their ordering along DAG edges.
    // =========================================================================

    // Largest factor by which a bone transform can stretch a vector: an UPPER
    // BOUND on the operator 2-norm of its linear part, which is the Lipschitz
    // constant every bound below multiplies a radius by.
    //
    // Computed as sqrt(||A||_1 * ||A||_inf) — the column-sum norm times the
    // row-sum norm — and the choice matters for CORRECTNESS, not for speed.
    // The obvious cheap answer, "the length of the longest column", is exact for
    // a rotation-times-scale and is NOT an upper bound in general: a sheared
    // matrix (which a non-uniformly scaled parent bone composed with a rotation
    // produces) can stretch a vector further than any of its columns. A bound
    // that under-estimates is the whole failure this file exists to prevent, and
    // it would under-estimate only for unusual rigs — i.e. it would hold
    // everywhere it was tested. The interpolation bound above is sound for every
    // matrix, costs nine absolute values, and is exact for the identity.
    [[nodiscard]] f32 SkinBoneScale(const glm::mat4& boneMatrix);

    // The same bound applied to (A - I): how far the bone's linear part can move
    // a vector RELATIVE to leaving it alone. Zero for a bone that is not posed,
    // which is what makes the displacement bound below exactly zero in the rest
    // pose rather than merely small.
    [[nodiscard]] f32 SkinBoneDeltaScale(const glm::mat4& boneMatrix);

    // max over bones of SkinBoneScale. Multiplies every group error of the
    // instance, because a surface deviation of E in rest pose maps to a
    // deviation of at most (max bone scale) * E once the same skin is applied to
    // both surfaces.
    //
    // Returns 1.0 for an empty palette, which is exactly the rigid behaviour.
    [[nodiscard]] f32 SkinMaxBoneScale(std::span<const glm::mat4> palette);

    // The instance-wide conservative displacement bound D: no point of the rest
    // surface moves further than this when the palette is applied.
    //
    // Derivation, for a vertex v influenced by bone b, where (c_b, R_b) is the
    // rest-pose sphere of everything b influences (so |v - c_b| <= R_b). Writing
    // the bone as M_b v = A v + t and d = v - c_b:
    //     M_b v - v = (A - I)(c_b + d) + t = (M_b c_b - c_b) + (A - I) d
    // so  |M_b v - v| <= |M_b c_b - c_b| + ||A - I|| * R_b
    // and |p'(v) - v| <= max over the influencing b of |M_b v - v| because p'(v)
    // is a convex combination of the M_b v and v is the same convex combination
    // of copies of itself. Taking the max over ALL bones is then a bound for
    // every vertex at once — which is what makes this O(bones) per frame rather
    // than O(vertices), and cheap enough to run on the render thread for every
    // skinned instance.
    //
    // `boneBounds` is indexed by bone id and may be shorter or longer than the
    // palette; a bone present in only one of the two contributes nothing, which
    // is correct — a bone with no bound influences no vertex, and a bound with
    // no matrix is not being posed.
    [[nodiscard]] f32 SkinDisplacementBound(std::span<const VirtualBoneBounds> boneBounds,
                                            std::span<const glm::mat4> palette);

    // The deformed bounding sphere of one cluster: xyz = centre, w = radius, in
    // the same object space the rest sphere is given in.
    //
    // `boneRefs` is the cluster's fixed-width bone list (kMaxClusterBones
    // entries, kNoClusterBone for empty). A list that is empty, or that names
    // ANY bone the palette does not have, returns the rest sphere UNCHANGED —
    // the caller must then fall back to the instance-wide bound, and
    // SkinnedClusterSphereIsTight says which case it got rather than leaving the
    // caller to compare spheres.
    //
    // GLSL twin: SkinnedClusterSphere in compute/VirtualClusterCull.comp, on the
    // helpers in include/VirtualSkinning.glsl.
    [[nodiscard]] glm::vec4 SkinnedClusterSphere(const glm::vec4& restSphere, std::span<const u32> boneRefs,
                                                 std::span<const glm::mat4> palette);

    // Whether `boneRefs` names at least one bone and EVERY bone it names is in
    // the palette — i.e. whether SkinnedClusterSphere returned a real deformed
    // bound rather than the rest sphere it was handed.
    //
    // "Every", not "at least one", and that is the correctness half: a vertex
    // whose only influences are bones the palette does not have is left in its
    // REST position by the skin, and the hull of the bones that ARE present need
    // not contain that position.
    [[nodiscard]] bool SkinnedClusterSphereIsTight(std::span<const u32> boneRefs, std::span<const glm::mat4> palette);

    // Linear-blend skinning of one position by one skin binding. The CPU
    // reference the shaders mirror, and the oracle the containment tests
    // generate their sample points with.
    //
    // A binding whose weights sum to (near) zero is treated as UNSKINNED and
    // returns the position unchanged — the same rule PBR_GBuffer_Skinned.glsl
    // applies, and the reason a rigid vertex inside a skinned mesh does not
    // collapse to the origin.
    [[nodiscard]] glm::vec3 SkinPosition(const glm::vec3& position, const VirtualVertexSkinning& binding,
                                         std::span<const glm::mat4> palette);
} // namespace OloEngine
