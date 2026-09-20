#pragma once

// =============================================================================
// AnimalScheduler.h — one frame's work, shared out among a population of
// animals. Issue #1258.
//
// WHAT THIS IS FOR, AND WHY IT IS NOT #1252.
//
// #1252 gave every groom a distance ladder: an animal that shrinks on screen
// asks for fewer strands, fewer guides and a coarser shadow. That decision is
// PER ENTITY and reads only that entity's apparent size, which is exactly right
// for one animal and says nothing at all about a herd. Forty animals at the
// same distance each independently conclude they deserve full rate, and the
// frame costs forty times one animal. Nothing in the engine arbitrates that
// today — not the groom LOD, not the mesh LOD, and not the gameplay scheduler,
// which runs every registered system to completion every tick.
//
// This file is the arbiter. It takes the per-entity answers as REQUESTS, prices
// them against a calibrated cost model, and spends a frame budget on them.
//
// THE BUDGET IS SPENT IN CALIBRATED UNITS, NOT IN A LIVE FRAME-TIME READING,
// and that is the central design decision here rather than an implementation
// convenience. A scheduler that reads the clock is not reproducible: the same
// scene, the same frame and the same camera produce a different allocation on a
// busy machine than on an idle one, so the population's trajectories stop being
// reproducible — which is criterion 1 — and every capture downstream becomes
// noise. Instead the cost model carries coefficients MEASURED on named hardware
// (docs/analysis/multi-animal-scheduling-budgets-1258.md), the schedule is a
// pure function of them, and drift between the model and the machine shows up
// as a counter (AnimalSchedulerStats::EstimatedCostUnits against the frame's
// measured cost) rather than as a silently different picture.
//
// THE THREE FAILURE MODES THE ISSUE NAMES ARE MECHANISMS HERE, NOT HOPES. Each
// is a rule in ScheduleAnimalPopulation and an assertion in
// AnimalSchedulerContractTest:
//
//   starvation            -- the service order is keyed on a per-axis
//                            starvation counter, so an animal passed over rises
//                            until it outranks its competitors and the loss
//                            ROTATES. The bound is RELATIVE — an animal held
//                            below its desired step while a peer of the same
//                            role sits at its own — because when the whole
//                            population must be coarsened there is nobody to
//                            swap with, and an absolute bound would be a
//                            promise no allocator can keep.
//   abrupt motion changes -- a step never moves by more than one halving per
//                            hold window, and an axis may never coarsen past
//                            the step whose pose displacement would exceed
//                            MaxPoseStepPixels on screen. A reduced tick rate
//                            is invisible only while the pose step is
//                            sub-pixel; past that it is judder, so the bound is
//                            screen-space and not a distance.
//   invisible distant coat -- MinVisibleStrands is a FLOOR the budget cannot
//                            cross. A coat that cannot be afforded at the floor
//                            hands over to a coarser representation or is
//                            reported; it never quietly thins to nothing.
//
// AND WHEN THE BUDGET CANNOT BE MET, THAT IS SAID OUT LOUD. Every non-hero at
// its cap and still over budget sets AnimalSchedulerStats::BudgetExceeded and
// leaves the hero at full rate. Degrading the hero silently to make a number
// fit would hide the one thing the budget exists to protect.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Math/Math.h"

#include <array>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace OloEngine
{

    // -------------------------------------------------------------------------
    // The axes
    // -------------------------------------------------------------------------

    // The four kinds of per-animal work this schedules, each spent separately.
    //
    // FOUR AXES AND NOT ONE NUMBER, for GroomLodPolicy's reason: they degrade
    // differently and they are noticed differently. A coat whose guides thin out
    // looks identical until the animal moves; a coat whose strands thin out is
    // visible standing still; a body whose skeleton ticks at quarter rate is
    // invisible at distance and judder up close. Collapsing them into one
    // "quality" scalar would make every one of those trade-offs unauthorable.
    enum class AnimalWorkAxis : u8
    {
        /// Skeletal pose evaluation — how OFTEN the body's animation is ticked.
        /// The one axis with no prior art in this engine: AnimationSystem::Update
        /// runs unconditionally for every skinned entity every frame today.
        Deformation = 0,
        /// Groom guide simulation — how many guides are integrated.
        Simulation,
        /// Strand ribbon geometry — how many strands are built and drawn.
        Visibility,
        /// Shadow work, as a BIAS on the technique's own resolution choice, for
        /// the reason GroomLodPolicy::Shadow states: a shadow spends a
        /// resolution rather than a count, so a step here is added to the
        /// technique's answer instead of replacing it.
        Shadow,
        Count
    };

    inline constexpr sizet AnimalWorkAxisCount = static_cast<sizet>(AnimalWorkAxis::Count);

    [[nodiscard]] constexpr std::string_view ToString(AnimalWorkAxis axis) noexcept
    {
        switch (axis)
        {
            case AnimalWorkAxis::Deformation:
                return "Deformation";
            case AnimalWorkAxis::Simulation:
                return "Simulation";
            case AnimalWorkAxis::Visibility:
                return "Visibility";
            case AnimalWorkAxis::Shadow:
                return "Shadow";
            case AnimalWorkAxis::Count:
                break;
        }
        return "Unknown";
    }

    [[nodiscard]] inline constexpr bool IsValidAnimalWorkAxis(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(AnimalWorkAxisCount);
    }

    // -------------------------------------------------------------------------
    // The roles
    // -------------------------------------------------------------------------

    // What an animal is FOR, which is what decides who gives way.
    //
    // AUTHORED, NOT DERIVED FROM DISTANCE. A hero is the animal the shot is
    // about, and it is still the hero when it walks away from camera — deriving
    // the role from apparent size would make "preserve hero quality" mean
    // "preserve the nearest animal's quality", which is a different and much
    // weaker promise. Ordered most-important-first so a plain comparison of the
    // underlying value ranks them.
    enum class AnimalRole : u8
    {
        /// Never coarsened while any other animal can still give way, and never
        /// below its authored floor at all. See ScheduleAnimalPopulation.
        Hero = 0,
        /// Coarsened only after every Background animal is at its cap.
        Featured,
        /// The herd. Gives way first.
        Background,
        Count
    };

    inline constexpr sizet AnimalRoleCount = static_cast<sizet>(AnimalRole::Count);

    [[nodiscard]] constexpr std::string_view ToString(AnimalRole role) noexcept
    {
        switch (role)
        {
            case AnimalRole::Hero:
                return "Hero";
            case AnimalRole::Featured:
                return "Featured";
            case AnimalRole::Background:
                return "Background";
            case AnimalRole::Count:
                break;
        }
        return "Unknown";
    }

    [[nodiscard]] inline constexpr bool IsValidAnimalRole(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(AnimalRoleCount);
    }

    // -------------------------------------------------------------------------
    // What happened to one animal
    // -------------------------------------------------------------------------

    // Why an animal is running where it is running. A RESULT, never a request.
    //
    // The distinction between Coarsened and CapHeld is the one worth reading:
    // Coarsened means the budget took work away and the animal can afford to
    // lose it, CapHeld means the budget WANTED to take more and a protection
    // rule refused. A frame full of CapHeld with BudgetExceeded set is the
    // signature of a population the budget cannot serve, and it is the honest
    // answer rather than a frame that quietly looks wrong.
    enum class AnimalBudgetOutcome : u8
    {
        /// Not scheduled: the policy is off, or this animal opted out.
        NotScheduled = 0,
        /// Running at exactly what its apparent size asked for.
        AtDesired,
        /// The budget coarsened at least one axis below the desired step.
        Coarsened,
        /// The budget asked for more and a floor, a pose-step bound or the hero
        /// rule refused. The work is being spent whether or not it fits.
        CapHeld,
        /// A coarsening is pending but the hold window has not elapsed, so this
        /// frame still runs the finer step. Named because a frame that is over
        /// budget purely through holds is a TRANSIENT, and telling it apart from
        /// a persistent over-subscription is the difference between tuning the
        /// hold and rebuilding the population.
        HeldByHysteresis,
        Count
    };

    inline constexpr sizet AnimalBudgetOutcomeCount = static_cast<sizet>(AnimalBudgetOutcome::Count);

    [[nodiscard]] constexpr std::string_view ToString(AnimalBudgetOutcome outcome) noexcept
    {
        switch (outcome)
        {
            case AnimalBudgetOutcome::NotScheduled:
                return "NotScheduled";
            case AnimalBudgetOutcome::AtDesired:
                return "AtDesired";
            case AnimalBudgetOutcome::Coarsened:
                return "Coarsened";
            case AnimalBudgetOutcome::CapHeld:
                return "CapHeld";
            case AnimalBudgetOutcome::HeldByHysteresis:
                return "HeldByHysteresis";
            case AnimalBudgetOutcome::Count:
                break;
        }
        return "Unknown";
    }

    // -------------------------------------------------------------------------
    // The cost model
    // -------------------------------------------------------------------------

    // What one unit of each axis costs, in CALIBRATED COST UNITS.
    //
    // One cost unit is one microsecond on the calibration machine, so a budget
    // reads naturally (a 4 ms slice is 4000 units) without the schedule ever
    // depending on a clock. The defaults are the measured coefficients from
    // AnimalSchedulingCensusTest on the hardware named in
    // docs/analysis/multi-animal-scheduling-budgets-1258.md; the census SHIPS as
    // a test rather than as a table in a PR body, so a coefficient that stops
    // describing the engine fails loudly instead of ageing quietly.
    //
    // DRAW CALLS ARE PRICED HERE ON PURPOSE, AND THAT IS CRITERION 2. The issue
    // instructs us not to assume draw calls are the limit. The only way to make
    // that instruction bite is to give draw calls a line in the model beside the
    // others and let the census say what share they carry — which, measured, is
    // the smallest of the five. A model that omitted them could not have been
    // wrong about them.
    struct AnimalCostModel
    {
        /// Per bone, per tick, for one skeletal pose evaluation.
        f32 DeformationPerBone = 0.42f;
        /// Per guide particle, per fixed substep.
        f32 SimulationPerGuidePoint = 0.31f;
        /// Per strand built into ribbon geometry.
        f32 VisibilityPerStrand = 0.0135f;
        /// Per shadow-volume voxel baked.
        f32 ShadowPerVoxel = 0.0009f;
        /// Per draw call submitted for this animal.
        f32 PerDrawCall = 6.5f;

        [[nodiscard]] auto operator==(const AnimalCostModel& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(AnimalCostModel) == 20,
                  "AnimalCostModel is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<AnimalCostModel>);

    /// Replaces every non-finite or negative coefficient with its default.
    /// Never rejects a model: a bad coefficient takes the calibrated value,
    /// because a cost model that silently became zero would price the whole
    /// population at nothing and the budget would never bind — a performance
    /// cliff with no symptom.
    [[nodiscard]] AnimalCostModel SanitizeAnimalCostModel(const AnimalCostModel& model) noexcept;

    // -------------------------------------------------------------------------
    // The authored policy
    // -------------------------------------------------------------------------

    // Everything the population budget is authored with. Arrives from scene
    // YAML, a save game, the editor or an MCP write, each of which is an
    // untrusted float per CLAUDE.md — so every consumer builds through
    // SanitizeAnimalBudgetPolicy and nothing reads these fields raw.
    struct AnimalBudgetPolicy
    {
        // MEMBERS ARE ORDERED 4-BYTE THEN 1-BYTE with the tail padded
        // explicitly, because operator== below is a whole-object memcmp and a
        // padded layout compares unequal when logically equal (issue #1019).
        // BitwiseEqualLayoutTest lists this type for exactly that reason.

        /// The whole population's per-frame allowance, in cost units.
        f32 FrameBudgetUnits = 6000.0f;

        /// How the allowance splits across the four axes. Normalised by
        /// SanitizeAnimalBudgetPolicy, so these are WEIGHTS and not fractions —
        /// an author who sets them to 2/1/1/1 gets the same schedule as one who
        /// writes 0.4/0.2/0.2/0.2, and neither can accidentally hand out 140%
        /// of the frame.
        ///
        /// PER AXIS RATHER THAN ONE POOL, so a population that is expensive to
        /// simulate cannot eat the allowance that keeps its silhouette. One pool
        /// makes the axes compete, and the axis that wins is whichever the cost
        /// model happens to price highest — which is a tuning accident, not a
        /// decision anybody made.
        std::array<f32, AnimalWorkAxisCount> AxisWeights{ 1.0f, 1.0f, 1.0f, 1.0f };

        /// Screen-space bound on a single pose update's displacement.
        ///
        /// THIS IS WHAT MAKES A REDUCED TICK RATE INVISIBLE RATHER THAN JUDDER.
        /// An animal ticked at 1/2^k rate moves 2^k times as far between poses;
        /// that is unnoticeable exactly while the displacement stays under about
        /// a pixel, and is a visible stutter as soon as it does not. So the cap
        /// on the Deformation axis is derived from the animal's own measured
        /// motion and its apparent size (AnimalWorkItem::MaxStep), not from a
        /// distance threshold that would be right for one gait and wrong for the
        /// next.
        f32 MaxPoseStepPixels = 1.0f;

        /// The fewest strands a VISIBLE coat may be built with.
        ///
        /// A floor and not a target. Below this a coat reads as a bald patch
        /// rather than as a cheaper coat, which is the "invisible distant coats"
        /// failure named in the issue — and it is invisible to the author too,
        /// because it only happens under budget pressure the authoring session
        /// never sees.
        u32 MinVisibleStrands = 256u;

        /// Consecutive frames a COARSER allocation must persist before it is
        /// taken. Refining is immediate, for GroomLodPolicy::HoldFrames' reason:
        /// an animal that just became important and is still cheap is a picture
        /// anyone can see, while one that stays expensive a few frames too long
        /// is merely expensive.
        u32 HoldFrames = 4u;

        /// The fairness bound: the most consecutive frames any one animal may
        /// spend below its desired step on an axis while another animal of the
        /// same or lower role sits at its desired step on that axis.
        ///
        /// Enforced by CONSTRUCTION rather than checked — the starvation counter
        /// is part of the service order, so a passed-over animal rises until it
        /// outranks its competitors. The number is what the contract test
        /// sweeps, and a population that cannot meet it sets
        /// AnimalSchedulerStats::BudgetExceeded rather than quietly missing it.
        u32 StarvationFrames = 8u;

        /// False leaves every animal exactly as #1252 left it: each one at
        /// whatever its own apparent size asks for, with no population
        /// arbitration at all. The A/B control for every capture is "turn this
        /// off", and it is the default so that no scene authored before this
        /// existed changes.
        bool Enabled = false;

        /// Whether the hero is protected absolutely. False lets the hero be
        /// coarsened alongside everyone else once the population is
        /// over-subscribed, which is the measured-and-rejected alternative kept
        /// as an authoring choice rather than deleted — see the analysis doc.
        bool ProtectHero = true;
        u8 Pad0 = 0;
        u8 Pad1 = 0;

        [[nodiscard]] auto operator==(const AnimalBudgetPolicy& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(AnimalBudgetPolicy) == 40,
                  "AnimalBudgetPolicy is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<AnimalBudgetPolicy>);

    /// Replaces every non-finite or out-of-range field with a usable value, and
    /// normalises AxisWeights to sum to one.
    ///
    /// Never rejects a policy. An all-zero or non-finite weight set takes an
    /// equal split rather than zero, because a zero weight on an axis means that
    /// axis gets no allowance at all — every animal pinned at its coarsest step
    /// forever, which looks like a broken renderer rather than a bad number.
    [[nodiscard]] AnimalBudgetPolicy SanitizeAnimalBudgetPolicy(const AnimalBudgetPolicy& policy) noexcept;

    // -------------------------------------------------------------------------
    // One animal's gathered inputs
    // -------------------------------------------------------------------------

    // Everything the scheduler knows about one animal this frame. Every field is
    // a RESULT the frame already computed, never a request: the desired steps
    // come from the per-entity ladders that already ran, and the counts come
    // from the assets that are already resolved.
    struct AnimalWorkItem
    {
        /// Stable identity, and the deterministic tie-break of last resort.
        /// A UUID rather than an entity handle because the service order must
        /// not change when EnTT recycles an id — an order that depends on
        /// allocation history is not reproducible, and reproducibility is
        /// criterion 1.
        UUID Id{ 0 };

        AnimalRole Role = AnimalRole::Background;

        /// Apparent size in pixels of the render target's height, from
        /// EstimateProjectedPixelSize against Renderer3D::GetLODViewParams() —
        /// the SAME metric every other LOD in this engine reads. An animal
        /// computing its own would be the one that disagreed.
        f32 PixelSize = 0.0f;

        /// What this animal's own ladder asked for, per axis, before any
        /// population arbitration. The scheduler may raise a step (coarsen) but
        /// never lowers one below this: the budget's job is to take work away
        /// when the frame cannot afford it, not to hand out work a distance
        /// ladder already said was pointless.
        std::array<u32, AnimalWorkAxisCount> DesiredStep{ 0u, 0u, 0u, 0u };

        /// The coarsest step each axis may take, after the floors:
        /// MinVisibleStrands on Visibility, MaxPoseStepPixels on Deformation,
        /// and the authored MaxSteps on the rest. Computed by the caller
        /// because only the caller holds the asset — see
        /// Scene::GatherAnimalWorkItems.
        std::array<u32, AnimalWorkAxisCount> MaxStep{ 0u, 0u, 0u, 0u };

        /// The work one full-rate frame would spend on each axis, in the cost
        /// model's own units: bones, guide points, strands, shadow voxels.
        /// A step of k costs 2^-k of these.
        u32 BoneCount = 0u;
        u32 GuidePointCount = 0u;
        u32 StrandCount = 0u;
        u32 ShadowVoxelCount = 0u;

        /// Draw calls this animal submits. Does NOT scale with any step — that
        /// is the point of pricing it (criterion 2): a budget that coarsens
        /// every axis to nothing still pays this, so if draw calls dominated,
        /// scheduling could not help and the answer would have to be batching.
        u32 DrawCallCount = 1u;

        /// Substeps the guide solver will run this frame, so the Simulation axis
        /// is priced at what it will actually integrate rather than at one step.
        u32 SimulationSubsteps = 1u;

        /// True when this animal is on screen. An off-screen animal is priced at
        /// its shadow and deformation cost only, and is exempt from
        /// MinVisibleStrands — the floor exists to stop a VISIBLE coat vanishing
        /// and would otherwise force strands nobody can see.
        bool Visible = true;
    };

    // -------------------------------------------------------------------------
    // The persistent state
    // -------------------------------------------------------------------------

    // One animal's scheduling memory, advanced exactly once per frame.
    //
    // KEYED BY UUID AND HELD BY Scene, not by a pass, for GroomLodState's
    // reason: a pass runs once per CAMERA and a hysteresis must advance once per
    // FRAME, so a split-screen scene would burn its hold twice as fast and the
    // starvation counters would advance at double rate against a budget that did
    // not.
    struct AnimalScheduleState
    {
        // 4-byte members first, the one-byte enum and its explicit padding last:
        // operator== is a whole-object memcmp (issue #1019).

        /// The step each axis is actually running at.
        std::array<u32, AnimalWorkAxisCount> Step{ 0u, 0u, 0u, 0u };

        /// The step the budget asked for, and how many consecutive frames it has
        /// asked for it. FOUR COUNTERS AND NOT ONE, for GroomLodState's reason:
        /// a shadow allocation that keeps flickering must not pin the
        /// deformation step at its old value, or the axes are coupled again
        /// through their hysteresis even though their budgets are separate.
        std::array<u32, AnimalWorkAxisCount> RequestedStep{ 0u, 0u, 0u, 0u };
        std::array<u32, AnimalWorkAxisCount> StableFrames{ 0u, 0u, 0u, 0u };

        /// Consecutive frames this animal has run BELOW its desired step on each
        /// axis. The service order reads this, which is the whole of the
        /// anti-starvation guarantee: a counter that only ever grew would be a
        /// diagnostic, one that is part of the sort is a mechanism.
        std::array<u32, AnimalWorkAxisCount> StarvedFrames{ 0u, 0u, 0u, 0u };

        AnimalBudgetOutcome Outcome = AnimalBudgetOutcome::NotScheduled;
        u8 Pad0 = 0;
        u8 Pad1 = 0;
        u8 Pad2 = 0;

        [[nodiscard]] auto operator==(const AnimalScheduleState& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(AnimalScheduleState) == 68,
                  "AnimalScheduleState is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<AnimalScheduleState>);

    // -------------------------------------------------------------------------
    // The decision
    // -------------------------------------------------------------------------

    struct AnimalSchedule
    {
        // 4-byte members first, the one-byte members and their explicit padding
        // last: operator== is a whole-object memcmp (issue #1019).

        /// Which animal this is for. Carried on the decision so a consumer can
        /// match it back without relying on the input order, which the
        /// scheduler's service sort does not preserve.
        UUID Id{ 0 };

        /// The step each axis runs at this frame, after budget and hysteresis.
        std::array<u32, AnimalWorkAxisCount> Step{ 0u, 0u, 0u, 0u };

        /// The coarsest step each axis was ALLOWED to take — the authored cap
        /// tightened by the floors (MinVisibleStrands on Visibility,
        /// MaxPoseStepPixels on Deformation).
        ///
        /// CARRIED ON THE DECISION because a consumer that combines this
        /// schedule with another ladder has to clamp against it. Spending
        /// `max(myLadderStep, scheduleStep)` and stopping there lets the OTHER
        /// ladder walk straight past the floor — a 1000-strand coat whose
        /// distance ladder asks for a sixty-fourth is built at 15 strands while
        /// MinVisibleStrands says 256, and every assertion inside the scheduler
        /// still passes because the scheduler never saw it. Clamp to this.
        std::array<u32, AnimalWorkAxisCount> MaxStep{ 0u, 0u, 0u, 0u };

        /// 2^-step per axis, precomputed so no consumer has to agree with any
        /// other about what a step means.
        std::array<f32, AnimalWorkAxisCount> Fraction{ 1.0f, 1.0f, 1.0f, 1.0f };

        /// What this animal is priced at, at the steps above. The sum over a
        /// frame's schedules is AnimalSchedulerStats::EstimatedCostUnits.
        f32 EstimatedCostUnits = 0.0f;

        /// The apparent size the decision was made at, carried so a panel can
        /// show the input beside the answer.
        f32 PixelSize = 0.0f;

        AnimalBudgetOutcome Outcome = AnimalBudgetOutcome::NotScheduled;
        AnimalRole Role = AnimalRole::Background;

        /// True on the ONE frame any axis's step actually changed. Carried on
        /// the decision rather than recomputed by each consumer, because the
        /// only thing that knows is the state this call just advanced — a
        /// consumer comparing "what I have now" against "what I had last frame"
        /// would be maintaining a second copy of the hysteresis, which is how
        /// two copies drift.
        bool StepChanged = false;

        // FOUR pad bytes and not one. UUID is a u64, so this struct aligns to 8
        // and the four one-byte members above would leave four bytes of TAIL
        // padding — which operator== below would read as part of the object.
        // Naming them is what keeps two logically equal schedules comparing
        // equal (issue #1019).
        u8 Pad0 = 0;
        u8 Pad1 = 0;
        u8 Pad2 = 0;
        u8 Pad3 = 0;
        u8 Pad4 = 0;

        [[nodiscard]] auto operator==(const AnimalSchedule& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(AnimalSchedule) == 72,
                  "AnimalSchedule is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<AnimalSchedule>);

    /// The identity schedule: what an animal gets when the policy is off. Full
    /// rate on every axis, forever — exactly the frame #1252 produced.
    [[nodiscard]] AnimalSchedule IdentityAnimalSchedule(UUID id) noexcept;

    // -------------------------------------------------------------------------
    // The frame's telemetry (criterion 4)
    // -------------------------------------------------------------------------

    struct AnimalSchedulerStats
    {
        u32 AnimalsConsidered = 0u;
        u32 AnimalsAtDesired = 0u;
        u32 AnimalsCoarsened = 0u;
        u32 AnimalsCapHeld = 0u;
        u32 AnimalsHeldByHysteresis = 0u;

        /// Per role, so "the hero was preserved" is a number rather than a
        /// claim: HeroesCoarsened non-zero with ProtectHero set is a bug, and
        /// the contract test asserts it stays zero.
        std::array<u32, AnimalRoleCount> ConsideredByRole{ 0u, 0u, 0u };
        std::array<u32, AnimalRoleCount> CoarsenedByRole{ 0u, 0u, 0u };

        /// Per axis: what was asked for, what was spent, and what was allowed.
        std::array<f32, AnimalWorkAxisCount> DesiredCostUnits{ 0.0f, 0.0f, 0.0f, 0.0f };
        std::array<f32, AnimalWorkAxisCount> ScheduledCostUnits{ 0.0f, 0.0f, 0.0f, 0.0f };
        std::array<f32, AnimalWorkAxisCount> AxisBudgetUnits{ 0.0f, 0.0f, 0.0f, 0.0f };

        /// The share of the scheduled cost each axis carries. THIS IS THE ANSWER
        /// TO CRITERION 2 and the reason draw calls are a line in the cost
        /// model: a reader can see which axis dominates instead of assuming.
        std::array<f32, AnimalWorkAxisCount> AxisShareOfScheduled{ 0.0f, 0.0f, 0.0f, 0.0f };

        /// Draw-call cost, which no step scales. Reported beside the axes rather
        /// than inside one, because it is the cost scheduling CANNOT reach — the
        /// census's "what the budget cannot touch" half.
        f32 DrawCallCostUnits = 0.0f;
        u32 DrawCalls = 0u;

        f32 EstimatedCostUnits = 0.0f;
        f32 FrameBudgetUnits = 0.0f;

        /// The worst starvation counter anywhere in the population this frame.
        ///
        /// A MEASURE OF PRESSURE, NOT OF UNFAIRNESS, and the distinction is
        /// worth stating because the obvious reading is wrong. Under a budget
        /// that cannot serve anybody at their desired step, EVERY animal is
        /// below desired every frame and this grows without bound — correctly,
        /// because there is nobody to rotate the loss onto.
        ///
        /// The fairness bound StarvationFrames states is RELATIVE: an animal
        /// held below its desired step while a peer of the same role sits at
        /// its own. That is what the service order guarantees and what
        /// AnimalSchedulerStarvation.NoAnimalIsPassedOverWhileAPeerSitsAtItsDesiredStep
        /// asserts. A large number here alongside BudgetExceeded is a
        /// population that has outgrown its budget, not a scheduler defect.
        u32 MaxStarvedFrames = 0u;

        /// Animals held at MinVisibleStrands — a coat the budget wanted to thin
        /// further and was refused. Non-zero is not an error; it is the floor
        /// doing its job, and it is the counter that says the population has
        /// outgrown the budget.
        u32 AnimalsAtVisibilityFloor = 0u;

        /// Animals whose deformation step was capped by MaxPoseStepPixels.
        u32 AnimalsAtPoseStepCap = 0u;

        /// Steps that changed this frame, summed over animals and axes. A
        /// counter that stays near AnimalsConsidered * 4 IS thrashing.
        u32 StepChanges = 0u;

        /// True when every animal that could give way is at its cap and the
        /// population still does not fit. The hero is left at full rate; this
        /// flag is how that is said out loud rather than absorbed.
        bool BudgetExceeded = false;

        // Explicit tail padding, for AnimalSchedule's reason: operator== is a
        // whole-object memcmp and would otherwise read three uninitialised
        // bytes (issue #1019).
        u8 Pad0 = 0;
        u8 Pad1 = 0;
        u8 Pad2 = 0;

        [[nodiscard]] auto operator==(const AnimalSchedulerStats& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(AnimalSchedulerStats) == 144,
                  "AnimalSchedulerStats is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<AnimalSchedulerStats>);

    // -------------------------------------------------------------------------
    // The scheduler
    // -------------------------------------------------------------------------

    /// Per-animal persistent state, looked up by the caller and handed in.
    /// A pointer per item rather than a map lookup inside, so the scheduler
    /// stays pure: it reads no clock, no camera, no registry and no global, and
    /// a test can drive a whole population's history without a Scene.
    struct AnimalScheduleSlot
    {
        AnimalWorkItem Item;
        AnimalScheduleState* State = nullptr;
    };

    /// Share this frame out among `slots` and advance every state by one frame.
    ///
    /// PURE GIVEN (policy, model, slots, states). `states` are in/out because the
    /// hysteresis and the starvation counters ARE the state — advancing them
    /// twice in a frame is exactly how a two-viewport scene would halve both.
    ///
    /// The policy and the model are sanitised on the way in, so a caller that
    /// reached here with raw authored numbers still cannot make the allocation
    /// non-monotone or price the population at zero.
    ///
    /// Returns one schedule per slot, in the slots' original order. `outStats`
    /// may be null.
    ///
    /// ORDER OF THE RULES, which is the whole algorithm:
    ///   1. Every animal starts at its DesiredStep.
    ///   2. While an axis is over its allowance, coarsen the animal that should
    ///      give way FIRST: the one with the coarsest-eligible role, then the
    ///      LOWEST starvation counter, then the smallest apparent size, then the
    ///      highest UUID. Every one of those four is deterministic; the UUID is
    ///      there so two identical animals cannot tie and make the order depend
    ///      on the gather order.
    ///   3. Never past MaxStep. An animal already at its cap is removed from the
    ///      candidates, and when the candidate set empties the axis stops trying
    ///      and sets BudgetExceeded.
    ///   4. A hero is a candidate only when ProtectHero is false, or when every
    ///      non-hero candidate is exhausted AND ProtectHero is false. With
    ///      ProtectHero set the hero is never a candidate at all.
    ///   5. The resulting step is run through the hold: a coarsening needs
    ///      HoldFrames consecutive frames of being asked for, refining is
    ///      immediate.
    [[nodiscard]] std::vector<AnimalSchedule> ScheduleAnimalPopulation(const AnimalBudgetPolicy& policy,
                                                                      const AnimalCostModel& model,
                                                                      std::span<const AnimalScheduleSlot> slots,
                                                                      AnimalSchedulerStats* outStats);

    /// What one animal costs at `steps`, in cost units. Exposed because the
    /// census test prices populations the scheduler never sees, and two copies
    /// of this arithmetic would drift.
    [[nodiscard]] f32 EstimateAnimalCostUnits(const AnimalCostModel& model, const AnimalWorkItem& item,
                                              std::span<const u32> steps) noexcept;

    /// The fraction of full rate a step runs at: 2^-step. Clamped at 30 so the
    /// shift is defined for any u32 a corrupt state could produce.
    [[nodiscard]] constexpr f32 AnimalStepFraction(u32 step) noexcept
    {
        const u32 clamped = step < 30u ? step : 30u;
        return 1.0f / static_cast<f32>(1u << clamped);
    }

    /// The coarsest deformation step whose pose displacement still lands under
    /// `maxPoseStepPixels` on screen.
    ///
    /// `fullRateMotionPixels` is how far this animal's fastest bone moves in one
    /// FULL-RATE frame, already projected to pixels. A step of k multiplies that
    /// by 2^k, so this is the largest k with `motion * 2^k <= bound` — and it is
    /// a bound on the PICTURE rather than on the distance, which is what makes
    /// it right for a sprinting animal and a grazing one at the same range.
    ///
    /// Returns 0 for a non-finite or non-positive bound: refusing to reduce the
    /// rate at all is the safe answer, because the failure of guessing wrong
    /// here is visible judder.
    [[nodiscard]] u32 MaxDeformationStepForPoseBound(f32 fullRateMotionPixels, f32 maxPoseStepPixels,
                                                     u32 authoredMaxStep) noexcept;

    /// The coarsest visibility step that still builds at least
    /// `minVisibleStrands` of `strandCount`.
    ///
    /// Returns 0 when the coat is ALREADY below the floor at full rate: a groom
    /// authored with fewer strands than the floor is not a budget failure and
    /// must not be forced up to it.
    [[nodiscard]] u32 MaxVisibilityStepForStrandFloor(u32 strandCount, u32 minVisibleStrands,
                                                      u32 authoredMaxStep) noexcept;

} // namespace OloEngine
