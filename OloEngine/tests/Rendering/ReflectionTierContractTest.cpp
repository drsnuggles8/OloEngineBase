// OLO_TEST_LAYER: plumbing
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ReflectionTier.h"

#include <array>
#include <cmath>
#include <limits>
#include <vector>

// =============================================================================
// The reflection hierarchy's no-double-count ratchet — issue #1057, ADR 0020.
//
// #979's non-goal is "do not silently double-count DDGI/SSGI/RT/PT
// contributions", and the issue's own words are that such a double-count is
// "close to invisible in a still frame". A test that renders a scene and looks
// for it would therefore be the WEAKEST possible check. So this file does not
// render anything.
//
// Instead it asserts the algebraic property the contract is built on: every
// tier reports a confidence, the tiers composite bottom-up as an ordered "over"
// with the bottom tier pinned at 1, and the resulting effective weights sum to
// EXACTLY one for every possible confidence vector — not for a scene someone
// chose. A hierarchy with that property cannot double-count, and one without it
// eventually will, whatever any single frame looks like.
//
// That is what "energy conservation as a ratchet, not as a comment" means here:
// the sum is the invariant, and it is checked exhaustively over a swept grid
// plus the degenerate inputs (NaN, infinity, out-of-range) that a live edit, a
// script or an MCP write can actually deliver.
//
// The GLSL side mirrors this in PostProcess_SSRComposite.glsl's
// OloReflectionTierDebugColor and in each tier's own composite; the C++ here is
// the specification the shaders are written against.
// =============================================================================

namespace
{
    using OloEngine::ComputeReflectionTierWeights;
    using OloEngine::DominantReflectionTier;
    using OloEngine::kReflectionTierCount;
    using OloEngine::ReflectionTier;
    using OloEngine::ReflectionTierConfidences;

    [[nodiscard]] f32 SumWeights(const ReflectionTierConfidences& weights)
    {
        f32 total = 0.0f;
        for (const f32 weight : weights)
            total += weight;
        return total;
    }

    // A single f32 add per tier, so the accumulated error is a few ulps at most.
    constexpr f32 kSumTolerance = 1e-5f;
} // namespace

// -----------------------------------------------------------------------------
// The invariant itself
// -----------------------------------------------------------------------------

TEST(ReflectionTierContractTest, WeightsSumToOneAcrossTheWholeConfidenceGrid)
{
    // Every combination of four confidences on an 11-point grid: 14,641 cases,
    // which covers the boundaries (all zero, all one) and everything between.
    // Swept rather than sampled because the whole point of the contract is that
    // there is no confidence vector that breaks it.
    constexpr int kSteps = 10;
    int cases = 0;
    for (int a = 0; a <= kSteps; ++a)
    {
        for (int b = 0; b <= kSteps; ++b)
        {
            for (int c = 0; c <= kSteps; ++c)
            {
                for (int d = 0; d <= kSteps; ++d)
                {
                    const ReflectionTierConfidences confidences{
                        static_cast<f32>(a) / kSteps,
                        static_cast<f32>(b) / kSteps,
                        static_cast<f32>(c) / kSteps,
                        static_cast<f32>(d) / kSteps,
                    };
                    const auto weights = ComputeReflectionTierWeights(confidences);
                    ASSERT_NEAR(SumWeights(weights), 1.0f, kSumTolerance)
                        << "tier weights must sum to exactly 1 — confidences ("
                        << confidences[0] << ", " << confidences[1] << ", " << confidences[2] << ", "
                        << confidences[3] << ")";
                    ++cases;
                }
            }
        }
    }
    EXPECT_EQ(cases, 14641);
}

TEST(ReflectionTierContractTest, NoTierEverContributesANegativeOrOverUnitWeight)
{
    // A negative weight is energy SUBTRACTED from another tier, which is the
    // double-count's mirror image and just as invisible in a still frame.
    constexpr int kSteps = 8;
    for (int a = 0; a <= kSteps; ++a)
    {
        for (int b = 0; b <= kSteps; ++b)
        {
            for (int c = 0; c <= kSteps; ++c)
            {
                const ReflectionTierConfidences confidences{
                    static_cast<f32>(a) / kSteps,
                    static_cast<f32>(b) / kSteps,
                    static_cast<f32>(c) / kSteps,
                    1.0f,
                };
                for (const f32 weight : ComputeReflectionTierWeights(confidences))
                {
                    ASSERT_GE(weight, 0.0f);
                    ASSERT_LE(weight, 1.0f + kSumTolerance);
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// The hand-off rules the contract states in words
// -----------------------------------------------------------------------------

TEST(ReflectionTierContractTest, AConfidentTierTakesEverythingAndStarvesTheOnesBelow)
{
    // The motivating case, stated positively: where SSR is fully confident, the
    // ray-query tier and the probes contribute exactly nothing, so enabling the
    // ray tier cannot change a pixel SSR already owns.
    ReflectionTierConfidences confidences{};
    confidences[static_cast<u32>(ReflectionTier::Planar)] = 0.0f;
    confidences[static_cast<u32>(ReflectionTier::SSR)] = 1.0f;
    confidences[static_cast<u32>(ReflectionTier::RayQuery)] = 1.0f;
    confidences[static_cast<u32>(ReflectionTier::ProbeIBL)] = 1.0f;

    const auto weights = ComputeReflectionTierWeights(confidences);
    EXPECT_FLOAT_EQ(weights[static_cast<u32>(ReflectionTier::SSR)], 1.0f);
    EXPECT_FLOAT_EQ(weights[static_cast<u32>(ReflectionTier::RayQuery)], 0.0f);
    EXPECT_FLOAT_EQ(weights[static_cast<u32>(ReflectionTier::ProbeIBL)], 0.0f);
    EXPECT_EQ(DominantReflectionTier(confidences), ReflectionTier::SSR);
}

TEST(ReflectionTierContractTest, TheRayTierPicksUpExactlyWhatSSRLeavesAtTheScreenEdge)
{
    // THE ARTEFACT THIS ISSUE EXISTS TO REMOVE, in numbers. As an SSR ray
    // approaches the screen border its confidence fades to 0; today the pixel
    // falls to the low-frequency probes and the reflection visibly changes
    // character. Under the contract the ray-query tier takes precisely the share
    // SSR gave up — no more (that would double-count) and no less (that would
    // darken the seam).
    for (int step = 0; step <= 10; ++step)
    {
        const f32 ssrConfidence = static_cast<f32>(step) / 10.0f;
        ReflectionTierConfidences confidences{};
        confidences[static_cast<u32>(ReflectionTier::Planar)] = 0.0f;
        confidences[static_cast<u32>(ReflectionTier::SSR)] = ssrConfidence;
        confidences[static_cast<u32>(ReflectionTier::RayQuery)] = 1.0f; // a confident ray hit
        confidences[static_cast<u32>(ReflectionTier::ProbeIBL)] = 1.0f;

        const auto weights = ComputeReflectionTierWeights(confidences);
        EXPECT_NEAR(weights[static_cast<u32>(ReflectionTier::SSR)], ssrConfidence, kSumTolerance);
        EXPECT_NEAR(weights[static_cast<u32>(ReflectionTier::RayQuery)], 1.0f - ssrConfidence, kSumTolerance);
        EXPECT_NEAR(weights[static_cast<u32>(ReflectionTier::ProbeIBL)], 0.0f, kSumTolerance);
        EXPECT_NEAR(SumWeights(weights), 1.0f, kSumTolerance);
    }
}

TEST(ReflectionTierContractTest, ADisabledRayTierReproducesTodaysSSRToProbeHierarchyExactly)
{
    // ADR 0020 §5's promise, and the reason the raster-only output is
    // byte-identical when the tier is off: it is not a tested coincidence, it is
    // mix(x, y, 0) == x. Every weight must match the three-tier hierarchy that
    // shipped before this issue.
    for (int step = 0; step <= 10; ++step)
    {
        const f32 ssrConfidence = static_cast<f32>(step) / 10.0f;
        ReflectionTierConfidences withRayTier{};
        withRayTier[static_cast<u32>(ReflectionTier::SSR)] = ssrConfidence;
        withRayTier[static_cast<u32>(ReflectionTier::RayQuery)] = 0.0f; // unavailable / gated out
        withRayTier[static_cast<u32>(ReflectionTier::ProbeIBL)] = 1.0f;

        const auto weights = ComputeReflectionTierWeights(withRayTier);
        EXPECT_NEAR(weights[static_cast<u32>(ReflectionTier::RayQuery)], 0.0f, kSumTolerance);
        // Everything SSR did not claim lands on the probes, exactly as before.
        EXPECT_NEAR(weights[static_cast<u32>(ReflectionTier::SSR)], ssrConfidence, kSumTolerance);
        EXPECT_NEAR(weights[static_cast<u32>(ReflectionTier::ProbeIBL)], 1.0f - ssrConfidence, kSumTolerance);
    }
}

TEST(ReflectionTierContractTest, TheBottomTierTakesTheResidualEvenWhenItReportsLessThanOne)
{
    // ADR 0020 §7's single invariant. A probe/IBL tier allowed to "admit it does
    // not know" would leave energy unclaimed and darken the frame — so the
    // bottom tier's reported confidence is deliberately ignored. This test
    // exists to make that a decision rather than an accident: if someone later
    // makes the bottom tier's confidence meaningful, this fails loudly.
    ReflectionTierConfidences confidences{};
    confidences[static_cast<u32>(ReflectionTier::SSR)] = 0.25f;
    confidences[static_cast<u32>(ReflectionTier::RayQuery)] = 0.0f;
    confidences[static_cast<u32>(ReflectionTier::ProbeIBL)] = 0.0f; // "I don't know"

    const auto weights = ComputeReflectionTierWeights(confidences);
    EXPECT_NEAR(weights[static_cast<u32>(ReflectionTier::ProbeIBL)], 0.75f, kSumTolerance);
    EXPECT_NEAR(SumWeights(weights), 1.0f, kSumTolerance);
}

// -----------------------------------------------------------------------------
// Degenerate inputs — the ones that actually arrive
// -----------------------------------------------------------------------------

TEST(ReflectionTierContractTest, OutOfRangeAndNonFiniteConfidencesStillConserveEnergy)
{
    // These are not hypothetical. Confidences are assembled from artist settings
    // that arrive from a live edit, a script or an MCP write, and a NaN reaching
    // a weight would spread through bloom as a black block rather than as an
    // obvious error. The clamp inside ComputeReflectionTierWeights is the
    // backstop, and this is what pins it.
    const std::vector<f32> nasty{
        -1.0f,
        -0.0f,
        2.0f,
        1e30f,
        std::numeric_limits<f32>::infinity(),
        -std::numeric_limits<f32>::infinity(),
        std::numeric_limits<f32>::quiet_NaN(),
    };

    for (const f32 value : nasty)
    {
        for (u32 tier = 0; tier < kReflectionTierCount; ++tier)
        {
            ReflectionTierConfidences confidences{ 0.5f, 0.5f, 0.5f, 1.0f };
            confidences[tier] = value;

            const auto weights = ComputeReflectionTierWeights(confidences);
            f32 total = 0.0f;
            for (const f32 weight : weights)
            {
                ASSERT_TRUE(std::isfinite(weight)) << "tier " << tier << " weight is not finite";
                ASSERT_GE(weight, 0.0f);
                total += weight;
            }
            ASSERT_NEAR(total, 1.0f, kSumTolerance) << "tier " << tier << " broke conservation";
        }
    }
}

TEST(ReflectionTierContractTest, ANaNConfidenceIsTreatedAsNoAnswerRatherThanAFullOne)
{
    // The direction of the clamp matters and is not arbitrary: `NaN > 1.0f` and
    // `NaN < 0.0f` are both FALSE, so a naive clamp leaves the NaN in place.
    // ComputeReflectionTierWeights is written so the NaN branch lands on 0 —
    // "this tier answered nothing" — which degrades to the tier below instead of
    // letting a broken tier claim the whole pixel.
    ReflectionTierConfidences confidences{};
    confidences[static_cast<u32>(ReflectionTier::SSR)] = std::numeric_limits<f32>::quiet_NaN();
    confidences[static_cast<u32>(ReflectionTier::RayQuery)] = 0.0f;
    confidences[static_cast<u32>(ReflectionTier::ProbeIBL)] = 1.0f;

    const auto weights = ComputeReflectionTierWeights(confidences);
    EXPECT_FLOAT_EQ(weights[static_cast<u32>(ReflectionTier::SSR)], 0.0f);
    EXPECT_FLOAT_EQ(weights[static_cast<u32>(ReflectionTier::ProbeIBL)], 1.0f);
}

// -----------------------------------------------------------------------------
// The debug view's arbitration
// -----------------------------------------------------------------------------

TEST(ReflectionTierContractTest, TheDominantTierIsTheOneWithTheLargestWeight)
{
    {
        ReflectionTierConfidences confidences{ 0.0f, 0.0f, 0.0f, 1.0f };
        EXPECT_EQ(DominantReflectionTier(confidences), ReflectionTier::ProbeIBL);
    }
    {
        ReflectionTierConfidences confidences{ 0.0f, 0.1f, 0.9f, 1.0f };
        EXPECT_EQ(DominantReflectionTier(confidences), ReflectionTier::RayQuery);
    }
    {
        ReflectionTierConfidences confidences{ 1.0f, 1.0f, 1.0f, 1.0f };
        EXPECT_EQ(DominantReflectionTier(confidences), ReflectionTier::Planar);
    }
}

TEST(ReflectionTierContractTest, TheTierOrderIsBestInformedFirstAndIsLoadBearing)
{
    // The enum's numbering IS the composite order, so a reorder silently changes
    // which tier wins every contested pixel. Pinning it here makes that a
    // deliberate edit with a failing test attached.
    EXPECT_EQ(static_cast<u32>(ReflectionTier::Planar), 0u);
    EXPECT_EQ(static_cast<u32>(ReflectionTier::SSR), 1u);
    EXPECT_EQ(static_cast<u32>(ReflectionTier::RayQuery), 2u);
    EXPECT_EQ(static_cast<u32>(ReflectionTier::ProbeIBL), 3u);
    EXPECT_EQ(kReflectionTierCount, 4u);
}
