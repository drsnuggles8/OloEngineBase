#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomCompositionSelectionTest — issue #1246, acceptance criterion 4.
//
// "Capability fallbacks are explicit on OpenGL/Vulkan."
//
// SelectGroomComposition is the whole of that criterion's CPU half: it is the
// only place that decides what a groom actually gets, and the only place that
// can say why. So every reason is covered here with ONE input knocked out at a
// time, in the shape ShadowTechniqueSelectionTest established — a case that
// disabled two things at once would pass while reporting whichever reason the
// implementation happened to check first, which is precisely the drift these
// cases exist to catch.
//
// The fallback arm is also the arm CI runs: there is no GPU here, so the
// machine running this file is the machine that would take every fallback.
// That is the argument for putting the decision on the CPU at all.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Groom/GroomVisibility.h"

#include <set>
#include <string>

using namespace OloEngine;

namespace
{
    // The inputs of a frame that can do everything, so each case below can
    // remove exactly one capability and name the reason that removal causes.
    [[nodiscard]] GroomCompositionInputs FullyCapable()
    {
        GroomCompositionInputs inputs;
        inputs.TargetSampleCount = 4u;
        inputs.AlphaToCoverageSupported = true;
        inputs.TemporalResolveActive = true;
        inputs.OITTargetsAvailable = true;
        inputs.RequiresDepthComposition = true;
        return inputs;
    }
} // namespace

// ── The happy paths ─────────────────────────────────────────────────────────

TEST(GroomCompositionSelection, ACapableFrameGrantsEveryDepthWritingMode)
{
    for (const auto requested : { GroomCompositionMode::OpaqueRibbon, GroomCompositionMode::StochasticAlpha,
                                  GroomCompositionMode::AlphaToCoverage })
    {
        GroomCompositionInputs inputs = FullyCapable();
        inputs.Requested = requested;
        const GroomCompositionDecision decision = SelectGroomComposition(inputs);
        EXPECT_EQ(decision.Effective, requested) << ToString(requested) << " was refused by a capable frame";
        EXPECT_FALSE(decision.IsFallback()) << ToString(decision.Reason);
    }
}

TEST(GroomCompositionSelection, AlphaToCoverageCarriesTheTargetSampleCountForward)
{
    for (const u32 samples : { 2u, 4u, 8u })
    {
        GroomCompositionInputs inputs = FullyCapable();
        inputs.Requested = GroomCompositionMode::AlphaToCoverage;
        inputs.TargetSampleCount = samples;
        const GroomCompositionDecision decision = SelectGroomComposition(inputs);
        ASSERT_EQ(decision.Effective, GroomCompositionMode::AlphaToCoverage);
        // A caller sizing a sample-mask table reads this rather than
        // re-deriving it; if the two ever disagree the mask is built for the
        // wrong width and the coverage is silently wrong.
        EXPECT_EQ(decision.EffectiveSampleCount, samples);
    }
}

TEST(GroomCompositionSelection, EveryModeOtherThanAlphaToCoverageResolvesAtOneSample)
{
    for (const auto requested : { GroomCompositionMode::OpaqueRibbon, GroomCompositionMode::StochasticAlpha })
    {
        GroomCompositionInputs inputs = FullyCapable();
        inputs.Requested = requested;
        EXPECT_EQ(SelectGroomComposition(inputs).EffectiveSampleCount, 1u) << ToString(requested);
    }
}

// ── One capability removed at a time ────────────────────────────────────────

TEST(GroomCompositionSelection, AlphaToCoverageWithoutBackendSupportIsNamedAsSuch)
{
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = GroomCompositionMode::AlphaToCoverage;
    inputs.AlphaToCoverageSupported = false;
    const GroomCompositionDecision decision = SelectGroomComposition(inputs);
    EXPECT_EQ(decision.Effective, GroomCompositionMode::OpaqueRibbon);
    EXPECT_EQ(decision.Reason, GroomCompositionFallbackReason::AlphaToCoverageUnimplemented);
    EXPECT_TRUE(decision.IsFallback());
}

TEST(GroomCompositionSelection, AlphaToCoverageOnASingleSampleTargetIsNamedAsSuch)
{
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = GroomCompositionMode::AlphaToCoverage;
    inputs.TargetSampleCount = 1u;
    const GroomCompositionDecision decision = SelectGroomComposition(inputs);
    EXPECT_EQ(decision.Effective, GroomCompositionMode::OpaqueRibbon);
    EXPECT_EQ(decision.Reason, GroomCompositionFallbackReason::MultisampleTargetUnavailable);
    // This is the forward / Forward+ case, and it is the reason the analysis
    // gives for not selecting alpha-to-coverage as the production mode: four
    // of the six {backend} x {path} cells reach this line.
    EXPECT_EQ(decision.EffectiveSampleCount, 1u);
}

TEST(GroomCompositionSelection, BackendSupportIsCheckedBeforeTheTarget)
{
    // Both broken at once. The reason reported must be the more fundamental
    // one, or the counter tells a user to raise their MSAA setting on a device
    // that could never have honoured it.
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = GroomCompositionMode::AlphaToCoverage;
    inputs.AlphaToCoverageSupported = false;
    inputs.TargetSampleCount = 1u;
    EXPECT_EQ(SelectGroomComposition(inputs).Reason, GroomCompositionFallbackReason::AlphaToCoverageUnimplemented);
}

TEST(GroomCompositionSelection, StochasticAlphaWithoutATemporalResolveIsRefused)
{
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = GroomCompositionMode::StochasticAlpha;
    inputs.TemporalResolveActive = false;
    const GroomCompositionDecision decision = SelectGroomComposition(inputs);
    EXPECT_EQ(decision.Effective, GroomCompositionMode::OpaqueRibbon)
        << "stochastic coverage was granted with nothing to converge it; a noisy coat would ship silently";
    EXPECT_EQ(decision.Reason, GroomCompositionFallbackReason::TemporalResolveUnavailable);
}

TEST(GroomCompositionSelection, WeightedBlendedOITWithoutTargetsIsNamedAsSuch)
{
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = GroomCompositionMode::WeightedBlendedOIT;
    inputs.RequiresDepthComposition = false;
    inputs.OITTargetsAvailable = false;
    const GroomCompositionDecision decision = SelectGroomComposition(inputs);
    EXPECT_EQ(decision.Effective, GroomCompositionMode::OpaqueRibbon);
    EXPECT_EQ(decision.Reason, GroomCompositionFallbackReason::OITTargetsUnavailable);
}

TEST(GroomCompositionSelection, WeightedBlendedOITIsRefusedWhenDepthCompositionIsNeeded)
{
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = GroomCompositionMode::WeightedBlendedOIT;
    const GroomCompositionDecision decision = SelectGroomComposition(inputs);
    EXPECT_EQ(decision.Effective, GroomCompositionMode::OpaqueRibbon);
    EXPECT_EQ(decision.Reason, GroomCompositionFallbackReason::DepthCompositionRequired);
    // The property the refusal rests on, asserted here so the refusal cannot
    // outlive its own reason.
    EXPECT_FALSE(GroomModeWritesDepth(GroomCompositionMode::WeightedBlendedOIT));
}

TEST(GroomCompositionSelection, WeightedBlendedOITIsGrantedToACallerThatDoesNotNeedDepth)
{
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = GroomCompositionMode::WeightedBlendedOIT;
    inputs.RequiresDepthComposition = false;
    const GroomCompositionDecision decision = SelectGroomComposition(inputs);
    EXPECT_EQ(decision.Effective, GroomCompositionMode::WeightedBlendedOIT);
    EXPECT_FALSE(decision.IsFallback());
}

// ── Properties of the whole seam ────────────────────────────────────────────

TEST(GroomCompositionSelection, OpaqueRibbonIsGrantedByAFrameThatCanDoNothingElse)
{
    // The always-available claim, stated as the case that proves it: every
    // capability off, and the request still honoured. Everything above depends
    // on this being true.
    GroomCompositionInputs inputs;
    inputs.Requested = GroomCompositionMode::OpaqueRibbon;
    inputs.TargetSampleCount = 1u;
    inputs.AlphaToCoverageSupported = false;
    inputs.TemporalResolveActive = false;
    inputs.OITTargetsAvailable = false;
    const GroomCompositionDecision decision = SelectGroomComposition(inputs);
    EXPECT_EQ(decision.Effective, GroomCompositionMode::OpaqueRibbon);
    EXPECT_FALSE(decision.IsFallback()) << "the baseline tier was reported as a failure";
    EXPECT_EQ(decision.Reason, GroomCompositionFallbackReason::NotRequested);
}

TEST(GroomCompositionSelection, EveryRefusalFallsBackToTheAlwaysAvailableTier)
{
    // No refusal may degrade to a *different* partially-capable mode: a chain
    // of partial degradations produces an appearance nobody chose. Swept over
    // every combination of the four capability booleans and every request.
    for (u32 bits = 0; bits < 16u; ++bits)
    {
        for (const auto requested : { GroomCompositionMode::OpaqueRibbon, GroomCompositionMode::StochasticAlpha,
                                      GroomCompositionMode::AlphaToCoverage,
                                      GroomCompositionMode::WeightedBlendedOIT })
        {
            GroomCompositionInputs inputs;
            inputs.Requested = requested;
            inputs.AlphaToCoverageSupported = (bits & 1u) != 0u;
            inputs.TemporalResolveActive = (bits & 2u) != 0u;
            inputs.OITTargetsAvailable = (bits & 4u) != 0u;
            inputs.RequiresDepthComposition = (bits & 8u) != 0u;
            inputs.TargetSampleCount = ((bits & 1u) != 0u) ? 4u : 1u;

            const GroomCompositionDecision decision = SelectGroomComposition(inputs);
            if (decision.IsFallback())
            {
                EXPECT_EQ(decision.Effective, GroomCompositionMode::OpaqueRibbon)
                    << "request " << ToString(requested) << " fell back to " << ToString(decision.Effective);
                EXPECT_EQ(decision.EffectiveSampleCount, 1u);
            }
            else
            {
                EXPECT_EQ(decision.Effective, requested) << "a non-fallback decision changed the mode";
            }
        }
    }
}

TEST(GroomCompositionSelection, ACorruptRequestIsRefusedRatherThanReadAsTheBaseline)
{
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = static_cast<GroomCompositionMode>(200);
    const GroomCompositionDecision decision = SelectGroomComposition(inputs);
    EXPECT_EQ(decision.Effective, GroomCompositionMode::OpaqueRibbon);
    // Reported as a fallback, not as NotRequested: a corrupt component field
    // is something to fix, and folding it into "the user asked for the
    // baseline" is how it would never be noticed.
    EXPECT_TRUE(decision.IsFallback());
    // And with its OWN reason, not a borrowed capability one. Reporting this
    // as AlphaToCoverageUnimplemented sent a reader to fix a device that was
    // working perfectly well.
    EXPECT_EQ(decision.Reason, GroomCompositionFallbackReason::RequestedModeInvalid);
}

TEST(GroomCompositionSelection, TheSelectionIsUsableInAConstantExpression)
{
    // The header claims constexpr, and a claim nothing evaluates at compile
    // time is a claim that quietly stops being true the first time someone
    // reaches for renderer state from inside the function.
    constexpr GroomCompositionInputs inputs{ GroomCompositionMode::StochasticAlpha, 1u, false, true, false, true };
    constexpr GroomCompositionDecision decision = SelectGroomComposition(inputs);
    static_assert(decision.Effective == GroomCompositionMode::StochasticAlpha);
    static_assert(!decision.IsFallback());
    SUCCEED();
}

// ── The reason strings ──────────────────────────────────────────────────────

TEST(GroomCompositionSelection, EveryReasonHasADistinctNonEmptySentence)
{
    std::set<std::string> seen;
    for (u32 i = 0; i < static_cast<u32>(GroomCompositionFallbackReason::Count); ++i)
    {
        const auto reason = static_cast<GroomCompositionFallbackReason>(i);
        const std::string text{ ToString(reason) };
        EXPECT_FALSE(text.empty()) << "reason " << i;
        EXPECT_NE(text, "unknown") << "reason " << i << " has no sentence, so its counter is unactionable";
        EXPECT_TRUE(seen.insert(text).second) << "reason " << i << " repeats an earlier sentence: " << text;
    }
}

TEST(GroomCompositionSelection, EveryModeHasADistinctName)
{
    std::set<std::string> seen;
    for (u32 i = 0; i < static_cast<u32>(GroomCompositionMode::Count); ++i)
    {
        const std::string text{ ToString(static_cast<GroomCompositionMode>(i)) };
        EXPECT_NE(text, "Unknown") << "mode " << i;
        EXPECT_TRUE(seen.insert(text).second) << "mode " << i << " repeats an earlier name: " << text;
        EXPECT_TRUE(IsValidGroomCompositionMode(static_cast<i32>(i)));
    }
    EXPECT_FALSE(IsValidGroomCompositionMode(static_cast<i32>(GroomCompositionMode::Count)));
    EXPECT_FALSE(IsValidGroomCompositionMode(-1));
}

// ── Counters ────────────────────────────────────────────────────────────────

TEST(GroomCompositionStatsTest, SeparatesFailedToDeliverFromNeverAsked)
{
    GroomCompositionStats stats;

    GroomCompositionInputs baseline;
    baseline.Requested = GroomCompositionMode::OpaqueRibbon;
    stats.Record(SelectGroomComposition(baseline));
    stats.Record(SelectGroomComposition(baseline));

    GroomCompositionInputs refused = FullyCapable();
    refused.Requested = GroomCompositionMode::StochasticAlpha;
    refused.TemporalResolveActive = false;
    stats.Record(SelectGroomComposition(refused));

    EXPECT_EQ(stats.GroomsConsidered, 3u);
    // The two baseline grooms are NOT failures. A scene full of them must not
    // read as a scene full of broken grooms.
    EXPECT_EQ(stats.GroomsFellBack, 1u);
    EXPECT_EQ(stats.GroomsOnRequestedMode, 2u);
    EXPECT_EQ(stats.ByReason[static_cast<sizet>(GroomCompositionFallbackReason::NotRequested)], 2u);
    EXPECT_EQ(stats.DominantFallbackReason(), GroomCompositionFallbackReason::TemporalResolveUnavailable);
}

TEST(GroomCompositionStatsTest, DominantReasonIsNoneWhenNothingActuallyFellBack)
{
    GroomCompositionStats stats;
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = GroomCompositionMode::AlphaToCoverage;
    stats.Record(SelectGroomComposition(inputs));
    inputs.Requested = GroomCompositionMode::OpaqueRibbon;
    stats.Record(SelectGroomComposition(inputs));

    EXPECT_EQ(stats.GroomsFellBack, 0u);
    EXPECT_EQ(stats.DominantFallbackReason(), GroomCompositionFallbackReason::None);
}

TEST(GroomCompositionStatsTest, TiesBreakTowardTheMoreFundamentalReason)
{
    GroomCompositionStats stats;

    GroomCompositionInputs unsupported = FullyCapable();
    unsupported.Requested = GroomCompositionMode::AlphaToCoverage;
    unsupported.AlphaToCoverageSupported = false;
    stats.Record(SelectGroomComposition(unsupported));

    GroomCompositionInputs noResolve = FullyCapable();
    noResolve.Requested = GroomCompositionMode::StochasticAlpha;
    noResolve.TemporalResolveActive = false;
    stats.Record(SelectGroomComposition(noResolve));

    // One of each. The answer must name the cause a user could act on first.
    EXPECT_EQ(stats.DominantFallbackReason(), GroomCompositionFallbackReason::AlphaToCoverageUnimplemented);
}

TEST(GroomCompositionStatsTest, ResetClearsEveryCounter)
{
    GroomCompositionStats stats;
    GroomCompositionInputs inputs = FullyCapable();
    inputs.Requested = GroomCompositionMode::StochasticAlpha;
    inputs.TemporalResolveActive = false;
    stats.Record(SelectGroomComposition(inputs));
    ASSERT_NE(stats, GroomCompositionStats{});
    stats.Reset();
    EXPECT_EQ(stats, GroomCompositionStats{});
}
