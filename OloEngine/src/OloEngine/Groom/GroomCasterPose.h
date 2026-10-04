#pragma once

// =============================================================================
// GroomCasterPose.h -- a shadow caster's runs in THIS frame's pose (#1533).
//
// A shadow view casts a prefix of each run of a coat's strands, as many as
// DecideGroomCasterRun allows from the run's direction moments and box. The
// walk records both in the groom's REST space, which is right for an unbound
// coat and wrong for a bound one in any pose but the bind pose: a body that
// turns a run of strands toward the light makes it lay fewer layers than its
// rest shape says, and the view would thin it into holes. So a bound coat's
// runs are re-posed every frame before the views decide, and the decision
// relies on two properties of what comes out:
//
//   * the MOMENTS are the posed strands' own: DecideGroomCasterRun's
//     projected-length bound L - d^T M d is a lower bound only for the actual
//     second moments of the geometry it is applied to;
//   * the BOX holds every posed point: a box that is too small raises the
//     layer estimate and thins strands; one too large only keeps more.
//
// HOW, EXACTLY. A bound strand is carried rigidly by its root's frame, and the
// frame is its root TRIANGLE's (MakeGroomSurfaceFrame: the triangle's normal
// and first edge; the barycentric only moves the origin). So every strand of a
// run rooted on one triangle turns by the same rotation, and the run is kept
// as ENTRIES, one per (run, root triangle), each the sum of its strands' rest
// moments and the box of their points' offsets from their own bind origins.
// A pose evaluates each root triangle's frame once and turns each entry:
//
//     M_run = sum_e R_e M_e R_e^T                    (exact, not estimated)
//     box_run = union_e hull(posed corners) + R_e(offset box_e)
//
// The origin lies in its triangle (a barycentric point), the offsets turn by
// R_e, so the box holds every posed point whatever the skinning did to the
// triangle -- a blended triangle that shrinks keeps its frame and its strands'
// full reach, which no bone's image of the strands bounds.
//
// WHAT THE POSE CANNOT SEE is made explicit, never estimated away:
//   * a strand with no usable binding record is drawn at rest: its rest
//     moments and box, always;
//   * a triangle that does not frame this pose is held at rest by both the CPU
//     and the GPU evaluation: the entry's rest moments, and the run's rest box;
//   * a triangle whose frame the GPU's float arithmetic could turn
//     differently from ours (GroomCasterFrameTolerance) costs the bound that
//     difference -- each segment's angle to the light is uncertain by it -- and
//     a frame too ill-conditioned to trust gets no projected-length credit at
//     all (ProjectedLengthLoss) and a box of its hull padded by its full reach,
//     joined with the run's rest box;
//   * the guide simulation moves points after the root transform, by a blend
//     of guide displacements whose weights sum to one. The box is padded by
//     the farthest any guide slot the run's strands draw on was displaced, and
//     the bound gives up each strand's weighted sum of those slots' total
//     variation from root to tip: a segment's projected length shrinks by no
//     more than the change in displacement between its ends, and a blend's
//     variation is at most the blend of its parts'. When the frame leaves a
//     slot out without a stand-in (its strands' weights are renormalised), or
//     the pose was built without the influence table, each strand gives up the
//     largest variation of any guide of its coat roles instead.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <span>
#include <vector>

namespace OloEngine
{
    class GroomBindingAsset;
    class GroomGuideInfluenceTable;
    struct GroomDeformationInputs;
    struct GroomRootTransform;
    struct GroomStrandSimulation;
    struct GroomSurfaceView;

    /// One run's strands rooted on one triangle: what they share under a pose
    /// (the triangle's frame) and what they sum to at rest, both in their BIND
    /// frame -- the frame the binder recorded for every root of that triangle,
    /// which a pose turns into the triangle's frame this frame.
    struct GroomCasterPoseEntry
    {
        /// The entry's triangle among GroomCasterPose::Triangles.
        u32 Slot = 0;
        /// Its strands' base curves are GroomCasterPose::EntryCurves
        /// [FirstCurve, FirstCurve + Strands).
        u32 FirstCurve = 0;
        u32 Strands = 0;
        /// Their summed moments (xx, yy, zz, xy, xz, yz of sum l t t^T) and
        /// length, in the bind frame.
        std::array<f32, 6> Moments{};
        f32 Length = 0.0f;
        /// Every drawn point's offset from its own root's bind origin, in the
        /// bind frame, boxed (each strand's rest box, radius included, less
        /// its origin and turned into the frame).
        glm::vec3 OffsetMin{ 0.0f };
        glm::vec3 OffsetMax{ 0.0f };
    };

    /// One run's share of the pose.
    struct GroomCasterRunPose
    {
        u32 FirstEntry = 0;
        u32 EntryCount = 0;
        u32 Strands = 0;
        /// The strands no binding record frames: drawn at rest in every pose.
        std::array<f32, 6> HeldMoments{};
        f32 HeldLength = 0.0f;
        glm::vec3 HeldMin{ 0.0f };
        glm::vec3 HeldMax{ 0.0f };
        bool HasHeld = false;
        /// The coat roles of the run's strands, as bits: the guides whose
        /// displacement can reach them.
        u32 RoleMask = 0;
        /// The guide slots the run's strands are displaced by, with their
        /// influence weights summed over the strands
        /// (GroomCasterPose::SlotWeights[FirstSlotWeight, + SlotWeightCount)).
        u32 FirstSlotWeight = 0;
        u32 SlotWeightCount = 0;
    };

    /// One guide slot's summed influence on a run's strands.
    struct GroomCasterSlotWeight
    {
        u32 Slot = 0;
        f32 Weight = 0.0f;
    };

    /// A caster stream's runs, kept so any pose can be evaluated exactly. Built
    /// once with the stream.
    struct GroomCasterPose
    {
        /// Parallel to the order's runs.
        std::vector<GroomCasterRunPose> Runs;
        std::vector<GroomCasterPoseEntry> Entries;
        /// The distinct root triangles the entries name (surface triangle ids).
        std::vector<u32> Triangles;
        std::vector<u32> EntryCurves;
        std::vector<GroomCasterSlotWeight> SlotWeights;
        /// True when the pose was built with the groom's influence table, so a
        /// run's slot weights are known (none: its strands are not simulated).
        bool SlotWeightsKnown = false;

        [[nodiscard]] bool IsUsable() const noexcept
        {
            return !Runs.empty();
        }
        [[nodiscard]] u64 CpuBytes() const noexcept;
    };

    /// The per-entity half for a pose evaluated from the SURFACE: each of the
    /// pose's triangles as indices into a compact list of the surface vertices
    /// they use, so a frame skins each vertex once.
    struct GroomCasterPoseSurface
    {
        std::vector<u32> Vertices;
        /// Per pose triangle; kNoCorners where the surface has no such triangle.
        std::vector<glm::uvec3> Corners;
        static constexpr u32 kNoCorners = ~0u;

        [[nodiscard]] bool IsUsable() const noexcept
        {
            return !Corners.empty();
        }
        [[nodiscard]] u64 CpuBytes() const noexcept;
    };

    /// A strand's drawn points in its root's bind frame, boxed.
    struct GroomCasterLocalBox
    {
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
    };

    /// Each strand's box in its bind frame, from a REST stream -- whose
    /// vertices are each strand's points in its root's bind frame
    /// (BuildGroomStrandRestMesh) -- with each point's radius. Empty when the
    /// stream does not describe `strandFirstIndex`'s strands.
    [[nodiscard]] std::vector<GroomCasterLocalBox> CollectGroomCasterLocalBoxes(
        std::span<const GroomStrandVertex> restVertices, std::span<const u32> indices,
        std::span<const u32> strandFirstIndex);

    /// Builds the pose of a caster order's runs (`runs`, `strandOrder` its
    /// outStrandOrder) from the builder's summaries, the strands' base curves
    /// (`strandCurves[strand]`) and the binding they are drawn with. Empty when
    /// these do not describe the runs; a run whose strands have no usable
    /// binding is all held. `localBoxes`, from a rest stream, gives each
    /// strand's exact box in its bind frame; without it a strand's rest box is
    /// turned into the frame, which holds the same points more loosely.
    /// `influence`, the groom's guide influence table, gives each run's slot
    /// weights for the simulation's per-slot bound.
    [[nodiscard]] GroomCasterPose BuildGroomCasterPose(std::span<const GroomCasterRun> runs,
                                                       std::span<const u32> strandOrder,
                                                       std::span<const GroomCasterStrand> strands,
                                                       std::span<const u32> strandCurves,
                                                       const GroomBindingAsset& binding,
                                                       std::span<const GroomCasterLocalBox> localBoxes = {},
                                                       const GroomGuideInfluenceTable* influence = nullptr);

    /// The surface half for `pose` on `surface`.
    [[nodiscard]] GroomCasterPoseSurface BuildGroomCasterPoseSurface(const GroomCasterPose& pose,
                                                                     const GroomSurfaceView& surface);

    /// `moments` (xx, yy, zz, xy, xz, yz of sum l * outer(t, t)) turned by
    /// `rotation`: R M R^T.
    [[nodiscard]] std::array<f32, 6> RotateGroomCasterMoments(const std::array<f32, 6>& moments,
                                                              const glm::mat3& rotation) noexcept;

    /// What a run's posed box and projected-length bound must allow for beyond
    /// the root transforms, per coat role.
    struct GroomCasterPosePadding
    {
        /// The widest drawn radius.
        f32 Radius = 0.0f;
        /// The farthest any simulated guide of the role was displaced this frame.
        std::array<f32, GroomCoatRoleCount> Displacement{};
        /// The most any such guide's displacement changes from root to tip,
        /// summed along it (its total variation): the most the simulation can
        /// shorten one strand's projected length.
        std::array<f32, GroomCoatRoleCount> Variation{};
        /// Per guide SLOT, its displacement's farthest point and total
        /// variation this frame; SlotsComplete when every slot has a
        /// displacement (every guide simulated, or the stand-ins expanded), so
        /// every strand blends its own slots with its table weights.
        std::vector<f32> SlotDisplacement;
        std::vector<f32> SlotVariation;
        bool SlotsComplete = false;
    };

    /// This frame's padding: each simulated guide's displacement and variation
    /// under the coat role of its curve (`coat` resolves a group's role; none
    /// puts every guide and strand in one role), and `radius`. Without a usable
    /// simulation only the radius.
    [[nodiscard]] GroomCasterPosePadding MeasureGroomCasterPosePadding(const GroomStrandSimulation* simulation,
                                                                       std::span<const u16> curveGroups,
                                                                       const GroomCoatContext* coat, f32 radius);

    /// How far the GPU's frame of a triangle can turn from this file's: the
    /// first-order effect of a disagreement of `positionTolerance` per corner,
    /// in radians. Infinite for a triangle that does not frame.
    [[nodiscard]] f32 GroomCasterFrameTolerance(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                                f32 positionTolerance) noexcept;

    /// The relative float disagreement allowed between the CPU's and the GPU's
    /// skinned corners (GroomRootFrames.comp skins the same inputs in f32), and
    /// the frame error past which a triangle earns no projected-length credit.
    inline constexpr f32 kGroomCasterPositionTolerance = 1.0e-6f;
    inline constexpr f32 kGroomCasterMaxFrameError = 0.05f;

    /// This frame's runs from the SURFACE the GPU evaluates the drawn roots
    /// from: each of the pose's triangles skinned by `inputs`' palette (or as
    /// the surface holds it, unskinned), carried by its SurfaceToGroom and
    /// framed as GroomRootFrames.comp and EvaluateGroomRootTransforms frame it.
    /// False, with `outRuns` untouched, when the pose does not describe the
    /// runs or the surface half is not this surface's.
    bool PoseGroomCasterRunsBySurface(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                                      const GroomCasterPoseSurface& surface, const GroomDeformationInputs& inputs,
                                      const GroomCasterPosePadding& padding, std::vector<GroomCasterRun>& outRuns);

    /// This frame's runs from the ROOTS the CPU evaluated, which are exactly
    /// what the CPU-deformed stream draws: each entry turned by its first
    /// curve's transform against its bind frame, its box its strands' posed
    /// origins plus its turned offsets. A root not evaluated (not Valid) is at
    /// rest. `transforms` is indexed by base curve.
    bool PoseGroomCasterRunsByRoots(std::span<const GroomCasterRun> restRuns, const GroomCasterPose& pose,
                                    std::span<const GroomRootTransform> transforms,
                                    const GroomCasterPosePadding& padding, std::vector<GroomCasterRun>& outRuns);
} // namespace OloEngine
