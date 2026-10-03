#pragma once

// =============================================================================
// GroomCasterPose.h -- a shadow caster's runs in THIS frame's pose (#1533).
//
// A shadow view casts a prefix of each run of a coat's strands, as many as
// DecideGroomCasterRun allows from the run's direction moments and box. Those
// come from the builder's walk in the groom's REST space, which is right for an
// unbound coat and wrong for a bound one in any pose but the bind pose: a body
// that turns a run of strands toward the light makes it lay fewer layers than
// its rest shape says, and the view thins it into holes. So a bound coat's runs
// are re-posed every frame before the views decide:
//
//   * MOMENTS from a SAMPLE of each run's strands -- the first
//     kGroomCasterPoseSamples of the run's hashed order, which are a uniform
//     random sample of it (BuildGroomCasterOrder) -- each strand's rest moments
//     turned by its root's rotation this frame, the change scaled from the
//     sample's length to the run's. Second moments add, so the estimate is
//     unbiased, and exact at the bind pose and for a run no larger than its
//     sample.
//   * the BOX from the run's strands' own rest boxes, carried by the bones
//     that move their roots. A box that is too large lowers the layer estimate
//     and keeps strands; one too small would thin them. A box of the posed
//     ROOTS padded by the run's longest strand on every side was safe and cast
//     three quarters of the dog where its rest boxes cast half.
//
// A root's rotation is its frame this frame against its bind frame, the
// rotation the strand's points are carried by (ApplyGroomRootTransform). Two
// sources, one per way a bound coat is moved: the ROOTS the CPU evaluated, and
// the SKELETON when the GPU evaluates the drawn roots -- each sampled root's
// triangle skinned and framed here exactly as EvaluateGroomRootTransforms does,
// and each run's box from its strands' boxes per bone (SkinGroomCasterPose).
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomGpuDeformation.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <span>
#include <vector>

namespace OloEngine
{
    class GroomBindingAsset;
    struct GroomDeformationInputs;
    struct GroomRootTransform;
    struct GroomSkinningView;
    struct GroomStrandSimulation;
    struct GroomSurfaceView;

    /// Strands of each run the posed moments are measured on.
    inline constexpr u32 kGroomCasterPoseSamples = 128;

    /// The share of a run's reach its skeleton box is padded by: a strand on a
    /// triangle two bones move turns with the blend of their frames, which can
    /// carry its tip a little outside both bones' images of its box.
    inline constexpr f32 kGroomCasterBlendAllowance = 0.15f;

    /// One strand's REST box, the groom's space.
    struct GroomCasterStrandBox
    {
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
    };

    /// The builder's summaries' boxes, in strand order: what a run's skeleton
    /// box is built from.
    [[nodiscard]] std::vector<GroomCasterStrandBox> CollectGroomCasterStrandBoxes(
        std::span<const GroomCasterStrand> strands);

    /// One sampled strand's rest summary.
    struct GroomCasterPoseSample
    {
        std::array<f32, 6> Moments{};
        f32 Length = 0.0f;
        u32 Strand = 0;
    };

    /// One run's share of the pose.
    struct GroomCasterRunPose
    {
        u32 FirstSample = 0;
        u32 SampleCount = 0;
        f32 SampleLength = 0.0f;
        /// Where the run's strands start in the order's strand order
        /// (BuildGroomCasterOrder's outStrandOrder).
        u32 FirstStrand = 0;
        u32 StrandCount = 0;
        /// The farthest any point of the run sits from its own root, rest
        /// space: the largest diagonal of its strands' rest boxes.
        f32 Reach = 0.0f;
        /// Once skinned: per bone, the box in the SURFACE's rest space of the
        /// run's strands whose root triangle that bone moves; the strands no
        /// bone moves (surface space) and those held at rest (groom space), as
        /// PoseGroomRootBoneBounds poses them.
        GroomRootBoneBounds Boxes;
    };

    struct GroomCasterPose
    {
        /// Parallel to the order's runs.
        std::vector<GroomCasterRunPose> Runs;
        std::vector<GroomCasterPoseSample> Samples;
        /// True once SkinGroomCasterPose filled each run's bone boxes.
        bool Skinned = false;

        [[nodiscard]] bool IsUsable() const noexcept
        {
            return !Runs.empty();
        }
        [[nodiscard]] u64 CpuBytes() const noexcept;
    };

    /// The pose-independent half: each run's sample and strand range, from the
    /// order a caster stream was built with (`strandOrder` is its
    /// outStrandOrder) and the builder's summaries. Empty when they do not
    /// describe the runs.
    [[nodiscard]] GroomCasterPose BuildGroomCasterPose(std::span<const GroomCasterRun> runs,
                                                       std::span<const u32> strandOrder,
                                                       std::span<const GroomCasterStrand> strands);

    /// The skeleton's half: each run's strand boxes per bone. Each strand's
    /// rest box is carried into the surface's rest space by its own root's
    /// rigid bind map (the root's rest frame on the surface against its bind
    /// frame in the groom) and joins the box of every bone that moves its root
    /// triangle's corners, read and rejected as the kernel reads them.
    /// `strandOrder` is the one the pose was built from, `rootCurves` maps a
    /// strand to its base curve (the rest stream's root slots) and
    /// `strandBoxes` holds each strand's rest box.
    void SkinGroomCasterPose(GroomCasterPose& pose, std::span<const u32> strandOrder, std::span<const u32> rootCurves,
                             std::span<const GroomCasterStrandBox> strandBoxes, const GroomBindingAsset& binding,
                             const GroomSurfaceView& surface, const GroomSkinningView& skinning, u32 boneCount,
                             u32 baseCurveCount);

    /// `moments` (xx, yy, zz, xy, xz, yz of sum l * outer(t, t)) turned by
    /// `rotation`: R M R^T.
    [[nodiscard]] std::array<f32, 6> RotateGroomCasterMoments(const std::array<f32, 6>& moments,
                                                              const glm::mat3& rotation) noexcept;

    /// The farthest the simulation displaced any guide point this frame -- how
    /// far past its rigid pose a simulated strand can reach. Zero without one.
    [[nodiscard]] f32 GroomGuideDisplacementReach(const GroomStrandSimulation* simulation) noexcept;

    /// What a run's posed box is padded by beyond its source's own allowance:
    /// the farthest the simulation displaced any guide point this frame, and
    /// the widest drawn radius.
    struct GroomCasterPosePadding
    {
        f32 Displacement = 0.0f;
        f32 Radius = 0.0f;
    };

    /// The form both sources reduce to: `restRuns` with each run's moments
    /// estimated from its sample turned by `rotationOf`, and its box from
    /// `posedBoxOf` -- false when the run has none -- padded by `reachShare`
    /// of its reach plus `padding`. A run without moments keeps its rest
    /// numbers. False, with `outRuns` untouched, when the pose does not
    /// describe the runs.
    bool PoseGroomCasterRuns(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                             const std::function<glm::mat3(const GroomCasterPoseSample&)>& rotationOf,
                             const std::function<bool(sizet run, glm::vec3& min, glm::vec3& max)>& posedBoxOf,
                             f32 reachShare, const GroomCasterPosePadding& padding,
                             std::vector<GroomCasterRun>& outRuns);

    /// This frame's runs under the SKELETON: each sampled root's triangle
    /// skinned by `inputs`' palette, carried by its SurfaceToGroom and framed as
    /// EvaluateGroomRootTransforms frames it; each run's box its bone boxes
    /// posed, padded by kGroomCasterBlendAllowance of its reach. False when the
    /// pose was not skinned or does not describe the runs.
    bool PoseGroomCasterRunsBySkeleton(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                                       std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                                       const GroomDeformationInputs& inputs, const GroomCasterPosePadding& padding,
                                       std::vector<GroomCasterRun>& outRuns);

    /// This frame's runs under the ROOTS the CPU evaluated: each sample's
    /// rotation is its root transform's against its bind frame, and each run's
    /// box spans its strands' posed root origins padded by its whole reach --
    /// looser than the skeleton's, on the paths that have no skeleton. A root not evaluated (not
    /// Valid) is at rest. `strandOrder` is the one the pose was built from,
    /// `rootCurves` maps a strand to its base curve, and `transforms` is indexed
    /// by base curve.
    bool PoseGroomCasterRunsByRoots(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                                    std::span<const u32> strandOrder, std::span<const u32> rootCurves,
                                    const GroomBindingAsset& binding, std::span<const GroomRootTransform> transforms,
                                    const GroomCasterPosePadding& padding, std::vector<GroomCasterRun>& outRuns);
} // namespace OloEngine
