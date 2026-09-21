#pragma once

#include "OloEngine/Containers/Array.h"

// =============================================================================
// GroomBodyCollider.h — the body a coat is not allowed inside. Issue #1250.
//
// THE PROXY IS FITTED, NOT AUTHORED, and that is the decision this file is
// about. Acceptance criterion 2 asks for "representative body shapes"; the two
// ways to get them are to hand an artist a rig of capsules to place, or to
// derive them from the body that is already there. This derives them:
//
//   * Every rigged body already carries the information. A bone's vertices ARE
//     that limb, so a capsule fitted to them is a better proxy than one placed
//     by eye, and it cannot go stale when the mesh is re-exported.
//   * An authored rig is a new asset type, a new editor, a new cook and a new
//     way for a coat to be silently attached to last week's skeleton. Criterion
//     2 is about the coat not penetrating the body, and none of that machinery
//     makes it more true.
//   * The fit is a pure function of the surface, so it is CACHED against the
//     same identity keys the binding's compatibility verdict uses and costs one
//     pass over the body per LOD switch — not per frame.
//
// A hand-tuned proxy asset is a legitimate later refinement; the per-entity
// radius scale and padding on GroomSimulationComponent are the knobs that stand
// in for it, and they are enough to lift a coat off a body the fit made too
// tight. What the fit removes is the case where there is NO proxy at all,
// which is a coat that passes straight through the animal.
//
// HOW ONE BONE BECOMES ONE CAPSULE. Take the REST positions of the vertices this
// bone dominates, find their mean and their principal axis (power iteration on
// the covariance — a limb is an elongated cloud, so the dominant eigenvector is
// the limb), project onto it and cut at a low and a high percentile so one
// stray vertex cannot stretch the capsule down the leg. The radius is a
// percentile of the perpendicular distance, for the same reason. Percentiles,
// not extrema, throughout: a proxy that contains every last vertex of a hand is
// a sphere around the whole hand, which pushes the coat off the arm.
//
// THE SPACE. The fit is in the body's REST object space, so a binding carries it
// exactly as it carries a strand: `palette[bone]` — the shared deformation
// output's final bone matrix (#1226) — maps it to this frame's pose. Nothing
// here needs the skeleton hierarchy, bone names or offset matrices, which is
// what lets it be tested against a synthetic surface with no Skeleton at all.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomGuideSimulation.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace OloEngine
{
    /// One fitted capsule, in the body's REST object space, plus the bone that
    /// carries it. 32 bytes, hole-free — this is built in bulk and walked once
    /// per frame.
    struct GroomColliderBinding
    {
        glm::vec3 PointA{ 0.0f };
        f32 Radius = 0.0f;
        glm::vec3 PointB{ 0.0f };
        u32 BoneIndex = 0;

        [[nodiscard]] bool operator==(const GroomColliderBinding&) const = default;
    };

    static_assert(sizeof(GroomColliderBinding) == 32, "GroomColliderBinding is walked once per frame per collider");

    struct GroomColliderBuildSettings
    {
        /// The percentiles the capsule's ENDS and its RADIUS are cut at. Extrema
        /// would let one vertex of a fingertip define the forearm.
        f32 AxisLowPercentile = 0.02f;
        f32 AxisHighPercentile = 0.98f;
        f32 RadiusPercentile = 0.85f;

        /// A capsule thinner or shorter than this contributes nothing a strand
        /// can notice and still costs every particle a distance test.
        f32 MinRadius = 1.0e-4f;

        /// Bones with fewer dominated vertices than this are not limbs — they
        /// are twist helpers, attachment points and IK targets, and fitting a
        /// capsule to three vertices produces a shape with no relation to the
        /// body.
        u32 MinVerticesPerBone = 8;

        /// The output cap. Kept at or below GroomSimulationLimits::MaxColliders
        /// by the caller; when the fit produces more, the LARGEST by volume are
        /// kept, because a coat penetrating a torso is visible and a coat
        /// penetrating a finger is not.
        u32 MaxColliders = 32;

        [[nodiscard]] bool operator==(const GroomColliderBuildSettings&) const = default;
    };

    /// What the fit did. Non-zero skip counts are facts about the BODY, which is
    /// why they are counted rather than logged: "this coat has no proxy" needs
    /// an answer in the inspector, not a line in a file.
    struct GroomColliderBuildStats
    {
        u32 BonesConsidered = 0;
        u32 CollidersBuilt = 0;
        u32 BonesSkippedTooFewVertices = 0;
        u32 BonesSkippedDegenerate = 0;
        /// True when the cap dropped fitted capsules. The ones dropped are the
        /// smallest, and saying so is what keeps "my fingers poke through" from
        /// being a mystery.
        bool Truncated = false;

        [[nodiscard]] bool operator==(const GroomColliderBuildStats&) const = default;
    };

    /**
     * @brief Fit one capsule per bone of `surface`, in its rest object space.
     *
     * `outBindings` is cleared first and left sorted by BoneIndex, so two runs
     * over the same body produce the same bytes — the determinism the cached
     * proxy and any test of it both depend on.
     *
     * An unskinned surface (no influences, or an empty palette) produces NO
     * colliders and is not an error: a static or morph-only body has nothing to
     * carry a capsule, and the honest answer is a coat with no body collision
     * rather than a proxy pinned to bone zero.
     */
    GroomColliderBuildStats BuildGroomBodyColliders(const GroomSurfaceView& surface,
                                                    const GroomSkinningView& skinning,
                                                    const GroomColliderBuildSettings& settings,
                                                    TArray<GroomColliderBinding>& outBindings);

    /**
     * @brief Carry the fitted capsules into this frame's pose, in WORLD space.
     *
     * `bindingToWorld` is the full body-rest-to-world chain the caller composes
     * once: the groom's world transform times the body-to-groom matrix. The
     * bone's own matrix is applied here, from `palette`, which is the shared
     * deformation output — the same array the strand roots are carried by, so
     * the coat and its colliders cannot end up a frame apart.
     *
     * The RADIUS is scaled by the bone matrix's mean axis length, so a body
     * scaled up at runtime gets a proxy that scales with it instead of a coat
     * that suddenly intersects. A non-uniformly scaled bone gets the mean, which
     * is an approximation and is named as one here: a capsule has one radius, so
     * there is no non-uniform answer to give.
     *
     * `outColliders` is cleared first. A binding naming a bone outside `palette`
     * is DROPPED, not clamped to a valid bone: a capsule at the wrong limb
     * pushes the coat somewhere plausible and wrong.
     */
    void ResolveGroomBodyColliders(std::span<const GroomColliderBinding> bindings,
                                   std::span<const glm::mat4> palette, const glm::mat4& bindingToWorld,
                                   f32 radiusScale, TArray<GroomCollider>& outColliders);
} // namespace OloEngine
