#pragma once

// =============================================================================
// GroomGuideSimulation.h — the moving half of a coat. Issue #1250.
//
// THE RULE THIS FILE EXISTS TO ENFORCE: a strand's LENGTH is a property of the
// groom, not of the frame rate. Everything below follows from that.
//
// WHAT IS SIMULATED. Only the GUIDE curves — the ones the groom flagged
// (GroomCurveFlag::Guide) — and only the ones a per-role budget selected. A
// production coat is 200k strands and 300 guides; simulating the coat is not a
// budget problem, it is three orders of magnitude out. The rendered strands
// INTERPOLATE from the guides, and that half lives in GroomGuideInfluence.h
// because it is a different question with a different cost curve.
//
// WHAT A GUIDE IS SIMULATED AGAINST. Its own groomed rest shape, carried by the
// body — i.e. exactly the positions #1249's GroomRootTransform already produces,
// mapped into world space. So this solver is a DEVIATION from that shape, not a
// replacement for it: with zero gravity, zero motion and no collider the
// simulated position IS the deformed rest position, bit for bit, and
// GroomGuideSimulationTest asserts that rather than asserting it looks right.
// That is what makes "preserves rest shape" a number instead of an opinion.
//
// WHY WORLD SPACE. Gravity is world-space, the body's colliders are world-space,
// and an inertial coat must react to the ENTITY moving through the level, not
// only to the body bending inside its own object space. Simulating in object
// space and adding a pseudo-force for the object-to-world change is the other
// option; it is the same arithmetic with one more chance to get a sign wrong,
// and it makes "the entity teleported" indistinguishable from "the coat
// stretched". The caller maps the answer back to object space once, at the end.
//
// ── WHY FOLLOW-THE-LEADER, AND WHAT WAS MEASURED ────────────────────────────
//
// The issue asked for a solver to be prototyped and measured before being
// committed to. Three were, all three are still here (they differ by a few
// lines, and a measurement nobody can re-run is a claim), and the numbers are in
// docs/analysis/groom-guide-simulation-1250.md:
//
//   * FollowTheLeader — one root-to-tip pass that PROJECTS each particle onto
//     its exact rest length from the already-corrected parent. Length error is
//     zero BY CONSTRUCTION, at any step size, with no iteration count to tune.
//     Its known flaw is that the projection silently removes momentum, so a
//     coat settles like it is in syrup.
//   * DynamicFollowTheLeader — the same pass plus Müller's velocity correction:
//     the momentum the projection removed is handed back to the particle that
//     lost it. Same exact-length guarantee, none of the over-damping. THE
//     DEFAULT.
//   * PositionBasedDistance — Gauss-Seidel distance constraints, N iterations.
//     The general tool, and the one that fails the criterion: its residual
//     stretch is a function of iteration count AND step size, so the same coat
//     is inextensible at 60 Hz and visibly rubbery through a frame spike. It is
//     kept because it is the reference the other two are measured against.
//
// The choice is therefore not taste. An exact-length projection makes
// acceptance criterion 1 ("preserve length across variable frame rate") a
// property of the algorithm; an iterative solver makes it a property of the
// tuning, which is the same thing as not having it.
//
// ── VARIABLE FRAME RATE, PAUSE/RESUME, TELEPORT ─────────────────────────────
//
// A FIXED STEP WITH A BOUNDED CATCH-UP, exactly as JoltScene does it and for the
// same reason: integrating a raw variable dt makes the coat's stiffness a
// function of the frame rate, so the same asset hangs differently on a fast
// machine. The accumulator carries the remainder; the catch-up is clamped to
// MaxSubsteps and the overflow is DROPPED and COUNTED (GroomSimulationStats::
// StepsClamped), never silently integrated — a 4-second alt-tab must not run
// 240 steps and detonate.
//
// Pause is dt == 0, which takes zero steps and leaves the state untouched, so
// resume continues from the pose the pause froze. That falls out of the
// accumulator; it is stated here because "pause/resume" is an acceptance
// criterion and a criterion met by accident is a criterion that regresses.
//
// TELEPORT RESETS, IT DOES NOT STRETCH, and that is the interesting one. The
// caller decides (it holds the entity's previous world position and the
// threshold); this file obeys `HasHistory`. On a reset the state is SNAPPED to
// the target shape and the accumulator is zeroed, so the first frame after a
// teleport draws the groomed coat and emits zero motion — rather than a coat
// whose tips are still at the old level and whose length constraint is now
// resolving a hundred-metre segment.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // The solver, as a value
    // -------------------------------------------------------------------------
    // Stored as a u8 on the component and in a save game. APPEND, NEVER
    // RENUMBER.
    enum class GroomSolverModel : u8
    {
        /// One root-to-tip projection pass. Exact length, over-damped.
        FollowTheLeader = 0,
        /// The same pass with Müller's velocity correction. Exact length,
        /// correct momentum. The default, and the measured pick.
        DynamicFollowTheLeader = 1,
        /// Gauss-Seidel distance constraints. Residual stretch depends on the
        /// iteration count and the step; kept as the measurement reference.
        PositionBasedDistance = 2,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GroomSolverModel model) noexcept
    {
        switch (model)
        {
            case GroomSolverModel::FollowTheLeader:
                return "FollowTheLeader";
            case GroomSolverModel::DynamicFollowTheLeader:
                return "DynamicFollowTheLeader";
            case GroomSolverModel::PositionBasedDistance:
                return "PositionBasedDistance";
            case GroomSolverModel::Count:
                break;
        }
        return "DynamicFollowTheLeader";
    }

    [[nodiscard]] inline constexpr bool IsValidGroomSolverModel(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomSolverModel::Count);
    }

    // -------------------------------------------------------------------------
    // The debug view
    // -------------------------------------------------------------------------
    // Acceptance criterion 4 asks for debug views, and these are the two
    // questions a coat that moves wrongly raises: "where are the guides" and
    // "where does the body think it is". Stored as a u8 on the component and in
    // a save game. APPEND, NEVER RENUMBER.
    enum class GroomSimulationDebugView : u8
    {
        None = 0,
        /// The simulated guide polylines, in the pose the coat was built from.
        Guides = 1,
        /// The fitted body capsules, at this frame's bone matrices.
        Colliders = 2,
        /// Both.
        GuidesAndColliders = 3,

        Count
    };

    [[nodiscard]] constexpr bool GroomDebugViewShowsGuides(GroomSimulationDebugView view) noexcept
    {
        return view == GroomSimulationDebugView::Guides || view == GroomSimulationDebugView::GuidesAndColliders;
    }

    [[nodiscard]] constexpr bool GroomDebugViewShowsColliders(GroomSimulationDebugView view) noexcept
    {
        return view == GroomSimulationDebugView::Colliders || view == GroomSimulationDebugView::GuidesAndColliders;
    }

    [[nodiscard]] constexpr std::string_view ToString(GroomSimulationDebugView view) noexcept
    {
        switch (view)
        {
            case GroomSimulationDebugView::None:
                return "None";
            case GroomSimulationDebugView::Guides:
                return "Guides";
            case GroomSimulationDebugView::Colliders:
                return "Colliders";
            case GroomSimulationDebugView::GuidesAndColliders:
                return "GuidesAndColliders";
            case GroomSimulationDebugView::Count:
                break;
        }
        return "None";
    }

    [[nodiscard]] inline constexpr bool IsValidGroomSimulationDebugView(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomSimulationDebugView::Count);
    }

    /// A body collider, in WORLD space, already resolved from whatever bone it
    /// rides. A sphere is a capsule whose two points coincide, so there is one
    /// closest-point routine and not two that can disagree about the cap.
    struct GroomCollider
    {
        glm::vec3 PointA{ 0.0f };
        glm::vec3 PointB{ 0.0f };
        f32 Radius = 0.0f;
        f32 Pad0 = 0.0f;

        [[nodiscard]] bool operator==(const GroomCollider&) const = default;
    };

    // -------------------------------------------------------------------------
    // Rejection bounds
    // -------------------------------------------------------------------------
    // REJECTION bounds, not taste bounds, and the same numbers the component,
    // the scene reader and the save-game reader all clamp against — so four
    // places cannot drift to three different opinions about a legal stiffness.
    namespace GroomSimulationLimits
    {
        /// Below 15 Hz a hair chain is not a simulation, it is a flip-book;
        /// above 480 the accumulator spends the whole budget on substeps.
        constexpr f32 MinFixedHz = 15.0f;
        constexpr f32 MaxFixedHz = 480.0f;

        /// The catch-up bound. 8 steps at 60 Hz is 133 ms of arrears, which is
        /// past any frame a running game has and short of the alt-tab that
        /// would detonate an unbounded loop.
        constexpr u32 MinSubsteps = 1;
        constexpr u32 MaxSubsteps = 8;

        /// Acceleration toward the groomed rest shape, per unit of offset.
        /// Zero is legal and means free hair.
        constexpr f32 MinStiffness = 0.0f;
        constexpr f32 MaxStiffness = 2000.0f;

        /// The STEP-DEPENDENT ceiling on stiffness, as the dimensionless product
        /// `Stiffness * dt^2`.
        ///
        /// MaxStiffness alone is not a stability bound, and believing it was is
        /// the bug this constant fixes: the predictor integrates the shape term
        /// explicitly, so a transverse error obeys
        ///
        ///     e' = e + rho*(e - ePrev) - kappa*e,   kappa = Stiffness * dt^2
        ///
        /// with `rho` the per-step velocity retention. Its characteristic
        /// polynomial is `z^2 - (1 + rho - kappa) z + rho`, and Jury's criteria
        /// put both roots inside the unit circle exactly when
        ///
        ///     0 < kappa < 2 * (1 + rho)
        ///
        /// The worst case is rho = 0 -- which the SLOWEST legal step reaches
        /// outright, since damping 60 at 15 Hz gives `1 - 60/15 = -3`, clamped
        /// to zero -- and the bound is then `kappa < 2`, STRICTLY. At that step
        /// the authored maximum of 2000 gives kappa = 8.9: inside every
        /// documented bound, and unstable.
        ///
        /// 1 rather than 2, and the difference is not cosmetic. At exactly 2
        /// with rho = 0 the recurrence is `e' = -e`: a period-two oscillation
        /// that never decays, which is marginal stability rather than stability.
        /// GroomGuideSimulationTest.TheSlowestStepWithTheStiffestCoatStillSettles
        /// was written against the first attempt at this constant and failed on
        /// it, measuring 0.106 m of tip travel a full twenty seconds in. At 1
        /// the rho = 0 case settles in a single step.
        ///
        /// The Follow-the-Leader projection does NOT rescue any of this, which
        /// is why it needs its own test: the projection restores the exact
        /// segment length while leaving the DIRECTION oscillating, so every
        /// length assertion in the suite passes throughout. Positions also stay
        /// bounded by the chain, so the MaxCoordinate poison check never fires.
        /// The only symptom is a coat that never settles.
        ///
        /// At 60 Hz this permits 3600 and therefore never binds on the authored
        /// range; it bites only in the low-rate regime that is genuinely
        /// unstable.
        constexpr f32 MaxStiffnessTimesStepSquared = 1.0f;

        /// Velocity damping rate. 1/s.
        constexpr f32 MinDamping = 0.0f;
        constexpr f32 MaxDamping = 60.0f;

        /// DFTL's velocity-correction coefficient. 0 degenerates to plain FTL
        /// (which is why FTL is not a separate code path below), 1 hands back
        /// every unit of removed momentum and rings.
        constexpr f32 MinVelocityCorrection = 0.0f;
        constexpr f32 MaxVelocityCorrection = 1.0f;

        /// PositionBasedDistance only.
        constexpr u32 MinIterations = 1;
        constexpr u32 MaxIterations = 16;

        /// The DECLARED length tolerance, as a fraction of the rest length.
        /// This is the number acceptance criterion 1 is stated in, so it is a
        /// contract and not a slider: the stats report the measured worst case
        /// against it and the test fails when the measurement exceeds it.
        constexpr f32 MinStretchTolerance = 1.0e-4f;
        constexpr f32 MaxStretchTolerance = 0.5f;
        constexpr f32 DefaultStretchTolerance = 0.01f;

        /// World-space distance a groom's body may move in one frame before the
        /// frame is treated as a cut rather than as motion.
        constexpr f32 MinTeleportDistance = 1.0e-3f;
        constexpr f32 MaxTeleportDistance = 1.0e6f;

        /// How many colliders one groom may be solved against. Every particle
        /// tests every collider, so this multiplies the inner loop directly;
        /// 64 capsules is a full biped plus a tail and is already generous for
        /// a proxy whose whole point is that it is not the render mesh.
        constexpr u32 MaxColliders = 64;

        /// Collider radius scale, and the shell a strand is held off the body
        /// by. The padding is in world units.
        constexpr f32 MinRadiusScale = 0.0f;
        constexpr f32 MaxRadiusScale = 100.0f;
        constexpr f32 MaxPadding = 1.0e3f;

        /// Tangential velocity retained on a collision. 0 is full stick (fur on
        /// skin), 1 is frictionless slide.
        constexpr f32 MinFriction = 0.0f;
        constexpr f32 MaxFriction = 1.0f;

        /// Guides one groom may simulate, summed over roles. A guard against a
        /// corrupt budget sizing an allocation, not a quality tier.
        constexpr u32 MaxGuides = 65536;

        /// Coordinate bound. A particle past it is an integration that has
        /// already failed, and carrying it forward poisons the bounds of every
        /// strand that interpolates from it.
        constexpr f32 MaxCoordinate = 1.0e7f;
    } // namespace GroomSimulationLimits

    /**
     * @brief Everything the solver reads that is not geometry.
     *
     * Every field is validated at the top of the step: a non-finite parameter
     * refuses the whole solve rather than being clamped into something
     * plausible, because a plausible coat produced from a corrupt stiffness is
     * the failure mode that survives review.
     */
    struct GroomSimulationParams
    {
        /// WORLD-space acceleration. Already scaled by the component's
        /// GravityScale before it gets here — one boundary, one interpretation.
        glm::vec3 Gravity{ 0.0f, -9.81f, 0.0f };

        /// Acceleration toward the groomed rest shape, per unit of offset
        /// (1/s^2). This is what makes fur hold its groom instead of hanging
        /// like wet hair, and it is the only term that knows the coat was
        /// authored at all.
        f32 Stiffness = 90.0f;

        /// Velocity damping rate (1/s).
        f32 Damping = 6.0f;

        /// DFTL's velocity correction. Ignored by the other two models.
        f32 VelocityCorrection = 0.85f;

        f32 FixedHz = 60.0f;

        /// The declared length tolerance, as a fraction of rest length.
        f32 StretchTolerance = GroomSimulationLimits::DefaultStretchTolerance;

        /// The world-space shell a strand is held off the body by, ON TOP of
        /// the collider's own radius.
        ///
        /// There is deliberately NO radius SCALE here. The authored scale is
        /// applied exactly once, by ResolveGroomBodyColliders, when it turns a
        /// fitted capsule into a world-space one -- so the capsule this solver
        /// tests against is the same capsule the debug view draws. It was
        /// applied in both places once, which made an authored scale of 2 a 4x
        /// shell and left the overlay disagreeing with the simulation; the
        /// default of 1 hid it in every test.
        f32 ColliderPadding = 0.0f;

        /// Tangential velocity retained on contact. 0 sticks, 1 slides.
        f32 ColliderFriction = 0.35f;

        u32 MaxSubsteps = 4;

        /// PositionBasedDistance only.
        u32 Iterations = 4;

        GroomSolverModel Model = GroomSolverModel::DynamicFollowTheLeader;

        bool CollisionEnabled = true;

        [[nodiscard]] bool operator==(const GroomSimulationParams&) const = default;
    };

    /**
     * @brief The persistent state of one entity's simulated guide set.
     *
     * Lives beside the binding runtime in Scene, keyed by UUID, for the reason
     * that state does: it is per-frame working data whose lifetime is the
     * entity's, and a component holding it could not stay trivially copyable.
     *
     * `Curr` / `Prev` are WORLD space. `RestLengths[i]` is the authored distance
     * from point i-1 to point i and is re-derived from the TARGET every step —
     * not cached from bind time — because a coat whose length multiplier was
     * just moved in the inspector must converge on the new length rather than
     * fight it forever.
     */
    struct GroomGuideSimulationState
    {
        /// Prefix table of length GuideCount + 1 over `Curr` / `Prev`, the same
        /// shape GroomAsset uses for its curves so an index is read the same way
        /// in both places.
        std::vector<u32> GuideOffsets;

        /// The CURVE index each simulated guide corresponds to, parallel to the
        /// guide dimension of `GuideOffsets`. The influence table joins on it.
        std::vector<u32> GuideCurves;

        std::vector<glm::vec3> Curr;
        std::vector<glm::vec3> Prev;

        /// Seconds of un-simulated time carried into the next frame.
        f32 Accumulator = 0.0f;

        /// False until the first successful step. Not redundant with an empty
        /// `Curr`: a groom whose guide budget is zero has an empty state and is
        /// not un-initialised, it is deliberately not simulated.
        bool Initialized = false;

        [[nodiscard]] u32 GuideCount() const noexcept
        {
            return GuideOffsets.empty() ? 0u : static_cast<u32>(GuideOffsets.size() - 1u);
        }

        /// Drop everything. The caller re-seeds from the targets on the next
        /// step, which is what makes a reset a snap rather than a stretch.
        void Clear() noexcept
        {
            GuideOffsets.clear();
            GuideCurves.clear();
            Curr.clear();
            Prev.clear();
            Accumulator = 0.0f;
            Initialized = false;
        }
    };

    /**
     * @brief What one frame's simulation did.
     *
     * Counters, not log lines — a refusal nobody can count reads as "this never
     * happens", which is the same sentence GroomDeformationStats is written
     * under and for the same reason.
     */
    struct GroomSimulationStats
    {
        u32 GuidesSimulated = 0;
        u32 PointsSimulated = 0;
        u32 StepsTaken = 0;

        /// Guides whose ROOT had no deformed frame this tick, so they were
        /// solved against their bind-pose shape while their neighbours moved.
        ///
        /// A per-guide, local wrongness rather than a whole-coat failure -- but
        /// one nobody can see without a number, because the affected guides look
        /// exactly like guides that happen not to be moving. Counted by the
        /// CALLER, which is the only place that holds the root transforms.
        u32 GuidesWithHeldRoots = 0;

        /// Particle-collider overlaps this frame's LAST step had to resolve.
        /// Zero on a coat that is clear of the body; a number that never falls
        /// is a proxy that is too big, which is a fact about the authoring.
        u32 ContactsResolved = 0;

        /// Worst |segment| / restLength over every simulated segment after the
        /// final step. THE criterion-1 number: 1.0 is exact, and the solve is
        /// in contract while |ratio - 1| <= StretchTolerance.
        f32 MaxStretchRatio = 1.0f;

        /// Worst distance from a simulated particle to its groomed rest
        /// position, in world units. The rest-shape half of criterion 1 — a
        /// coat that preserves length perfectly while hanging straight down has
        /// a MaxStretchRatio of exactly 1 and is still wrong.
        f32 MaxRestDeviation = 0.0f;

        /// Seconds of arrears carried into the next frame.
        f32 Accumulator = 0.0f;

        /// True when the catch-up bound dropped time this frame. Surfaced
        /// because a coat that is permanently in arrears looks fine in a still
        /// frame and lags the body by a constant offset in motion, which reads
        /// as a binding error.
        bool StepsClamped = false;

        /// True when this frame RE-SEEDED rather than integrated: the first
        /// frame, a teleport, a guide-set change. The frame emits zero motion.
        bool Reseeded = false;

        /// True when the parameters or the geometry were refused outright and
        /// nothing was simulated. The caller then draws the groomed rest coat,
        /// which is a diagnosable still coat rather than a plausible wrong one.
        bool Refused = false;

        [[nodiscard]] bool operator==(const GroomSimulationStats&) const = default;
    };

    /**
     * @brief The geometry one frame's simulation is solved against.
     *
     * `TargetPoints` is the groomed rest shape carried by the body, in WORLD
     * space, laid out by `GuideOffsets` — i.e. the same prefix-table shape the
     * state uses, and the caller is expected to hand the SAME table to both.
     * `GuideCurves` is carried through to the state so the influence join has
     * something to join on.
     */
    struct GroomSimulationInputs
    {
        std::span<const u32> GuideOffsets{};
        std::span<const u32> GuideCurves{};
        std::span<const glm::vec3> TargetPoints{};
        std::span<const GroomCollider> Colliders{};

        GroomSimulationParams Params{};

        /// Real seconds since the last frame. Zero is PAUSE and is a legal,
        /// meaningful value: no steps run and the state is held exactly.
        f32 DeltaTime = 0.0f;

        /// False re-seeds the state from `TargetPoints` and emits zero motion.
        /// The caller owns this decision because it owns the teleport threshold,
        /// the binding's history verdict and the reset control.
        bool HasHistory = false;
    };

    /**
     * @brief Advance one entity's guide set by `DeltaTime`.
     *
     * Deterministic: the same state, inputs and parameters always produce the
     * same positions, which is what lets a headless test assert on a particle
     * rather than on a picture of one.
     *
     * On refusal — a non-finite parameter, a malformed offset table, a guide
     * count past the limit — the state is CLEARED and `Refused` is set, so the
     * caller falls back to the groomed rest coat instead of interpolating from
     * whatever was left in the buffers.
     */
    GroomSimulationStats StepGroomGuideSimulation(const GroomSimulationInputs& inputs,
                                                  GroomGuideSimulationState& state);

    /**
     * @brief Closest point on the segment AB to P, and the standard one.
     *
     * Exposed for the same reason MakeGroomSurfaceFrame is: a second, subtly
     * different capsule test written next to this one would make a strand snag
     * on a seam between two colliders that are geometrically flush.
     */
    [[nodiscard]] glm::vec3 ClosestPointOnGroomCollider(const GroomCollider& collider,
                                                        const glm::vec3& point) noexcept;
} // namespace OloEngine
