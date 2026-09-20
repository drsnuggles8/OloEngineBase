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
//   * the cook is deterministic and conserves the coat's total width, and it
//     refuses rather than truncates.
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
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomLod.h"
#include "OloEngine/Groom/GroomLodBuilder.h"

#include <cmath>
#include <numbers>
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

TEST(GroomLodCook, ACardCarriesItsClustersTotalWidthAndNeverCrossesAGroup)
{
    const Ref<GroomAsset> pelt = MakeTestPelt();
    ASSERT_TRUE(pelt);
    ASSERT_GT(pelt->GetGroupCount(), 1u) << "the fixture must have several groups for the group claim to mean anything";

    GroomCardSettings settings;
    GroomLodLevel level;
    GroomCardBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*pelt, settings, level, reason, &stats)) << reason;

    // The density claim, at the cook: no member's width was dropped.
    ASSERT_GT(stats.MemberWidthSum, 0.0);
    EXPECT_NEAR(stats.CardWidthSum / stats.MemberWidthSum, 1.0, 1.0e-3);
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
