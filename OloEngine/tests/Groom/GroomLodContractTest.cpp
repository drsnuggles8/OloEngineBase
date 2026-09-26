#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomLodContractTest — issue #1252, the contracts that only exist in motion.
//
// The comparison (GroomLodComparisonTest) decides WHAT the representations are
// worth. This file pins the rules that a still frame cannot show:
//
//   * the coverage compensation is LINEAR — 1/k, not 1/sqrt(k) — and it is
//     computed from the ACHIEVED fraction;
//   * each of the three budgets moves on its OWN curve and its OWN hysteresis,
//     so one flickering axis cannot pin another (criterion 3);
//   * the two anti-thrash mechanisms deliver the two bounds GroomLod.h claims,
//     stated as the failure each prevents rather than as "no flicker";
//   * the cook is deterministic and conserves the width the coat COVERS --
//     not the width it would cover if no strand overlapped another (#1428) --
//     and it refuses rather than truncates.
//
// WHY THE HYSTERESIS CASES DRIVE A CAMERA. A hysteresis asserted at one point
// is not asserted at all: every implementation passes "at 300 px it is on
// strands". The cases below run a pixel-size SEQUENCE and count transitions,
// which is the only shape in which "does not thrash" is a statement with a
// truth value. That is criterion 2's "measured in motion", as a number.
// =============================================================================

#include <gtest/gtest.h>

#include "GroomLodFixture.h"
#include "GroomStrandFixture.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomLod.h"
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

using namespace OloEngine;

namespace
{
    [[nodiscard]] GroomLodPolicy EnabledPolicy()
    {
        GroomLodPolicy policy;
        policy.Enabled = true;
        return policy;
    }

    [[nodiscard]] GroomLodInputs InputsAt(f32 pixelSize, bool cards = true)
    {
        GroomLodInputs inputs;
        inputs.PixelSize = pixelSize;
        inputs.CardLevelAvailable = cards;
        inputs.MeshLevelAvailable = false;
        inputs.MeshTierSupported = false;
        return inputs;
    }

    // Runs `frames` of a pixel-size sequence and counts how often the
    // representation actually changed, and how often it got COARSER — the two
    // numbers the anti-thrash claims are about.
    struct Motion
    {
        u32 Changes = 0;
        u32 Coarsenings = 0;
        std::vector<u32> CoarseningFrames;
        GroomRepresentation Final = GroomRepresentation::Strand;
    };

    template<typename SizeAtFrame>
    [[nodiscard]] Motion Drive(const GroomLodPolicy& policy, u32 frames, SizeAtFrame&& sizeAtFrame)
    {
        Motion motion;
        GroomLodState state;
        for (u32 frame = 0; frame < frames; ++frame)
        {
            const GroomRepresentation before = state.Representation;
            const GroomLodDecision decision = AdvanceGroomLod(policy, InputsAt(sizeAtFrame(frame)), state);
            if (decision.Representation != before)
            {
                ++motion.Changes;
                if (decision.Representation > before)
                {
                    ++motion.Coarsenings;
                    motion.CoarseningFrames.push_back(frame);
                }
            }
            motion.Final = decision.Representation;
        }
        return motion;
    }

    // A groom small enough to cook quickly and dense enough that clustering
    // actually clusters.
    //
    // WITH ITS ROOT UVs FOLDED INTO THE UNIT CHART. #1246's generator records an
    // unwrapped `phi / 2pi`, which reaches thousands; the clump-cell addressing
    // clamps a UV to +/-16, so the raw fixture is exactly the groom
    // GroomLodBuilder now refuses by name. Folding here is what makes these
    // cases about the cook rather than about that refusal — and
    // ARawFixtureIsRefusedBecauseItsRootUVsAreUnaddressable below is what covers
    // the refusal itself.
    [[nodiscard]] Ref<GroomAsset> MakeTestPelt()
    {
        auto coat = Tests::GroomStrandFixture::MakePelt(2000u, 4u);
        EXPECT_TRUE(coat.Groom) << coat.FailureReason;
        if (!coat.Groom)
        {
            return nullptr;
        }
        std::string reason;
        Ref<GroomAsset> wrapped = Tests::GroomLodFixture::RebuildWithWrappedRootUVs(*coat.Groom, reason);
        EXPECT_TRUE(wrapped) << reason;
        return wrapped;
    }

    // Two clusters, one card each, in two groups sharing one root UV:
    //
    //   * a LOCK: `members` identical straight strands on one root. From every
    //     side they cover exactly one strand's width, however many there are.
    //   * a FAN: `members` straight strands in a row along x, `spacing` apart.
    //     Seen from almost every side no two of them overlap; seen end-on down
    //     the row they all do.
    //
    // The two ends of the overlap range, which is what #1428's long coat sits
    // between: its long-hair locks cover 0.40 of their summed width.
    [[nodiscard]] Ref<GroomAsset> MakeLockAndFan(u32 members, f32 width, f32 spacing)
    {
        GroomBuilder builder;
        std::string reason;
        u16 lock = 0;
        u16 fan = 0;
        EXPECT_TRUE(builder.AddGroup("lock", lock, reason)) << reason;
        EXPECT_TRUE(builder.AddGroup("fan", fan, reason)) << reason;
        const std::vector<f32> widths(3u, width);
        for (u32 m = 0; m < members; ++m)
        {
            GroomCurveInput strand;
            strand.Widths = widths;
            strand.RootUV = glm::vec2(0.5f);

            const std::vector<glm::vec3> lockPoints{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.05f, 0.0f }, { 0.0f, 0.1f, 0.0f } };
            strand.Points = lockPoints;
            strand.GroupId = lock;
            EXPECT_TRUE(builder.AddCurve(strand, reason)) << reason;

            const f32 x = static_cast<f32>(m) * spacing;
            const std::vector<glm::vec3> fanPoints{ { x, 0.0f, 1.0f }, { x, 0.05f, 1.0f }, { x, 0.1f, 1.0f } };
            strand.Points = fanPoints;
            strand.GroupId = fan;
            EXPECT_TRUE(builder.AddCurve(strand, reason)) << reason;
        }
        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        if (groom)
        {
            EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
        }
        return groom;
    }

    // The widths the card of `group` carries, root to tip.
    [[nodiscard]] std::vector<f32> CardWidths(const GroomAsset& base, const GroomLodLevel& level, u16 group)
    {
        const GroomCurveView view = level.GetCurveView();
        for (u32 card = 0; card < view.GetCurveCount(); ++card)
        {
            if (view.CurveGroupIds[card] == group)
            {
                const u32 first = view.GetCurveFirstPoint(card);
                const u32 count = view.GetCurvePointCount(card);
                return { level.PointWidths.begin() + first, level.PointWidths.begin() + first + count };
            }
        }
        ADD_FAILURE() << "no card for group " << group << " of " << base.GetGroupCount();
        return {};
    }
} // namespace

// =============================================================================
// The coverage compensation
// =============================================================================

TEST(GroomLodCompensation, TheRuleIsLinearBecauseAStrandIsABandAndNotASprite)
{
    // The contract, over a sweep rather than at a point: compensation x
    // retained fraction == 1 wherever the cap is not binding.
    //
    // 1/sqrt(k) — FoliageLod's rule, for sprites whose area goes as the square
    // of their linear size — would give 0.5 here at a quarter density and 0.35
    // at a sixteenth. The tolerance is far tighter than that gap, so this case
    // SEPARATES the two rules rather than merely bounding one of them.
    for (u32 step = 0; step <= 3; ++step)
    {
        const f32 fraction = GroomLodStepFraction(step);
        const f32 compensation = GroomLodWidthCompensation(fraction, 16.0f);
        EXPECT_NEAR(static_cast<f64>(compensation * fraction), 1.0, 1.0e-5)
            << "step " << step << ": the compensated coat does not carry the density it started with";
    }

    // And it is computed from a fraction, not from a step: a budget that
    // retained 0.37 of a role (which an integer stride will) compensates by
    // 1/0.37, not by the 1/0.5 the policy asked for.
    EXPECT_NEAR(static_cast<f64>(GroomLodWidthCompensation(0.37f, 16.0f)), 1.0 / 0.37, 1.0e-4);
}

TEST(GroomLodCompensation, TheCapIsReportedRatherThanPretendedAway)
{
    // Past the cap the coat genuinely is thinner than it was authored. The
    // function returns the cap rather than the ideal, so a caller comparing the
    // two can say so — which is what the editor's readout does.
    EXPECT_NEAR(static_cast<f64>(GroomLodWidthCompensation(1.0f / 64.0f, 8.0f)), 8.0, 1.0e-5);
    EXPECT_NEAR(static_cast<f64>(GroomLodWidthCompensation(1.0f, 8.0f)), 1.0, 1.0e-5);
    EXPECT_NEAR(static_cast<f64>(GroomLodWidthCompensation(2.0f, 8.0f)), 1.0, 1.0e-5);

    // An empty selection compensates by NOTHING. Returning the cap would make a
    // coat that came back from zero strands arrive as a mat of maximally fat
    // ones for one frame.
    EXPECT_NEAR(static_cast<f64>(GroomLodWidthCompensation(0.0f, 8.0f)), 1.0, 1.0e-5);
    EXPECT_NEAR(static_cast<f64>(GroomLodWidthCompensation(std::nanf(""), 8.0f)), 1.0, 1.0e-5);
}

// =============================================================================
// The budget curves
// =============================================================================

TEST(GroomLodBudget, ACoatThatGetsSmallerNeverAsksForMoreWork)
{
    // Monotonicity is what every stability argument in GroomLod.h rests on, and
    // it is swept rather than sampled: a non-monotone rung would pass three
    // spot checks and oscillate on the fourth.
    GroomLodBudgetCurve curve;
    u32 previous = 0;
    for (f32 pixelSize = 1024.0f; pixelSize > 0.5f; pixelSize *= 0.97f)
    {
        const u32 step = GroomLodBudgetStep(curve, pixelSize);
        EXPECT_GE(step, previous) << "at " << pixelSize << " px the budget asked for LESS thinning than at a larger size";
        previous = step;
    }
    EXPECT_EQ(previous, curve.MaxSteps) << "the curve never reached its coarsest step over a 2000x sweep";
}

TEST(GroomLodBudget, ACoatWithNoMeasurableSizeTakesTheCheapestStep)
{
    // Behind the camera, degenerate, or a NaN out of a corrupt transform. The
    // full budget would be spent on something nobody can see, which is the
    // failure this whole feature is about.
    GroomLodBudgetCurve curve;
    EXPECT_EQ(GroomLodBudgetStep(curve, 0.0f), curve.MaxSteps);
    EXPECT_EQ(GroomLodBudgetStep(curve, -10.0f), curve.MaxSteps);
    EXPECT_EQ(GroomLodBudgetStep(curve, std::nanf("")), curve.MaxSteps);
    EXPECT_EQ(GroomLodBudgetStep(curve, curve.FullPixelSize), 0u);
    EXPECT_EQ(GroomLodBudgetStep(curve, curve.FullPixelSize * 4.0f), 0u);
}

TEST(GroomLodBudget, TheThreeAxesMoveIndependently)
{
    // Criterion 3, stated as the thing that would break it: one apparent size
    // at which the three curves disagree, and three different answers.
    GroomLodPolicy policy = EnabledPolicy();
    policy.Visibility = GroomLodBudgetCurve{ 512.0f, 4u };
    policy.Simulation = GroomLodBudgetCurve{ 128.0f, 4u };
    policy.Shadow = GroomLodBudgetCurve{ 2048.0f, 5u };
    policy.Hysteresis = 0.0f;
    policy.HoldFrames = 0;

    GroomLodState state;
    const GroomLodDecision decision = AdvanceGroomLod(policy, InputsAt(100.0f), state);

    // The rung rule: halve FullPixelSize until the coat is at least as large as
    // the rung, and the number of halvings taken is the step. At 100 px:
    //   512  -> 256, 128, 64: the first rung 100 clears is 64, so step 3
    //   128  ->  64:          step 1
    //   2048 -> ... 64:       step 5
    // Three curves, one apparent size, three different answers — which is
    // criterion 3 as a number rather than as a design intention.
    EXPECT_EQ(decision.VisibilityStep, 3u);
    EXPECT_EQ(decision.SimulationStep, 1u);
    EXPECT_EQ(decision.ShadowStep, 5u);
    EXPECT_NE(decision.VisibilityStep, decision.ShadowStep);
    EXPECT_NE(decision.VisibilityStep, decision.SimulationStep)
        << "the three budgets collapsed onto one number, which is the coupled LOD scalar criterion 3 forbids";
}

TEST(GroomLodBudget, AFlickeringShadowStepDoesNotPinTheVisibilityStep)
{
    // The failure a SHARED stability counter would produce: the shadow axis
    // straddling one of its rungs would reset the counter every frame and the
    // visibility axis would never be allowed to coarsen at all.
    GroomLodPolicy policy = EnabledPolicy();
    policy.Hysteresis = 0.0f;
    policy.HoldFrames = 3;
    // The shadow curve's rung at 256 px; the sequence straddles it every frame.
    policy.Shadow = GroomLodBudgetCurve{ 512.0f, 3u };
    // The visibility curve is nowhere near a rung at these sizes.
    policy.Visibility = GroomLodBudgetCurve{ 4096.0f, 4u };

    GroomLodState state;
    u32 visibilityStep = 0;
    for (u32 frame = 0; frame < 32; ++frame)
    {
        const f32 pixelSize = (frame % 2u == 0u) ? 258.0f : 254.0f;
        visibilityStep = AdvanceGroomLod(policy, InputsAt(pixelSize), state).VisibilityStep;
    }
    // 256 px against a 4096 px full size is four halvings, which the curve
    // allows. If the shadow axis had pinned it, it would still be at 0.
    EXPECT_EQ(visibilityStep, 4u) << "a flickering shadow step held the visibility step at its old value";
}

// =============================================================================
// The representation ladder
// =============================================================================

TEST(GroomLodLadder, ApparentSizeSelectsTheTierAndTheReasonSaysWhyNot)
{
    GroomLodPolicy policy = EnabledPolicy();
    policy.Hysteresis = 0.0f;
    policy.HoldFrames = 0;

    {
        GroomLodState state;
        const GroomLodDecision decision = AdvanceGroomLod(policy, InputsAt(1000.0f), state);
        EXPECT_EQ(decision.Representation, GroomRepresentation::Strand);
        EXPECT_EQ(decision.Reason, GroomLodFallbackReason::None);
        EXPECT_FALSE(decision.IsFallback());
    }
    {
        GroomLodState state;
        const GroomLodDecision decision = AdvanceGroomLod(policy, InputsAt(100.0f), state);
        EXPECT_EQ(decision.Representation, GroomRepresentation::Card);
        EXPECT_EQ(decision.Reason, GroomLodFallbackReason::None);
    }
    {
        // Below the mesh threshold with no shell tier shipped: cards, and a
        // reason that sends the reader to the measurement rather than to a
        // setting.
        GroomLodState state;
        const GroomLodDecision decision = AdvanceGroomLod(policy, InputsAt(10.0f), state);
        EXPECT_EQ(decision.Representation, GroomRepresentation::Card);
        EXPECT_EQ(decision.Reason, GroomLodFallbackReason::MeshTierNotSelected);
        EXPECT_TRUE(decision.IsFallback());
    }
    {
        // No card level cooked: strands, and the reason names the COOK. The
        // distinction matters — one of these is fixed by re-cooking and the
        // other by measuring, and a single "not available" would send half the
        // readers to the wrong place.
        GroomLodState state;
        const GroomLodDecision decision = AdvanceGroomLod(policy, InputsAt(100.0f, /*cards*/ false), state);
        EXPECT_EQ(decision.Representation, GroomRepresentation::Strand);
        EXPECT_EQ(decision.Reason, GroomLodFallbackReason::LevelNotCooked);
    }
}

TEST(GroomLodLadder, ADisabledPolicyIsTheIdentityAndForgetsWhereItWas)
{
    // The A/B control for every capture in this issue is "turn it off", so it
    // has to be the same picture every time it is taken — including after the
    // coat has spent a hundred frames on cards.
    GroomLodPolicy policy = EnabledPolicy();
    GroomLodState state;
    for (u32 frame = 0; frame < 100; ++frame)
    {
        (void)AdvanceGroomLod(policy, InputsAt(20.0f), state);
    }
    ASSERT_EQ(state.Representation, GroomRepresentation::Card);

    policy.Enabled = false;
    const GroomLodDecision decision = AdvanceGroomLod(policy, InputsAt(20.0f), state);
    EXPECT_EQ(decision.Representation, GroomRepresentation::Strand);
    EXPECT_EQ(decision.Reason, GroomLodFallbackReason::NotRequested);
    EXPECT_FALSE(decision.IsFallback());
    EXPECT_EQ(decision.VisibilityStep, 0u);
    EXPECT_EQ(decision.SimulationStep, 0u);
    EXPECT_EQ(decision.ShadowStep, 0u);
    EXPECT_EQ(state, GroomLodState{}) << "a disabled policy left hysteresis state behind";
}

TEST(GroomLodLadder, AnInvertedThresholdPairCannotMakeTheLadderNonMonotone)
{
    // An author can set the mesh threshold above the card one. Left alone that
    // makes a coat skip the card tier on the way out and take it on the way
    // back, and every stability statement in GroomLod.h false.
    GroomLodPolicy policy = EnabledPolicy();
    policy.CardPixelSize = 64.0f;
    policy.MeshPixelSize = 512.0f;
    const GroomLodPolicy sane = SanitizeGroomLodPolicy(policy);
    EXPECT_LE(sane.MeshPixelSize, sane.CardPixelSize);

    // And a NaN takes the default rather than poisoning every comparison.
    GroomLodPolicy poisoned = EnabledPolicy();
    poisoned.CardPixelSize = std::nanf("");
    poisoned.Hysteresis = std::nanf("");
    const GroomLodPolicy repaired = SanitizeGroomLodPolicy(poisoned);
    EXPECT_TRUE(std::isfinite(repaired.CardPixelSize));
    EXPECT_TRUE(std::isfinite(repaired.Hysteresis));
    EXPECT_GT(repaired.CardPixelSize, 0.0f);
}

// =============================================================================
// Anti-thrash: the two bounds, each as the failure it prevents
// =============================================================================

TEST(GroomLodHysteresis, AnOscillationNarrowerThanTheDeadBandChangesTheTierAtMostOnce)
{
    // THE BAND GUARANTEE. The hand-over edge slides by ±h·threshold depending
    // on which tier the coat holds, so the window a transition can happen in is
    // 2h·threshold wide. A camera whose travel is narrower than that window
    // cannot contain both edges, so it crosses at most one of them — ever,
    // whatever its period.
    GroomLodPolicy policy = EnabledPolicy();
    policy.CardPixelSize = 256.0f;
    policy.Hysteresis = 0.15f;
    policy.HoldFrames = 0; // isolate the band from the frame hold

    // A travel of 60 px centred on the threshold: comfortably inside the
    // 2 x 0.15 x 256 = 76.8 px dead band.
    const auto sequence = [](u32 frame)
    {
        const f64 phase = static_cast<f64>(frame) * 0.7;
        return 256.0f + static_cast<f32>(30.0 * std::sin(phase));
    };
    const Motion motion = Drive(policy, 400u, sequence);
    EXPECT_LE(motion.Changes, 1u) << "a camera oscillating inside the dead band flipped the representation "
                                  << motion.Changes << " times";
}

TEST(GroomLodHysteresis, AFrameByFrameOscillationAcrossTheWholeLadderNeverCoarsensAtAll)
{
    // THE HOLD GUARANTEE, at its strongest. A camera that alternates every
    // frame never accumulates the consecutive stable frames a coarsening needs,
    // so it never gets one.
    //
    // This is the case FoliageLod.h explicitly cannot beat — it carries no
    // per-instance state, so one frame of camera history is all it has. A groom
    // is an entity and its LOD state persists, which is why the bound here is
    // an absolute rather than a fraction.
    GroomLodPolicy policy = EnabledPolicy();
    policy.CardPixelSize = 256.0f;
    policy.Hysteresis = 0.15f;
    policy.HoldFrames = 4;

    const Motion motion = Drive(policy, 200u, [](u32 frame)
                                { return (frame % 2u == 0u) ? 600.0f : 40.0f; });
    EXPECT_EQ(motion.Coarsenings, 0u) << "a two-frame oscillation coarsened the representation";
    EXPECT_EQ(motion.Final, GroomRepresentation::Strand);
}

TEST(GroomLodHysteresis, ASlowOscillationCoarsensAtMostOncePerHold)
{
    // The general bound, for a camera the band cannot stop: coarsenings are at
    // least HoldFrames apart. Stated as the gap rather than as a count, because
    // a count alone would pass for an implementation that coarsened twice in
    // adjacent frames and then stopped.
    GroomLodPolicy policy = EnabledPolicy();
    policy.CardPixelSize = 256.0f;
    policy.Hysteresis = 0.15f;
    policy.HoldFrames = 4;

    const Motion motion = Drive(policy, 200u, [](u32 frame)
                                { return (frame / 6u) % 2u == 0u ? 600.0f : 40.0f; });
    ASSERT_GT(motion.Coarsenings, 0u) << "the sequence never crossed, so this case decides nothing";
    for (sizet i = 1; i < motion.CoarseningFrames.size(); ++i)
    {
        EXPECT_GE(motion.CoarseningFrames[i] - motion.CoarseningFrames[i - 1], policy.HoldFrames)
            << "two coarsenings landed within the hold";
    }
}

TEST(GroomLodHysteresis, StraddlingTheMeshEdgeStillHandsOverToCards)
{
    // THE DEADLOCK THE HOLD COUNTER CAUSED WHEN IT COUNTED THE IDEAL TIER.
    //
    // The shell tier ships in no build today, so a coat oscillating across the
    // MESH edge has an `ideal` that alternates Mesh/Card while the tier it can
    // actually take is Card on both frames. Counting the ideal reset the
    // stability counter every frame, it never reached HoldFrames, and a groom
    // still on Strand never handed over at all — paying full strand cost at far
    // range and reporting a permanent HeldByHysteresis, a reason that sends its
    // owner to a setting which cannot fix it.
    //
    // Reachable by anything that arrives far away rather than walking there: a
    // spawned or teleported entity starts at Strand, and the camera only has to
    // jitter across the mesh edge.
    GroomLodPolicy policy = EnabledPolicy();
    policy.CardPixelSize = 256.0f;
    policy.MeshPixelSize = 48.0f;
    policy.Hysteresis = 0.15f;
    policy.HoldFrames = 4;

    // 30 px is below the mesh edge (48 x 0.85 = 40.8) and 60 px is above it and
    // below the card edge (256 x 0.85 = 217.6), so the IDEAL alternates
    // Mesh/Card while the TARGET is Card on every frame.
    GroomLodState state;
    GroomLodDecision decision;
    u32 lastHeldFrame = 0;
    u32 heldFrames = 0;
    for (u32 frame = 0; frame < 40u; ++frame)
    {
        decision = AdvanceGroomLod(policy, InputsAt((frame % 2u == 0u) ? 30.0f : 60.0f), state);
        if (decision.Reason == GroomLodFallbackReason::HeldByHysteresis)
        {
            lastHeldFrame = frame;
            ++heldFrames;
        }
    }

    // HeldByHysteresis on the first few frames is CORRECT — the coat starts on
    // Strand and the hold has genuinely not elapsed. The bug was that it never
    // elapsed. So what is asserted is TRANSIENCE: the hold must be done with by
    // the time it has had HoldFrames consecutive stable requests, and nothing
    // after that may still be waiting on it.
    EXPECT_LE(lastHeldFrame, policy.HoldFrames)
        << "the coat was still being held at frame " << lastHeldFrame << " of 40 (" << heldFrames
        << " held frames in total) — the hold is never elapsing, so the hand-over can never happen";

    EXPECT_EQ(decision.Representation, GroomRepresentation::Card)
        << "a coat straddling the mesh edge never handed over to cards, so it pays full strand cost at a "
           "distance it is invisible at";

    // The reason depends on which side of the mesh edge the frame landed on,
    // and BOTH are correct answers — which is why the loop asserts the absence
    // of the wrong one rather than the presence of a single right one.
    //
    // In the mesh band the coat is on the coarsest tier this engine draws and
    // the reason names the MEASUREMENT that refused the shell
    // (docs/analysis/groom-representation-lod-1252.md), not the hold.
    EXPECT_EQ(AdvanceGroomLod(policy, InputsAt(30.0f), state).Reason,
              GroomLodFallbackReason::MeshTierNotSelected);
    // In the card band it is simply on the tier its size selects.
    EXPECT_EQ(AdvanceGroomLod(policy, InputsAt(60.0f), state).Reason, GroomLodFallbackReason::None);
}

TEST(GroomLodHysteresis, RefiningIsImmediateBecauseACoarseCoatUpCloseIsAPictureAnyoneCanSee)
{
    GroomLodPolicy policy = EnabledPolicy();
    policy.HoldFrames = 30;

    GroomLodState state;
    // Settle onto cards.
    for (u32 frame = 0; frame < 60; ++frame)
    {
        (void)AdvanceGroomLod(policy, InputsAt(100.0f), state);
    }
    ASSERT_EQ(state.Representation, GroomRepresentation::Card);

    // One frame of being close is enough.
    const GroomLodDecision decision = AdvanceGroomLod(policy, InputsAt(2000.0f), state);
    EXPECT_EQ(decision.Representation, GroomRepresentation::Strand)
        << "a coat that filled the screen stayed on cards waiting for a hold";
    EXPECT_EQ(decision.Reason, GroomLodFallbackReason::None);
}

// =============================================================================
// The cook
// =============================================================================

TEST(GroomLodCook, CookingTheSameGroomTwiceProducesTheSameCards)
{
    // The determinism contract GroomCooker.h sets for the whole format, applied
    // to the new section. A sorted vector rather than an iterated hash map is
    // what makes it true; this is what would catch a regression to the latter.
    const Ref<GroomAsset> pelt = MakeTestPelt();
    ASSERT_TRUE(pelt);

    GroomCardSettings settings;
    GroomLodLevel first;
    GroomLodLevel second;
    std::string reason;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*pelt, settings, first, reason, nullptr)) << reason;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*pelt, settings, second, reason, nullptr)) << reason;
    EXPECT_TRUE(first == second) << "two cooks of one groom produced different cards";
}

TEST(GroomLodCook, ACardCarriesItsClustersCoveredWidthAndNeverCrossesAGroup)
{
    const Ref<GroomAsset> pelt = MakeTestPelt();
    ASSERT_TRUE(pelt);
    ASSERT_GT(pelt->GetGroupCount(), 1u) << "the fixture must have several groups for the group claim to mean anything";

    GroomCardSettings settings;
    GroomLodLevel level;
    GroomCardBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*pelt, settings, level, reason, &stats)) << reason;

    // The density claim, at the cook: every card carries what its members
    // cover, and that is never more than their summed width.
    ASSERT_GT(stats.MemberWidthSum, 0.0);
    ASSERT_GT(stats.MemberCoveredWidthSum, 0.0);
    EXPECT_NEAR(stats.CardWidthSum / stats.MemberCoveredWidthSum, 1.0, 1.0e-3);
    EXPECT_LE(stats.MemberCoveredWidthSum, stats.MemberWidthSum * (1.0 + 1.0e-6));
    EXPECT_LT(stats.CardsBuilt, stats.CurvesConsidered);

    // The group claim: every card's members share its group, so a card keeps
    // its role, its coat description and its budget weight. Checked through the
    // source map — the representative is a member, so its group is the
    // cluster's.
    const GroomCurveView view = level.GetCurveView();
    for (u32 card = 0; card < view.GetCurveCount(); ++card)
    {
        const u32 source = level.SourceCurves[card];
        ASSERT_LT(source, pelt->GetCurveCount());
        EXPECT_EQ(view.CurveGroupIds[card], pelt->GetCurveGroupIds()[source])
            << "card " << card << " does not belong to its representative's group";
    }

    // And every card is reachable by the renderer: the source map is in range
    // for the binding's root-transform array, which is the read that would go
    // out of bounds.
    std::string levelReason;
    EXPECT_TRUE(level.Validate(pelt->GetCurveCount(), pelt->GetGroupCount(), levelReason)) << levelReason;
}

TEST(GroomLodCook, ACardIsAsWideAsWhatItsMembersCoverAndNoWider)
{
    // #1428. A card that SUMS its members' widths is exact only while no member
    // hides behind another. Strands sharing a clump cell grow from roots a few
    // millimetres apart and are combed the same way, so on a dense coat they
    // overlap, and a summed card covers more than the strands it replaced: the
    // long-coated horse's card tier drew 1.35x the coat's share of the animal.
    constexpr u32 kMembers = 16u;
    constexpr f32 kWidth = 0.001f;
    const Ref<GroomAsset> groom = MakeLockAndFan(kMembers, kWidth, 20.0f * kWidth);
    ASSERT_TRUE(groom);
    ASSERT_EQ(groom->GetGroupCount(), 2u);

    GroomCardSettings settings;
    ASSERT_EQ(settings.Width, GroomCardWidth::Covered) << "the covered width is the shipped default";
    // The GEOMETRY first: a zero source size measures the exact union of the
    // bands, with no pixel in it. The pixel's part is the last block below.
    settings.SourcePixelSize = 0.0f;
    GroomLodLevel level;
    GroomCardBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*groom, settings, level, reason, &stats)) << reason;
    ASSERT_EQ(stats.CardsBuilt, 2u) << "one card per group: the lock and the fan";

    // The lock: sixteen strands in one place cover ONE strand's width.
    for (const f32 w : CardWidths(*groom, level, 0u))
    {
        EXPECT_NEAR(w, kWidth, kWidth * 1.0e-4) << "a card of coincident strands is one strand wide, not sixteen";
    }
    // The fan: spread twenty widths apart, they overlap only when seen end-on
    // down the row, which is one of the eight directions averaged. So the card
    // is (7 x 16 + 1) / 8 strand widths: nearly the sum, and not the sum.
    constexpr f32 kFanCovered = (7.0f * kMembers + 1.0f) / 8.0f * kWidth;
    for (const f32 w : CardWidths(*groom, level, 1u))
    {
        EXPECT_NEAR(w, kFanCovered, kWidth * 1.0e-3) << "a fan's card carries what the fan covers";
    }
    EXPECT_NEAR(stats.CardWidthSum / stats.MemberCoveredWidthSum, 1.0, 1.0e-6);
    EXPECT_LT(stats.MemberCoveredWidthSum, stats.MemberWidthSum);

    // The measured-and-rejected alternative still does what it says: the SUM,
    // sixteen widths for both clusters. That is the card #1428 measured.
    settings.Width = GroomCardWidth::Summed;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*groom, settings, level, reason, &stats)) << reason;
    for (const u16 group : { u16{ 0 }, u16{ 1 } })
    {
        for (const f32 w : CardWidths(*groom, level, group))
        {
            EXPECT_NEAR(w, kMembers * kWidth, kWidth * 1.0e-4) << "group " << group;
        }
    }
    EXPECT_NEAR(stats.CardWidthSum / stats.MemberWidthSum, 1.0, 1.0e-6);

    // And the averaged aggregation takes the same width rule: its mean
    // centreline through the lock is the lock itself.
    settings.Width = GroomCardWidth::Covered;
    settings.Aggregation = GroomCardAggregation::MeanCentreline;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*groom, settings, level, reason, &stats)) << reason;
    for (const f32 w : CardWidths(*groom, level, 0u))
    {
        EXPECT_NEAR(w, kWidth, kWidth * 1.0e-4) << "MeanCentreline: the lock's card";
    }

    // THE PIXEL. Past the hand-over a strand is thinner than a pixel, and the
    // shader draws it one pixel wide at an alpha of its true width, the
    // overlapping ones composited as independent layers. Sixteen coincident
    // strands are then DRAWN as 1 - (1 - w/P)^16 of a pixel, not as one
    // strand's width, and a card cooked to the bare union draws the coat thin
    // at range: on the long-coated horse it kept 0.65 of the coat's share. So
    // the shipped cook measures at the pixel the hand-over happens at.
    settings.Aggregation = GroomCardAggregation::RepresentativeStrand;
    settings.SourcePixelSize = 256.0f;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*groom, settings, level, reason, &stats)) << reason;
    const f32 pixel = stats.PixelFootprint;
    ASSERT_GT(pixel, kWidth) << "the fixture must be sub-pixel at its hand-over for this to test the pixel";
    const f32 drawnLock = pixel * (1.0f - std::pow(1.0f - kWidth / pixel, static_cast<f32>(kMembers)));
    for (const f32 w : CardWidths(*groom, level, 0u))
    {
        EXPECT_NEAR(w, drawnLock, drawnLock * 1.0e-4) << "the lock's card is as wide as the lock is DRAWN";
    }
    EXPECT_GT(drawnLock, kWidth) << "and that is wider than its bare geometric union";
    EXPECT_LT(drawnLock, kMembers * kWidth) << "and narrower than its sum";

    // An unknown width model is refused by name, like every other setting.
    settings.Width = GroomCardWidth::Count;
    EXPECT_FALSE(GroomLodBuilder::BuildCardLevel(*groom, settings, level, reason, nullptr));
    EXPECT_NE(reason.find("width model"), std::string::npos) << reason;
}

TEST(GroomLodCook, TheCardTierShadowIsBakedAtEachGroupsFibreArea)
{
    // #1428. The self-shadow volume stores FIBRE, and a card is drawn as wide as
    // its members cover -- one strand's width for a lock of sixteen identical
    // strands, all sixteen for a fan whose strands never overlap. So a card
    // stands for sixteen times its drawn fibre in one group and once its drawn
    // fibre in the other, and any single factor over-darkens one of them. On
    // the horses the groom-wide factor was 2.7-3.2 where the body's groups
    // needed 1.1 and the tail's 4-5. Measured on the drawn streams: each
    // group's baked fibre (drawn length x diameter x its segment's scale) is
    // the base groom's.
    constexpr u32 kMembers = 16u;
    const Ref<GroomAsset> groom = MakeLockAndFan(kMembers, 1.0e-3f, 0.01f);
    ASSERT_TRUE(groom);
    GroomLodLevel level;
    std::string reason;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*groom, GroomCardSettings{}, level, reason, nullptr)) << reason;

    GroomStrandBuildSettings build;
    build.MaxStrands = groom->GetCurveCount();
    std::vector<f32> scales;
    GroomCardFibreScales(*groom, level, build, nullptr, scales);

    // Fibre per group of a stream, each segment weighted by `weights` (or 1).
    const auto fibre = [&](const GroomBuildSource& source, std::span<const f32> weights)
    {
        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        (void)BuildGroomStrandMesh(source, build, vertices, indices);
        std::vector<f64> byGroup(groom->GetGroupCount(), 0.0);
        // One scale per emitted segment, or the weighting below would read
        // past the list -- the very regression this case exists to catch.
        EXPECT_TRUE(weights.empty() || weights.size() * 4u == vertices.size())
            << weights.size() << " scales for " << vertices.size() / 4u << " segments";
        if (!weights.empty() && weights.size() * 4u != vertices.size())
        {
            return byGroup;
        }
        sizet segment = 0;
        for (u32 curve = 0; curve < source.Curves.GetCurveCount(); ++curve)
        {
            for (u32 i = 0; i + 1u < source.Curves.GetCurvePointCount(curve); ++i, ++segment)
            {
                const GroomStrandVertex& p0 = vertices[segment * 4u];
                const GroomStrandVertex& p1 = vertices[(segment * 4u) + 2u];
                const f64 weight = weights.empty() ? 1.0 : static_cast<f64>(weights[segment]);
                byGroup[source.Curves.GetCurveGroupIds()[curve]] +=
                    static_cast<f64>(glm::length(p1.Position - p0.Position)) * (p0.Radius + p1.Radius) * weight;
            }
        }
        EXPECT_EQ(segment * 4u, vertices.size());
        return byGroup;
    };
    const std::vector<f64> base = fibre(GroomBuildSource::FromAsset(*groom), {});
    const std::vector<f64> baked = fibre(GroomBuildSource::FromLevel(*groom, level), scales);
    ASSERT_EQ(base.size(), 2u);
    for (sizet group = 0; group < base.size(); ++group)
    {
        ASSERT_GT(base[group], 0.0);
        EXPECT_NEAR(baked[group] / base[group], 1.0, 1.0e-4) << "group " << group << " baked at the wrong fibre area";
    }

    // And the two groups need different factors, so no one number could.
    f32 lowest = scales.front();
    f32 highest = scales.front();
    for (const f32 scale : scales)
    {
        lowest = std::min(lowest, scale);
        highest = std::max(highest, scale);
    }
    EXPECT_GT(highest, 3.0f * lowest) << "the fixture must need different factors per group";
}

TEST(GroomLodCook, ACardCarriesItsMembersMeanJitterAndNotOneStrands)
{
    // #1428. The coat's length jitter is a per-STRAND draw of +/- the amplitude.
    // A card stands for the N strands of its cell, so what it should carry is
    // the mean of their N draws, whose spread is the amplitude over sqrt(N).
    // Drawn at the full amplitude per card, a whole cluster's length moved
    // together: the long cards' tips stood out of the long coat as lone, dense
    // lumps in its self-shadow volume, and the card tier darkened the coat 15%
    // more than the strands did. Measured on the drawn stream, per curve: its
    // drawn length over its cooked length is exactly the multiplier it got.
    const Ref<GroomAsset> pelt = MakeTestPelt();
    ASSERT_TRUE(pelt);
    GroomLodLevel level;
    std::string reason;
    // Coarse cells, so a card stands for enough strands that sqrt(N) is a
    // real reduction rather than a rounding of 1.
    GroomCardSettings cardSettings;
    cardSettings.CellSize = 0.15f;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*pelt, cardSettings, level, reason, nullptr)) << reason;

    constexpr f32 kJitter = 0.4f;
    GroomCoatSettings coatSettings;
    coatSettings.Enabled = true;
    coatSettings.Seed = 7u;
    coatSettings.LengthJitter = kJitter;
    const GroomCoatContext coat{ &coatSettings, pelt->GetGroupCoats() };
    GroomStrandBuildSettings build;
    build.MaxStrands = pelt->GetCurveCount();

    // Every curve's drawn/cooked length. The budget keeps every curve, so the
    // stream is the curves in order, four corners per segment.
    const auto multipliers = [&](const GroomBuildSource& source)
    {
        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        const GroomStrandMeshStats stats = BuildGroomStrandMesh(source, build, vertices, indices, nullptr, &coat);
        EXPECT_EQ(stats.StrandsSelected, source.Curves.GetCurveCount()) << "the budget must keep every curve";
        std::vector<f32> out;
        sizet corner = 0;
        for (u32 curve = 0; curve < source.Curves.GetCurveCount(); ++curve)
        {
            const u32 first = source.Curves.GetCurveFirstPoint(curve);
            f64 cooked = 0.0;
            f64 drawn = 0.0;
            for (u32 i = 0; i + 1u < source.Curves.GetCurvePointCount(curve); ++i, corner += 4u)
            {
                cooked += glm::length(source.Curves.GetPoints()[first + i + 1u] - source.Curves.GetPoints()[first + i]);
                drawn += glm::length(vertices[corner + 2u].Position - vertices[corner].Position);
            }
            out.push_back(static_cast<f32>(drawn / cooked));
        }
        EXPECT_EQ(corner, vertices.size());
        return out;
    };
    const auto spread = [](const std::vector<f32>& values)
    {
        f64 mean = 0.0;
        for (const f32 v : values)
        {
            mean += v;
        }
        mean /= static_cast<f64>(values.size());
        f64 variance = 0.0;
        for (const f32 v : values)
        {
            variance += (v - mean) * (v - mean);
        }
        return std::sqrt(variance / static_cast<f64>(values.size()));
    };

    // The strands keep the authored draw: uniform on +/- the amplitude.
    const std::vector<f32> strands = multipliers(GroomBuildSource::FromAsset(*pelt));
    EXPECT_NEAR(spread(strands), kJitter / std::sqrt(3.0), 0.1 * kJitter / std::sqrt(3.0));

    // Each card inside its group's amplitude over sqrt(members), and the
    // jitter not simply switched off.
    const std::vector<f32> cards = multipliers(GroomBuildSource::FromLevel(*pelt, level));
    std::vector<u32> cardsInGroup(pelt->GetGroupCount(), 0u);
    for (const u16 group : level.CurveGroupIds)
    {
        ++cardsInGroup[group];
    }
    f32 widest = 0.0f;
    f64 expectedVariance = 0.0;
    for (u32 card = 0; card < cards.size(); ++card)
    {
        const u16 group = level.CurveGroupIds[card];
        const f32 members = static_cast<f32>(pelt->GetGroupRanges()[group].CurveCount) /
                            static_cast<f32>(cardsInGroup[group]);
        ASSERT_GT(members, 4.0f) << "the cook must cluster for the claim to mean anything";
        const f32 amplitude = kJitter / std::sqrt(members);
        EXPECT_LE(std::abs(cards[card] - 1.0f), amplitude + 1.0e-4f) << "card " << card << " of group " << group;
        widest = std::max(widest, std::abs(cards[card] - 1.0f) / amplitude);
        expectedVariance += static_cast<f64>(amplitude) * amplitude / 3.0;
    }
    EXPECT_GT(widest, 0.5f) << "the cards carry no jitter at all: the mean of N draws still varies";
    const f64 expected = std::sqrt(expectedVariance / static_cast<f64>(cards.size()));
    EXPECT_NEAR(spread(cards), expected, 0.25 * expected) << "the cards' spread is not the members' mean's";
    EXPECT_LT(spread(cards), 0.5 * spread(strands)) << "a card drew one strand's jitter for its whole cluster";
}

TEST(GroomLodCook, ARawFixtureIsRefusedBecauseItsRootUVsAreUnaddressable)
{
    // THE GUARD THE MEASUREMENT PUT THERE. GroomCoatClumpCell clamps a root UV
    // to +/-16, so a groom whose chart runs past that has every strand beyond
    // the clamp land in ONE cell: the cook "succeeds", produces a handful of
    // enormous cards, and the coat loses four fifths of its covered area the
    // instant it hands over. It looked like a working level until it was
    // measured (docs/analysis/groom-representation-lod-1252.md, finding 0).
    auto coat = Tests::GroomStrandFixture::MakePelt(2000u, 4u);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;

    GroomCardSettings settings;
    GroomLodLevel level;
    std::string reason;
    EXPECT_FALSE(GroomLodBuilder::BuildCardLevel(*coat.Groom, settings, level, reason, nullptr));
    EXPECT_NE(reason.find("root UV"), std::string::npos) << "the refusal did not name the cause: " << reason;
    EXPECT_EQ(level.GetCurveCount(), 0u);
}

TEST(GroomLodCook, ACellThatReducesNothingIsRefusedByNameRatherThanCooked)
{
    // A level whose curve count equals the base's costs disk, load time and
    // resident memory to draw exactly what the strand tier draws. Refusing it
    // is what stops that showing up only as a memory figure nobody reads.
    const Ref<GroomAsset> pelt = MakeTestPelt();
    ASSERT_TRUE(pelt);

    GroomCardSettings settings;
    settings.CellSize = GroomCoatLimits::MinClumpCellSize; // every strand its own cell
    GroomLodLevel level;
    std::string reason;
    EXPECT_FALSE(GroomLodBuilder::BuildCardLevel(*pelt, settings, level, reason, nullptr));
    EXPECT_NE(reason.find("no reduction"), std::string::npos) << "the refusal did not name the problem: " << reason;
    EXPECT_EQ(level.GetCurveCount(), 0u) << "a refused cook left a partially-filled level behind";
}

TEST(GroomLodCook, ACardBudgetIsARefusalAndNeverATruncation)
{
    // Truncating would delete whichever region of the pelt sorted last, which
    // is a bald flank — the failure groom-strand-visibility.md rule 7 gives the
    // strand budget its stride to avoid. The cook has no such excuse: it can
    // say no.
    const Ref<GroomAsset> pelt = MakeTestPelt();
    ASSERT_TRUE(pelt);

    GroomCardSettings settings;
    settings.MaxCards = 4;
    GroomLodLevel level;
    std::string reason;
    EXPECT_FALSE(GroomLodBuilder::BuildCardLevel(*pelt, settings, level, reason, nullptr));
    EXPECT_NE(reason.find("above the cap"), std::string::npos) << reason;
    EXPECT_EQ(level.GetCurveCount(), 0u);
}

TEST(GroomLodCook, TheCardCapItselfIsBoundedBecauseItSizesAnAllocation)
{
    // MaxCards is the one setting that reaches a reserve: the build asks for
    // `cardCount * PointsPerCard` points up front. Left unbounded it admits a
    // reserve of MaxCurveCount x 64 — about half a billion points — long before
    // GroomLodLevel::Validate sees the level, and std::bad_alloc is not
    // something any caller of BuildCardLevel catches. So it is validated like
    // the cell size and the point count beside it.
    Ref<GroomAsset> pelt = MakeTestPelt();
    ASSERT_TRUE(pelt);

    GroomLodLevel level;
    std::string reason;

    GroomCardSettings zero;
    zero.MaxCards = 0;
    EXPECT_FALSE(GroomLodBuilder::BuildCardLevel(*pelt, zero, level, reason, nullptr));
    EXPECT_NE(reason.find("card cap"), std::string::npos) << reason;

    GroomCardSettings huge;
    huge.MaxCards = GroomLimits::MaxCurveCount + 1u;
    EXPECT_FALSE(GroomLodBuilder::BuildCardLevel(*pelt, huge, level, reason, nullptr));
    EXPECT_NE(reason.find("card cap"), std::string::npos) << reason;
    EXPECT_EQ(level.GetCurveCount(), 0u);

    // And a level whose POINT count exceeds the format's cap is refused by the
    // level itself, not only by the decoder. The decoder bounds a file-supplied
    // count before it sizes anything; this is the authored path, which has no
    // file behind it to blame.
    GroomCardSettings ok;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*pelt, ok, level, reason, nullptr)) << reason;
    std::string levelReason;
    EXPECT_TRUE(level.Validate(pelt->GetCurveCount(), pelt->GetGroupCount(), levelReason)) << levelReason;

    // Crossing the REAL caps means 8 M curves or 256 M points -- a level of
    // several gigabytes -- so the two size branches would ship unexecuted if
    // the caps were not injectable. ValidateWithCaps exists for exactly this:
    // the caps come down to the level's own size and each branch fires by
    // name. Lowering a cap cannot make an oversized level pass, so the seam
    // does not weaken the boundary it tests.
    const u32 curves = level.GetCurveCount();
    const u64 points = level.Points.size();
    ASSERT_GT(curves, 1u);
    ASSERT_GT(points, 1u);

    EXPECT_FALSE(level.ValidateWithCaps(pelt->GetCurveCount(), pelt->GetGroupCount(), curves - 1u, points, levelReason));
    EXPECT_NE(levelReason.find("curve count"), std::string::npos) << levelReason;

    EXPECT_FALSE(level.ValidateWithCaps(pelt->GetCurveCount(), pelt->GetGroupCount(), curves, points - 1u, levelReason));
    EXPECT_NE(levelReason.find("point count"), std::string::npos) << levelReason;

    // The control both refusals need: AT the caps the same level passes, so
    // what was measured is the crossing and not some unrelated invariant the
    // level was failing all along.
    EXPECT_TRUE(level.ValidateWithCaps(pelt->GetCurveCount(), pelt->GetGroupCount(), curves, points, levelReason))
        << levelReason;
}

TEST(GroomLodCook, ALevelMayNotClaimTheStrandTierAndADuplicateIsRejected)
{
    // Non-const: AttachLodLevels writes to the groom, which is the whole point
    // of it being the one friend that may.
    Ref<GroomAsset> pelt = MakeTestPelt();
    ASSERT_TRUE(pelt);

    GroomCardSettings settings;
    GroomLodLevel level;
    std::string reason;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*pelt, settings, level, reason, nullptr)) << reason;

    // The scope boundary, enforced by the format: nothing may stand in for the
    // close-up strand tier.
    GroomLodLevel impostor = level;
    impostor.Representation = GroomRepresentation::Strand;
    std::string levelReason;
    EXPECT_FALSE(impostor.Validate(pelt->GetCurveCount(), pelt->GetGroupCount(), levelReason));

    // Two levels for one tier would make FindLodLevel's answer depend on write
    // order, which is a determinism hole in a format whose contract is
    // determinism.
    EXPECT_FALSE(GroomLodBuilder::AttachLodLevels(*pelt, { level, level }, reason));
    EXPECT_NE(reason.find("second level"), std::string::npos) << reason;
    EXPECT_TRUE(pelt->GetLodLevels().empty()) << "a rejected attach left levels on the groom";

    // And the good one attaches, is findable, and is counted in the memory
    // readout criterion 4 asks for.
    const u64 before = pelt->GetCpuMemoryBytes();
    ASSERT_TRUE(GroomLodBuilder::AttachLodLevels(*pelt, { level }, reason)) << reason;
    EXPECT_NE(pelt->FindLodLevel(GroomRepresentation::Card), nullptr);
    EXPECT_EQ(pelt->FindLodLevel(GroomRepresentation::Mesh), nullptr);
    EXPECT_GT(pelt->GetCpuMemoryBytes(), before);
}

TEST(GroomLodCook, ASourceMapPointingPastTheBaseGroomIsRejected)
{
    // The read that would go out of bounds is the binding's root-transform
    // lookup in the innermost loop of the renderer, so it is refused at the
    // boundary rather than clamped.
    const Ref<GroomAsset> pelt = MakeTestPelt();
    ASSERT_TRUE(pelt);

    GroomCardSettings settings;
    GroomLodLevel level;
    std::string reason;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*pelt, settings, level, reason, nullptr)) << reason;

    level.SourceCurves[0] = pelt->GetCurveCount();
    std::string levelReason;
    EXPECT_FALSE(level.Validate(pelt->GetCurveCount(), pelt->GetGroupCount(), levelReason));
    EXPECT_NE(levelReason.find("maps to base curve"), std::string::npos) << levelReason;
}
