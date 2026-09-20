// OLO_TEST_LAYER: unit
//
// =============================================================================
// GroomGuideSimulationTest.cpp — the contracts of issue #1250's solver.
//
// WHAT THIS FILE IS FOR. Acceptance criterion 1 is stated as a TOLERANCE — "
// guides preserve length/rest shape within declared tolerances across variable
// frame rate, pause/resume and teleport" — so it is a number, and this is where
// the number is taken. Every test here is a measurement against
// GroomSimulationParams::StretchTolerance, not an eyeball.
//
// It is also the file the issue's "prototype and measure a solver before
// committing to one" instruction is discharged in: SolverModelComparison runs
// all three models over the same motion and prints the length error each one
// leaves, which is the table in docs/analysis/groom-guide-simulation-1250.md.
// A measurement nobody can re-run is a claim, so the comparison is a test rather
// than a paragraph.
// =============================================================================

#include "OloEngine/Groom/GroomGuideSimulation.h"

#include "OloEngine/Math/Math.h"

#include <gtest/gtest.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    /// Straight guides of `pointsPerGuide` particles, laid along `direction`
    /// from evenly spaced roots, with a uniform segment length.
    ///
    /// The DIRECTION is a parameter, and that is not decoration. A chain laid
    /// along -Y is laid along gravity, and a length projection then puts every
    /// particle straight back where gravity took it from -- so the chain is
    /// EXACTLY at its rest shape, forever, and a test written on it measures
    /// nothing while looking like it measures everything. Every case that needs
    /// gravity to do work lays the chain across it.
    struct TestGuides
    {
        std::vector<u32> Offsets;
        std::vector<u32> Curves;
        std::vector<glm::vec3> Targets;

        static TestGuides Make(u32 guideCount, u32 pointsPerGuide, f32 segment = 0.1f,
                               glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f))
        {
            TestGuides g;
            g.Offsets.push_back(0u);
            for (u32 guide = 0; guide < guideCount; ++guide)
            {
                g.Curves.push_back(guide);
                const glm::vec3 root{ 0.0f, 0.0f, static_cast<f32>(guide) * 0.5f };
                for (u32 i = 0; i < pointsPerGuide; ++i)
                {
                    g.Targets.push_back(root + direction * (static_cast<f32>(i) * segment));
                }
                g.Offsets.push_back(static_cast<u32>(g.Targets.size()));
            }
            return g;
        }

        /// Move every target rigidly — the body walking, seen from the guides.
        void Translate(const glm::vec3& delta)
        {
            for (glm::vec3& p : Targets)
            {
                p += delta;
            }
        }

        [[nodiscard]] GroomSimulationInputs Inputs(const GroomSimulationParams& params, f32 dt,
                                                   bool hasHistory) const
        {
            GroomSimulationInputs inputs;
            inputs.GuideOffsets = Offsets;
            inputs.GuideCurves = Curves;
            inputs.TargetPoints = Targets;
            inputs.Params = params;
            inputs.DeltaTime = dt;
            inputs.HasHistory = hasHistory;
            return inputs;
        }
    };

    /// The worst |segment| / restLength over a solved state, measured against
    /// the targets that defined the rest lengths.
    [[nodiscard]] f32 WorstStretch(const GroomGuideSimulationState& state, const TestGuides& guides)
    {
        f32 worst = 1.0f;
        for (u32 g = 0; g + 1u < guides.Offsets.size(); ++g)
        {
            for (u32 i = guides.Offsets[g] + 1u; i < guides.Offsets[g + 1u]; ++i)
            {
                const f32 rest = glm::length(guides.Targets[i] - guides.Targets[i - 1u]);
                if (!(rest > 0.0f))
                {
                    continue;
                }
                const f32 ratio = glm::length(state.Curr[i] - state.Curr[i - 1u]) / rest;
                if (std::abs(ratio - 1.0f) > std::abs(worst - 1.0f))
                {
                    worst = ratio;
                }
            }
        }
        return worst;
    }

    /// Run `seconds` of simulation at a constant frame rate, with the body
    /// swinging left and right so the guides are genuinely in motion. Returns
    /// the last frame's stats and the worst stretch seen over the whole run —
    /// a solver that is inextensible only at the end is not inextensible.
    struct RunResult
    {
        GroomSimulationStats Last;
        f32 WorstStretchEver = 1.0f;
        f32 WorstDeviationEver = 0.0f;
        u32 Frames = 0;
    };

    RunResult RunMotion(TestGuides guides, const GroomSimulationParams& params, f32 frameDt, f32 seconds,
                        GroomGuideSimulationState& state, f32 swingAmplitude = 0.4f)
    {
        RunResult result;
        // Seed frame: HasHistory false, so the state snaps to the targets.
        (void)StepGroomGuideSimulation(guides.Inputs(params, frameDt, false), state);

        const TestGuides base = guides;
        f32 time = 0.0f;
        while (time < seconds)
        {
            time += frameDt;
            ++result.Frames;
            // A sine sway of the whole body. Smooth, so nothing here is a
            // teleport, and fast enough that inertia is doing real work.
            guides = base;
            guides.Translate(glm::vec3(swingAmplitude * std::sin(time * 6.0f), 0.0f, 0.0f));
            result.Last = StepGroomGuideSimulation(guides.Inputs(params, frameDt, true), state);
            result.WorstStretchEver =
                std::abs(result.Last.MaxStretchRatio - 1.0f) > std::abs(result.WorstStretchEver - 1.0f)
                    ? result.Last.MaxStretchRatio
                    : result.WorstStretchEver;
            result.WorstDeviationEver = std::max(result.WorstDeviationEver, result.Last.MaxRestDeviation);
        }
        return result;
    }
} // namespace

// -----------------------------------------------------------------------------
// Rest shape
// -----------------------------------------------------------------------------

// THE baseline claim of the whole feature: with no gravity, no motion and no
// collider the simulated coat IS the groomed coat. Not approximately — the
// re-seed assigns the targets, and every subsequent step's forces are exactly
// zero, so the particles never leave them. A feature whose "off" state is not
// bit-identical to the previous renderer cannot have its captures compared.
TEST(GroomGuideSimulation, UndisturbedGuidesStayExactlyAtRest)
{
    TestGuides guides = TestGuides::Make(3, 8);
    GroomSimulationParams params;
    params.Gravity = glm::vec3(0.0f);
    params.CollisionEnabled = false;

    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, false), state);
    for (u32 frame = 0; frame < 120; ++frame)
    {
        const GroomSimulationStats stats = StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, true), state);
        ASSERT_FALSE(stats.Refused);
        EXPECT_EQ(stats.MaxRestDeviation, 0.0f) << "frame " << frame;
    }
    for (sizet i = 0; i < guides.Targets.size(); ++i)
    {
        EXPECT_EQ(state.Curr[i], guides.Targets[i]) << "particle " << i;
    }
}

// Gravity pulls a guide off its groom, and the stiffness pulls it back. The
// steady state is a BOUNDED deviation, not a hanging chain: that bound is what
// distinguishes fur from hair, and it is the rest-shape half of criterion 1.
TEST(GroomGuideSimulation, StiffnessBoundsTheDeviationFromTheGroom)
{
    // HORIZONTAL, so gravity is perpendicular to the chain and can actually
    // deflect it -- see TestGuides::Make. A chain along -Y is a chain along
    // gravity, and the length projection restores it exactly every step.
    TestGuides guides = TestGuides::Make(1, 12, 0.1f, glm::vec3(1.0f, 0.0f, 0.0f));
    GroomSimulationParams params;
    params.CollisionEnabled = false;
    params.Stiffness = 400.0f;
    params.Damping = 12.0f;

    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, false), state);
    GroomSimulationStats stats;
    for (u32 frame = 0; frame < 600; ++frame) // ten seconds: long past settling
    {
        stats = StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, true), state);
    }
    ASSERT_FALSE(stats.Refused);

    // g / k is the analytic steady-state offset of a critically-held particle
    // under a constant acceleration. The chain is series-coupled, so the tip
    // sees more than one segment's worth; a factor of the point count bounds it
    // without pinning an arbitrary number.
    const f32 analyticPerParticle = 9.81f / params.Stiffness;
    EXPECT_LT(stats.MaxRestDeviation, analyticPerParticle * 12.0f)
        << "a stiff coat must hold its groom, not hang from it";
    EXPECT_GT(stats.MaxRestDeviation, 0.0f) << "gravity must actually do something";
}

// -----------------------------------------------------------------------------
// Length, across the frame rates the criterion names
// -----------------------------------------------------------------------------

TEST(GroomGuideSimulation, LengthHoldsAcrossFrameRates)
{
    GroomSimulationParams params;
    params.CollisionEnabled = false;
    const f32 tolerance = params.StretchTolerance;

    for (const f32 hz : { 30.0f, 60.0f, 144.0f })
    {
        GroomGuideSimulationState state;
        const RunResult run = RunMotion(TestGuides::Make(4, 10), params, 1.0f / hz, 2.0f, state);
        ASSERT_FALSE(run.Last.Refused) << hz << " Hz";
        ASSERT_GT(run.Frames, 0u);
        EXPECT_LE(std::abs(run.WorstStretchEver - 1.0f), tolerance)
            << hz << " Hz: worst stretch ratio " << run.WorstStretchEver;
    }
}

// A frame spike is the case an explicit integrator with a raw dt detonates on,
// and the case the bounded catch-up exists for. The contract is that length
// survives it AND that the drop is REPORTED — a coat permanently in arrears
// lags the body by a constant offset, which reads as a binding error.
TEST(GroomGuideSimulation, LengthSurvivesAFrameSpikeAndTheDropIsReported)
{
    TestGuides guides = TestGuides::Make(2, 12);
    GroomSimulationParams params;
    params.CollisionEnabled = false;

    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, false), state);
    for (u32 frame = 0; frame < 30; ++frame)
    {
        guides.Translate(glm::vec3(0.01f, 0.0f, 0.0f));
        (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, true), state);
    }

    // Half a second in one frame: eight times the catch-up bound at the default
    // four substeps.
    guides.Translate(glm::vec3(0.02f, 0.0f, 0.0f));
    const GroomSimulationStats spike = StepGroomGuideSimulation(guides.Inputs(params, 0.5f, true), state);
    ASSERT_FALSE(spike.Refused);
    EXPECT_TRUE(spike.StepsClamped) << "dropped time must be reported, never silently integrated";
    EXPECT_LE(spike.StepsTaken, params.MaxSubsteps);
    EXPECT_LE(std::abs(spike.MaxStretchRatio - 1.0f), params.StretchTolerance);
    EXPECT_LE(std::abs(WorstStretch(state, guides) - 1.0f), params.StretchTolerance);
}

// -----------------------------------------------------------------------------
// Pause / resume
// -----------------------------------------------------------------------------

// dt == 0 is PAUSE and is a legal, meaningful value: no steps run, the state is
// held exactly, and resume continues from the pose the pause froze. Asserted
// rather than assumed because a criterion met by accident is one that regresses.
TEST(GroomGuideSimulation, PauseHoldsTheStateAndResumeContinuesFromIt)
{
    TestGuides guides = TestGuides::Make(2, 10);
    GroomSimulationParams params;
    params.CollisionEnabled = false;

    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, false), state);
    for (u32 frame = 0; frame < 20; ++frame)
    {
        guides.Translate(glm::vec3(0.02f, 0.0f, 0.0f));
        (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, true), state);
    }

    const std::vector<glm::vec3> beforePause = state.Curr;
    const std::vector<glm::vec3> prevBeforePause = state.Prev;
    const f32 accumulatorBeforePause = state.Accumulator;

    for (u32 frame = 0; frame < 10; ++frame)
    {
        const GroomSimulationStats paused = StepGroomGuideSimulation(guides.Inputs(params, 0.0f, true), state);
        ASSERT_FALSE(paused.Refused);
        EXPECT_FALSE(paused.Reseeded) << "a pause is not a discontinuity";
        EXPECT_EQ(paused.StepsTaken, 0u);
    }

    EXPECT_EQ(state.Curr, beforePause);
    EXPECT_EQ(state.Prev, prevBeforePause);
    EXPECT_EQ(state.Accumulator, accumulatorBeforePause);

    // And the resumed frame integrates, rather than re-seeding.
    const GroomSimulationStats resumed = StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, true), state);
    EXPECT_FALSE(resumed.Reseeded);
    EXPECT_GT(resumed.StepsTaken, 0u);
}

// -----------------------------------------------------------------------------
// Teleport
// -----------------------------------------------------------------------------

// The interesting one. A teleport must RESET, not stretch: the frame after a
// cut draws the groomed coat and emits zero motion, rather than a coat whose
// tips are still at the old level and whose length constraint is now resolving
// a hundred-metre segment.
TEST(GroomGuideSimulation, TeleportReseedsRatherThanStretching)
{
    TestGuides guides = TestGuides::Make(2, 10);
    GroomSimulationParams params;
    params.CollisionEnabled = false;

    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, false), state);
    for (u32 frame = 0; frame < 30; ++frame)
    {
        guides.Translate(glm::vec3(0.02f, 0.0f, 0.0f));
        (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, true), state);
    }

    // A hundred metres in one frame. The caller is what decides this is a cut;
    // the solver obeys HasHistory == false.
    guides.Translate(glm::vec3(100.0f, 0.0f, 0.0f));
    const GroomSimulationStats cut = StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, false), state);
    ASSERT_FALSE(cut.Refused);
    EXPECT_TRUE(cut.Reseeded);
    EXPECT_EQ(cut.StepsTaken, 0u) << "the re-seeding frame must not also integrate a frame of gravity";
    EXPECT_EQ(cut.MaxRestDeviation, 0.0f);
    EXPECT_EQ(state.Accumulator, 0.0f);
    for (sizet i = 0; i < guides.Targets.size(); ++i)
    {
        EXPECT_EQ(state.Curr[i], guides.Targets[i]) << "particle " << i;
        EXPECT_EQ(state.Prev[i], guides.Targets[i]) << "particle " << i << " must emit zero motion";
    }
}

// The failure this is written against: if the SAME teleport is fed through with
// HasHistory true — i.e. the caller's threshold missed it — the length
// constraint must still hold. The coat is wrong (it lags), but it is not a
// hundred-metre segment being resolved into the geometry buffer.
TEST(GroomGuideSimulation, AMissedTeleportStillPreservesLength)
{
    TestGuides guides = TestGuides::Make(2, 10);
    GroomSimulationParams params;
    params.CollisionEnabled = false;

    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, false), state);
    guides.Translate(glm::vec3(100.0f, 0.0f, 0.0f));
    const GroomSimulationStats stats = StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, true), state);
    ASSERT_FALSE(stats.Refused);
    EXPECT_FALSE(stats.Reseeded);
    EXPECT_LE(std::abs(stats.MaxStretchRatio - 1.0f), params.StretchTolerance)
        << "a missed cut must lag, never stretch";
}

// -----------------------------------------------------------------------------
// The root is kinematic
// -----------------------------------------------------------------------------

// A simulated root is a coat that detaches from the animal — the one failure
// this feature must never produce. The root is where the body put it, exactly.
TEST(GroomGuideSimulation, RootsAreExactlyWhereTheBodyPutThem)
{
    TestGuides guides = TestGuides::Make(3, 8);
    GroomSimulationParams params;
    params.Stiffness = 0.0f; // free hair: nothing pulls a particle back at all
    params.CollisionEnabled = false;

    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, false), state);
    for (u32 frame = 0; frame < 120; ++frame)
    {
        guides.Translate(glm::vec3(0.05f, 0.0f, 0.0f));
        (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, true), state);
        for (u32 g = 0; g + 1u < guides.Offsets.size(); ++g)
        {
            const u32 root = guides.Offsets[g];
            EXPECT_EQ(state.Curr[root], guides.Targets[root]) << "guide " << g << " frame " << frame;
        }
    }
}

// -----------------------------------------------------------------------------
// Collision
// -----------------------------------------------------------------------------

TEST(GroomGuideSimulation, ClosestPointHandlesSphereCapsuleAndTheCaps)
{
    GroomCollider sphere;
    sphere.PointA = glm::vec3(1.0f, 2.0f, 3.0f);
    sphere.PointB = sphere.PointA;
    sphere.Radius = 0.5f;
    EXPECT_EQ(ClosestPointOnGroomCollider(sphere, glm::vec3(5.0f, 5.0f, 5.0f)), sphere.PointA);

    GroomCollider capsule;
    capsule.PointA = glm::vec3(0.0f);
    capsule.PointB = glm::vec3(0.0f, 1.0f, 0.0f);
    capsule.Radius = 0.25f;
    // Beside the middle: the projection.
    EXPECT_EQ(ClosestPointOnGroomCollider(capsule, glm::vec3(3.0f, 0.5f, 0.0f)), glm::vec3(0.0f, 0.5f, 0.0f));
    // Past each cap: clamped to the ends, never extrapolated down the axis.
    EXPECT_EQ(ClosestPointOnGroomCollider(capsule, glm::vec3(0.0f, -9.0f, 0.0f)), capsule.PointA);
    EXPECT_EQ(ClosestPointOnGroomCollider(capsule, glm::vec3(0.0f, 9.0f, 0.0f)), capsule.PointB);
}

// Criterion 2, as a number: no particle ends the frame conspicuously inside the
// proxy. "Conspicuously" is the tolerance the solver's own trade declares —
// collision is resolved BEFORE the length projection, so a particle may end a
// step a fraction of a SEGMENT inside, and the shell is what buys that back.
TEST(GroomGuideSimulation, CollisionKeepsGuidesOutOfTheBody)
{
    // HORIZONTAL guides, sagging under gravity onto a capsule below them.
    //
    // Not a chain hanging down through a capsule, which was the first shape
    // tried and is a trap twice over: gravity cannot deflect a chain laid along
    // it, and a capsule whose axis passes THROUGH the chain pushes each particle
    // along the chain rather than off it, so the resolution is invisible in
    // every measurement. A beam sagging onto an obstacle is the arrangement in
    // which "did the proxy catch it" has an answer.
    TestGuides guides = TestGuides::Make(4, 14, 0.08f, glm::vec3(1.0f, 0.0f, 0.0f));

    GroomCollider capsule;
    capsule.PointA = glm::vec3(0.6f, -0.18f, -1.0f);
    capsule.PointB = glm::vec3(0.6f, -0.18f, 3.0f);
    capsule.Radius = 0.2f;
    const std::vector<GroomCollider> colliders{ capsule };

    GroomSimulationParams params;
    params.CollisionEnabled = true;
    params.ColliderPadding = 0.01f;
    params.Stiffness = 20.0f; // weak, so gravity really drives them into the limb

    GroomGuideSimulationState state;
    {
        GroomSimulationInputs seed = guides.Inputs(params, 1.0f / 60.0f, false);
        seed.Colliders = colliders;
        (void)StepGroomGuideSimulation(seed, state);
    }

    GroomSimulationStats stats;
    // Counted over the WHOLE run, not read off the last frame. A settled strand
    // resting exactly ON the shell resolves nothing on the frame you happen to
    // look at while having been in contact for two hundred before it, and the
    // claim being made is about the run.
    u32 contactsEver = 0;
    for (u32 frame = 0; frame < 240; ++frame)
    {
        GroomSimulationInputs inputs = guides.Inputs(params, 1.0f / 60.0f, true);
        inputs.Colliders = colliders;
        stats = StepGroomGuideSimulation(inputs, state);
        contactsEver += stats.ContactsResolved;
    }
    ASSERT_FALSE(stats.Refused);
    EXPECT_GT(contactsEver, 0u) << "the guides must actually reach the capsule";

    // The declared bound: one segment's worth of length projection, plus the
    // shell. Stated as the arithmetic rather than as a magic number so a change
    // to either input moves the bound with it.
    const f32 allowed = 0.08f + params.ColliderPadding;
    f32 worstPenetration = 0.0f;
    for (const glm::vec3& p : state.Curr)
    {
        const glm::vec3 nearest = ClosestPointOnGroomCollider(capsule, p);
        worstPenetration = std::max(worstPenetration, capsule.Radius - glm::length(p - nearest));
    }
    EXPECT_LT(worstPenetration, allowed) << "worst penetration " << worstPenetration;

    // And length still holds while colliding, which is the trade being asserted
    // rather than assumed.
    EXPECT_LE(std::abs(stats.MaxStretchRatio - 1.0f), params.StretchTolerance);
}

TEST(GroomGuideSimulation, CollisionDisabledLetsGuidesPassThroughAndIsCountedAsZero)
{
    TestGuides guides = TestGuides::Make(1, 14, 0.08f);
    GroomCollider capsule;
    capsule.PointA = glm::vec3(-1.0f, -0.55f, 0.0f);
    capsule.PointB = glm::vec3(3.0f, -0.55f, 0.0f);
    capsule.Radius = 0.2f;
    const std::vector<GroomCollider> colliders{ capsule };

    GroomSimulationParams params;
    params.CollisionEnabled = false;
    params.Stiffness = 20.0f;

    GroomGuideSimulationState state;
    {
        GroomSimulationInputs seed = guides.Inputs(params, 1.0f / 60.0f, false);
        seed.Colliders = colliders;
        (void)StepGroomGuideSimulation(seed, state);
    }
    GroomSimulationStats stats;
    for (u32 frame = 0; frame < 240; ++frame)
    {
        GroomSimulationInputs inputs = guides.Inputs(params, 1.0f / 60.0f, true);
        inputs.Colliders = colliders;
        stats = StepGroomGuideSimulation(inputs, state);
    }
    EXPECT_EQ(stats.ContactsResolved, 0u);
}

// -----------------------------------------------------------------------------
// The step-dependent stiffness ceiling
// -----------------------------------------------------------------------------

// The combination this exists for is INSIDE every documented bound and was
// unstable anyway: the slowest legal step (15 Hz) with the highest legal
// stiffness (2000) gives Stiffness * dt^2 = 8.9, and the undamped transverse
// mode leaves the unit circle past 4.
//
// The Follow-the-Leader projection does not rescue it, which is what makes this
// worth a test rather than a comment: length stays EXACT while the direction
// oscillates, so every length assertion in this file passes while the coat
// never settles. The symptom is a rest deviation that does not decay.
TEST(GroomGuideSimulation, TheSlowestStepWithTheStiffestCoatStillSettles)
{
    // Horizontal, so gravity can deflect it and there is something to settle.
    const TestGuides base = TestGuides::Make(1, 12, 0.1f, glm::vec3(1.0f, 0.0f, 0.0f));

    GroomSimulationParams params;
    params.CollisionEnabled = false;
    params.FixedHz = GroomSimulationLimits::MinFixedHz;     // 15
    params.Stiffness = GroomSimulationLimits::MaxStiffness; // 2000
    params.Damping = GroomSimulationLimits::MaxDamping;     // 60 -> velocityRetain 0

    TestGuides guides = base;
    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 15.0f, false), state);

    // Let it run well past any settling time, then measure whether the LAST
    // second still moves. A settled coat is nearly still; an oscillating one is
    // not, however exact its segment lengths are.
    f32 lateTravel = 0.0f;
    glm::vec3 previousTip = state.Curr.back();
    for (u32 frame = 0; frame < 300u; ++frame)
    {
        const GroomSimulationStats stats =
            StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 15.0f, true), state);
        ASSERT_FALSE(stats.Refused) << "frame " << frame;
        // Length must hold throughout, which it does with or without the fix --
        // that is precisely why length cannot be the detector here.
        EXPECT_LE(std::abs(stats.MaxStretchRatio - 1.0f), params.StretchTolerance) << "frame " << frame;
        if (frame >= 285u)
        {
            lateTravel += glm::length(state.Curr.back() - previousTip);
        }
        previousTip = state.Curr.back();
    }

    std::printf("[solver] 15 Hz / stiffness 2000 / damping 60: tip travel over the last second = %.6f m\n",
                static_cast<f64>(lateTravel));

    // MEASURED, by disabling the clamp and re-running this case:
    //
    //   clamp disabled                     27.260134 m
    //   MaxStiffnessTimesStepSquared = 2    0.106383 m   (marginal: e' = -e)
    //   MaxStiffnessTimesStepSquared = 1    0.000000 m
    //
    // Three orders of magnitude between the bound and the failure, so it does
    // not need to be delicate. The middle row is why the constant is 1 and not
    // 2: at exactly 2 with zero velocity retention the error flips sign every
    // step and never decays, which is marginal stability wearing stability's
    // clothes. Every length assertion above passed in ALL THREE runs.
    EXPECT_LT(lateTravel, 0.05f)
        << "the coat is still oscillating after twenty seconds: Stiffness * dt^2 is past the "
           "explicit integrator's stability limit and the length projection is hiding it";

    for (const glm::vec3& p : state.Curr)
    {
        EXPECT_TRUE(Math::IsFinite(p));
    }
}

// The clamp is a function of the STEP, so it must not touch a configuration
// that is already stable. At 60 Hz the ceiling is 7200 and the authored maximum
// is 2000, so a fast-stepping coat keeps exactly the stiffness it asked for.
TEST(GroomGuideSimulation, TheStiffnessCeilingDoesNotBindAtSixtyHertz)
{
    const TestGuides base = TestGuides::Make(1, 10, 0.1f, glm::vec3(1.0f, 0.0f, 0.0f));

    GroomSimulationParams stiff;
    stiff.CollisionEnabled = false;
    stiff.FixedHz = 60.0f;
    stiff.Stiffness = GroomSimulationLimits::MaxStiffness;

    // A coat this stiff barely leaves its groom under gravity: g / k is 5 mm.
    TestGuides guides = base;
    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(stiff, 1.0f / 60.0f, false), state);
    GroomSimulationStats stats;
    for (u32 frame = 0; frame < 240u; ++frame)
    {
        stats = StepGroomGuideSimulation(guides.Inputs(stiff, 1.0f / 60.0f, true), state);
    }
    ASSERT_FALSE(stats.Refused);
    // Had the ceiling bound here it would have cut 2000 to something far lower
    // and the coat would sag visibly further than the analytic g/k bound.
    EXPECT_LT(stats.MaxRestDeviation, 9.81f / 1000.0f * 12.0f)
        << "the 60 Hz ceiling must not be clamping an already-stable stiffness";
}

// -----------------------------------------------------------------------------
// Determinism and refusal
// -----------------------------------------------------------------------------

// The property every headless assertion in this file rests on, and the property
// a capture compared against a committed PNG rests on too.
TEST(GroomGuideSimulation, IsDeterministic)
{
    GroomSimulationParams params;
    GroomGuideSimulationState a;
    GroomGuideSimulationState b;
    const RunResult runA = RunMotion(TestGuides::Make(5, 11), params, 1.0f / 60.0f, 1.0f, a);
    const RunResult runB = RunMotion(TestGuides::Make(5, 11), params, 1.0f / 60.0f, 1.0f, b);
    EXPECT_EQ(a.Curr, b.Curr);
    EXPECT_EQ(a.Prev, b.Prev);
    EXPECT_EQ(runA.Last, runB.Last);
}

// A non-finite parameter REFUSES the whole solve and CLEARS the state. Keeping
// the buffers would leave the caller interpolating a coat from particles that
// belong to a different frame's guide set — a plausible wrong coat, which is
// exactly what every refusal in this subsystem is written to avoid.
TEST(GroomGuideSimulation, NonFiniteParametersRefuseAndClear)
{
    TestGuides guides = TestGuides::Make(2, 8);
    GroomSimulationParams params;
    GroomGuideSimulationState state;
    (void)StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, false), state);
    ASSERT_TRUE(state.Initialized);

    params.Stiffness = std::numeric_limits<f32>::quiet_NaN();
    const GroomSimulationStats stats = StepGroomGuideSimulation(guides.Inputs(params, 1.0f / 60.0f, true), state);
    EXPECT_TRUE(stats.Refused);
    EXPECT_FALSE(state.Initialized);
    EXPECT_TRUE(state.Curr.empty());
}

TEST(GroomGuideSimulation, AMalformedOffsetTableIsRefused)
{
    TestGuides guides = TestGuides::Make(2, 8);
    guides.Offsets.back() = guides.Offsets.back() + 5u; // past the point array
    GroomGuideSimulationState state;
    const GroomSimulationStats stats =
        StepGroomGuideSimulation(guides.Inputs(GroomSimulationParams{}, 1.0f / 60.0f, false), state);
    EXPECT_TRUE(stats.Refused);
}

// A guide set that CHANGES — the budget moved — is a discontinuity even when the
// caller says it has history, because particle 7 is now a different strand.
TEST(GroomGuideSimulation, AChangedGuideSetReseedsEvenWithHistory)
{
    GroomSimulationParams params;
    GroomGuideSimulationState state;
    TestGuides four = TestGuides::Make(4, 8);
    (void)StepGroomGuideSimulation(four.Inputs(params, 1.0f / 60.0f, false), state);
    (void)StepGroomGuideSimulation(four.Inputs(params, 1.0f / 60.0f, true), state);

    TestGuides two = TestGuides::Make(2, 8);
    const GroomSimulationStats stats = StepGroomGuideSimulation(two.Inputs(params, 1.0f / 60.0f, true), state);
    EXPECT_TRUE(stats.Reseeded);
    EXPECT_EQ(stats.GuidesSimulated, 2u);
}

// A groom with no guides is not a refusal: it is a coat that is deliberately not
// simulated, and the caller draws the groomed rest shape it drew before #1250.
TEST(GroomGuideSimulation, NoGuidesIsNotARefusal)
{
    GroomGuideSimulationState state;
    GroomSimulationInputs inputs;
    static constexpr u32 kEmpty[] = { 0u };
    inputs.GuideOffsets = kEmpty;
    inputs.DeltaTime = 1.0f / 60.0f;
    const GroomSimulationStats stats = StepGroomGuideSimulation(inputs, state);
    EXPECT_FALSE(stats.Refused);
    EXPECT_EQ(stats.GuidesSimulated, 0u);
}

// -----------------------------------------------------------------------------
// The measurement the issue asked for
// -----------------------------------------------------------------------------

// The issue's instruction was to prototype and measure a solver before
// committing to one, and to rescope on the evidence if an approach is rejected.
// This is that measurement, kept as a test so it can be re-run: all three models
// over the same motion at three frame rates, reporting the length error each
// one leaves. The table it prints is the one in
// docs/analysis/groom-guide-simulation-1250.md.
//
// The ASSERTION is only on the two projection models, because that is what the
// evidence supports: an exact-length projection is inextensible at any step,
// and PositionBasedDistance is not — its number is RECORDED, and recording a
// number a solver fails is the point of keeping it.
TEST(GroomGuideSimulation, SolverModelComparison)
{
    struct Row
    {
        GroomSolverModel Model;
        f32 Hz;
        f32 WorstStretch;
        f32 WorstDeviation;
    };
    std::vector<Row> rows;

    for (const GroomSolverModel model : { GroomSolverModel::FollowTheLeader,
                                          GroomSolverModel::DynamicFollowTheLeader,
                                          GroomSolverModel::PositionBasedDistance })
    {
        for (const f32 hz : { 30.0f, 60.0f, 144.0f })
        {
            GroomSimulationParams params;
            params.Model = model;
            params.CollisionEnabled = false;
            GroomGuideSimulationState state;
            const RunResult run = RunMotion(TestGuides::Make(4, 12), params, 1.0f / hz, 2.0f, state, 0.8f);
            ASSERT_FALSE(run.Last.Refused);
            rows.push_back(Row{ model, hz, run.WorstStretchEver, run.WorstDeviationEver });
        }
    }

    for (const Row& row : rows)
    {
        std::printf("[solver] %-24s %6.1f Hz  worst |stretch-1| = %.6f  worst rest deviation = %.6f m\n",
                    std::string(ToString(row.Model)).c_str(), static_cast<f64>(row.Hz),
                    static_cast<f64>(std::abs(row.WorstStretch - 1.0f)), static_cast<f64>(row.WorstDeviation));
    }

    for (const Row& row : rows)
    {
        if (row.Model == GroomSolverModel::PositionBasedDistance)
        {
            continue; // recorded, not asserted — see the comment above
        }
        EXPECT_LE(std::abs(row.WorstStretch - 1.0f), 1.0e-4f)
            << ToString(row.Model) << " at " << row.Hz << " Hz is meant to be exact by construction";
    }

    // == What separates the two projection models, as the data actually shows ==
    //
    // Two directional claims were tried here before this one and BOTH were
    // wrong on their own numbers, which is worth recording because the wrong
    // ones are the intuitive ones:
    //
    //   * "DFTL deviates further from rest, because it moves more." It deviates
    //     LESS, at every frame rate.
    //   * "DFTL keeps swinging longer after the driver stops." On this fixture
    //     it travels roughly a third as far -- and the FTL figure is large
    //     enough (8 m of path for a 1.1 m chain in half a second) that what the
    //     path-length metric is really measuring is per-step chatter, not swing.
    //
    // What the table above does show, consistently and at every frame rate, is
    // that DFTL's worst deviation from the groomed shape is SMALLER than FTL's.
    // The correct reading is that returning the momentum the projection removed
    // lets the chain TRACK its driver instead of lagging behind it, and lag is
    // what the deviation measures. That is asserted here because it holds three
    // times out of three across a 5x span of step sizes, which is the
    // difference between a prediction and a coincidence.
    const auto deviationAt = [&rows](GroomSolverModel model, f32 hz)
    {
        const auto found = std::ranges::find_if(rows, [&](const Row& r)
                                                { return r.Model == model && std::abs(r.Hz - hz) < 0.5f; });
        return found != rows.end() ? found->WorstDeviation : 0.0f;
    };
    for (const f32 hz : { 30.0f, 60.0f, 144.0f })
    {
        EXPECT_LT(deviationAt(GroomSolverModel::DynamicFollowTheLeader, hz),
                  deviationAt(GroomSolverModel::FollowTheLeader, hz))
            << hz << " Hz: the velocity correction is meant to reduce the chain's lag behind its driver";
    }
}

// The structural invariant that justifies the two projection models being ONE
// code path: DFTL at a zero velocity correction IS FollowTheLeader, bit for
// bit. Someone splitting them into two implementations later has to break this
// to do it.
TEST(GroomGuideSimulation, DynamicFollowTheLeaderAtZeroCorrectionIsFollowTheLeader)
{
    GroomSimulationParams ftl;
    ftl.Model = GroomSolverModel::FollowTheLeader;
    ftl.CollisionEnabled = false;

    GroomSimulationParams dftl = ftl;
    dftl.Model = GroomSolverModel::DynamicFollowTheLeader;
    dftl.VelocityCorrection = 0.0f;

    GroomGuideSimulationState a;
    GroomGuideSimulationState b;
    (void)RunMotion(TestGuides::Make(3, 10), ftl, 1.0f / 60.0f, 1.0f, a);
    (void)RunMotion(TestGuides::Make(3, 10), dftl, 1.0f / 60.0f, 1.0f, b);
    EXPECT_EQ(a.Curr, b.Curr);
    EXPECT_EQ(a.Prev, b.Prev);
}
