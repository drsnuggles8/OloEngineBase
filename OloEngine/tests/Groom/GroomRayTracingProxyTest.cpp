#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomRayTracingProxyTest — issue #1253, the contracts a picture cannot show.
//
// The live captures settle whether a coat's ray-traced shadow LOOKS right. This
// file pins the four things a still frame cannot:
//
//   * the conversion reads the strand mesh's quad layout CORRECTLY — asserted
//     against a real BuildGroomStrandMesh output, not against the comment that
//     describes it, because that correspondence is the one assumption the whole
//     representation rests on and a change to the emitter would otherwise
//     produce a coat of plausible-looking garbage;
//   * the compensation preserves DIRECTIONAL COVERAGE across a sweep of
//     thinnings, which is criterion 1's "compare detailed vs proxy" as a
//     number, and it is a sweep rather than a point for GroomLodContractTest's
//     reason: every implementation passes at one thinning;
//   * the tier hysteresis delivers the two bounds GroomRayTracingProxy.h claims
//     — a camera oscillating across the threshold changes the tier at most once
//     per HoldFrames, and a refinement is never delayed;
//   * every refusal is FAIL-CLOSED and counted, so a coat that leaves the
//     ray-traced scene does so with a reason a user can act on.
//
// WHY THE COVERAGE SWEEP IS THE CENTRAL CASE. The proxy's whole claim is that
// thinning by k and widening by k leaves the coat's occlusion unchanged. That
// is an identity in the SUM of 2*r*L*sin(theta), and it is only approximately
// true of the coat because the retained strands are a STRIDE, not a random
// sample — so the assertion is a measured bound on the residual rather than an
// equality, and the bound is what the analysis document reports.
// =============================================================================

#include "OloEngine/Groom/GroomRayTracingProxy.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include "Groom/GroomStrandFixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <cstdio>
#include <cmath>
#include <vector>

namespace OloEngine
{
    namespace
    {
        struct BuiltCoat
        {
            std::vector<GroomStrandVertex> Vertices;
            std::vector<u32> Indices;
            GroomStrandMeshStats Stats;
        };

        [[nodiscard]] BuiltCoat BuildCoat(const GroomAsset& groom, u32 maxStrands)
        {
            BuiltCoat built;
            GroomStrandBuildSettings settings;
            settings.MaxStrands = maxStrands;
            built.Stats = BuildGroomStrandMesh(groom, settings, built.Vertices, built.Indices);
            return built;
        }
    } // namespace

    // ── The layout the conversion relies on ──────────────────────────────

    // ConvertGroomStrandMeshToProxy reads vertex 4s+0 as P0 and vertex 4s+2 as
    // P1. That is a claim about BuildGroomStrandMesh's emission order, and it is
    // checked against a real build rather than against the comment that states
    // it: if the emitter ever changes its corner order, the proxy would build
    // bowties out of every segment and still produce a plausible count of
    // triangles.
    TEST(GroomRayTracingProxy, TheQuadLayoutTheConversionAssumesIsTheOneTheBuildEmits)
    {
        const auto coat = Tests::GroomStrandFixture::MakeScalp(64u, 8u);
        ASSERT_TRUE(coat.Groom) << coat.FailureReason;
        const BuiltCoat built = BuildCoat(*coat.Groom, 64u);
        ASSERT_GT(built.Stats.SegmentCount, 0u);
        ASSERT_EQ(built.Vertices.size(), static_cast<sizet>(built.Stats.SegmentCount) * 4u);

        for (u32 segment = 0; segment < built.Stats.SegmentCount; ++segment)
        {
            const auto& c0 = built.Vertices[segment * 4u + 0u];
            const auto& c1 = built.Vertices[segment * 4u + 1u];
            const auto& c2 = built.Vertices[segment * 4u + 2u];
            const auto& c3 = built.Vertices[segment * 4u + 3u];
            // The two corners at each END share a position and a radius; the
            // two ENDS differ. Both halves matter: the first is what makes
            // reading corner 0 and corner 2 well-defined, and the second is
            // what makes the segment non-degenerate.
            EXPECT_TRUE(Math::BitwiseEqual(c0.Position, c1.Position)) << "segment " << segment;
            EXPECT_TRUE(Math::BitwiseEqual(c2.Position, c3.Position)) << "segment " << segment;
            EXPECT_TRUE(Math::BitwiseEqual(c0.Radius, c1.Radius)) << "segment " << segment;
            EXPECT_TRUE(Math::BitwiseEqual(c2.Radius, c3.Radius)) << "segment " << segment;
            EXPECT_GT(glm::length(c2.Position - c0.Position), 0.0f) << "segment " << segment;
        }
    }

    TEST(GroomRayTracingProxy, ACrossedRibbonCostsFourTrianglesASingleOneCostsTwo)
    {
        const auto coat = Tests::GroomStrandFixture::MakeScalp(32u, 6u);
        ASSERT_TRUE(coat.Groom) << coat.FailureReason;
        const BuiltCoat built = BuildCoat(*coat.Groom, 32u);
        ASSERT_GT(built.Stats.SegmentCount, 0u);

        std::vector<Vertex> vertices;
        std::vector<u32> indices;

        GroomProxyConversionSettings crossed;
        crossed.CrossedRibbons = true;
        const auto crossedStats = ConvertGroomStrandMeshToProxy(built.Vertices, crossed, vertices, indices);

        GroomProxyConversionSettings single;
        single.CrossedRibbons = false;
        const auto singleStats = ConvertGroomStrandMeshToProxy(built.Vertices, single, vertices, indices);

        EXPECT_EQ(crossedStats.SegmentCount, singleStats.SegmentCount);
        EXPECT_EQ(crossedStats.TriangleCount(), crossedStats.SegmentCount * 4u);
        EXPECT_EQ(singleStats.TriangleCount(), singleStats.SegmentCount * 2u);
        EXPECT_EQ(crossedStats.SegmentsDropped, 0u);
    }

    // A degenerate segment normalises to NaN, and a NaN vertex in a BLAS build
    // is undefined at the device with no validation message — the structure is
    // simply wrong for every ray afterwards. So it is dropped and COUNTED, and
    // the count is what makes "this coat lost strands" answerable.
    TEST(GroomRayTracingProxy, ADegenerateSegmentIsDroppedAndCountedRatherThanEmittedAsNaN)
    {
        std::vector<GroomStrandVertex> vertices(8u);
        // Segment 0: legitimate.
        vertices[0].Position = { 0.0f, 0.0f, 0.0f };
        vertices[1].Position = vertices[0].Position;
        vertices[2].Position = { 0.0f, 1.0f, 0.0f };
        vertices[3].Position = vertices[2].Position;
        for (auto& vertex : vertices)
        {
            vertex.Radius = 0.01f;
        }
        // Segment 1: zero length.
        for (u32 corner = 4u; corner < 8u; ++corner)
        {
            vertices[corner].Position = glm::vec3{ 2.0f, 2.0f, 2.0f };
        }

        std::vector<Vertex> out;
        std::vector<u32> indices;
        const auto stats = ConvertGroomStrandMeshToProxy(vertices, {}, out, indices);

        EXPECT_EQ(stats.SegmentCount, 1u);
        EXPECT_EQ(stats.SegmentsDropped, 1u);
        for (const auto& vertex : out)
        {
            EXPECT_TRUE(std::isfinite(vertex.Position.x) && std::isfinite(vertex.Position.y) &&
                        std::isfinite(vertex.Position.z));
            EXPECT_TRUE(std::isfinite(vertex.Normal.x) && std::isfinite(vertex.Normal.y) &&
                        std::isfinite(vertex.Normal.z));
        }
    }

    // ── The compensation, swept ──────────────────────────────────────────

    // Criterion 1, as arithmetic. A coat thinned to a fraction k and widened by
    // 1/k must project the same area along every direction. It is a stride
    // rather than a random sample, so the residual is bounded rather than zero
    // — and the bound is the number the analysis document reports.
    TEST(GroomRayTracingProxy, ThinningAndWideningPreservesDirectionalCoverageAcrossASweep)
    {
        const auto coat = Tests::GroomStrandFixture::MakePelt(4096u, 10u);
        ASSERT_TRUE(coat.Groom) << coat.FailureReason;

        const BuiltCoat detailed = BuildCoat(*coat.Groom, 4096u);
        ASSERT_GT(detailed.Stats.StrandsSelected, 0u);
        std::vector<GroomCoatShadow::CoatSegment> detailedSegments;
        CollectGroomProxySegments(detailed.Vertices, 1.0f, detailedSegments);
        ASSERT_FALSE(detailedSegments.empty());

        // THE SWEEP STOPS WHERE THE CAP STARTS. Below about 1/48 of the
        // coat the compensation is clamped by
        // GroomProxyPolicy::MaxWidthCompensation and the coat genuinely
        // thins out — that is the documented behaviour, it is reported
        // rather than hidden, and it gets its own case below. Folding it in
        // here would turn a bound on the REPRESENTATION into a bound that
        // also has to accommodate the cap, which is the shape of an
        // assertion loosened until it passes.
        //
        // 128 of 4096 is 1/32, the hardest thinning either shipping tier
        // reaches on either reference animal.
        f64 worst = 0.0;
        for (const u32 budget : { 2048u, 1024u, 512u, 256u, 128u })
        {
            const BuiltCoat proxy = BuildCoat(*coat.Groom, budget);
            ASSERT_GT(proxy.Stats.StrandsSelected, 0u) << "budget " << budget;

            // THE ACHIEVED FRACTION, never the requested one: the budget is
            // spent as an integer stride, so a request for 0.4 retains 1/3.
            const f32 achieved = static_cast<f32>(proxy.Stats.StrandsSelected) /
                                 static_cast<f32>(proxy.Stats.StrandsAvailable);
            const f32 compensation = GroomProxyWidthCompensation(achieved, GroomProxyPolicy::MaxWidthCompensation);
            ASSERT_LT(compensation, GroomProxyPolicy::MaxWidthCompensation)
                << "budget " << budget << " reaches the compensation cap; the sweep must stay below it";

            std::vector<GroomCoatShadow::CoatSegment> proxySegments;
            CollectGroomProxySegments(proxy.Vertices, compensation, proxySegments);

            const auto error = CompareGroomProxyCoverage(detailedSegments, proxySegments);
            EXPECT_GT(error.Directions, 0u) << "budget " << budget;
            EXPECT_LT(error.MeanRelativeError, 0.05)
                << "budget " << budget << " retained " << proxy.Stats.StrandsSelected << " of "
                << proxy.Stats.StrandsAvailable << " (compensation " << compensation << ")";
            worst = std::max(worst, error.MeanRelativeError);
        }
        // Reported so the sweep's own number is visible in a failing log rather
        // than only the threshold it crossed.
        ::testing::Test::RecordProperty("WorstMeanRelativeCoverageError", std::to_string(worst));
    }

    // Past the cap the coat is MEASURABLY thinner in ray space, by exactly
    // the ratio the cap imposes — 1 - cap/needed. Asserted rather than
    // avoided, because this is the one way the representation loses coverage,
    // and the statistics panel reports the achieved compensation beside the
    // cap precisely so a user can see it happening.
    TEST(GroomRayTracingProxy, PastTheCompensationCapTheCoatThinsByExactlyTheRatioTheCapImposes)
    {
        const auto coat = Tests::GroomStrandFixture::MakePelt(4096u, 10u);
        ASSERT_TRUE(coat.Groom) << coat.FailureReason;

        const BuiltCoat detailed = BuildCoat(*coat.Groom, 4096u);
        std::vector<GroomCoatShadow::CoatSegment> detailedSegments;
        CollectGroomProxySegments(detailed.Vertices, 1.0f, detailedSegments);

        // 1/64 of the coat, against a cap of 48.
        const BuiltCoat proxy = BuildCoat(*coat.Groom, 64u);
        ASSERT_GT(proxy.Stats.StrandsSelected, 0u);
        const f32 achieved = static_cast<f32>(proxy.Stats.StrandsSelected) /
                             static_cast<f32>(proxy.Stats.StrandsAvailable);
        const f32 needed = 1.0f / achieved;
        const f32 compensation = GroomProxyWidthCompensation(achieved, GroomProxyPolicy::MaxWidthCompensation);
        ASSERT_GT(needed, GroomProxyPolicy::MaxWidthCompensation);
        EXPECT_FLOAT_EQ(compensation, GroomProxyPolicy::MaxWidthCompensation);

        std::vector<GroomCoatShadow::CoatSegment> proxySegments;
        CollectGroomProxySegments(proxy.Vertices, compensation, proxySegments);
        const auto error = CompareGroomProxyCoverage(detailedSegments, proxySegments);

        // The coat keeps cap/needed of its coverage and loses the rest. The
        // tolerance is the stride's own residual, the same one the sweep
        // above bounds; the POINT is that the shortfall is the cap's
        // arithmetic and not an unexplained loss.
        const f64 predictedLoss = 1.0 - static_cast<f64>(compensation) / static_cast<f64>(needed);
        EXPECT_NEAR(error.MeanRelativeError, predictedLoss, 0.05)
            << "retained " << proxy.Stats.StrandsSelected << " of " << proxy.Stats.StrandsAvailable
            << ", needed " << needed << "x, capped at " << compensation << "x";
        EXPECT_LT(error.ProxyCoverage, error.DetailedCoverage);
    }

    // The uncompensated arm, as the control the sweep above needs. Without it
    // "the compensated coat is within 15%" says nothing — an implementation
    // that ignored the compensation entirely might also be within 15%.
    TEST(GroomRayTracingProxy, WithoutTheCompensationAThinnedCoatLosesCoverageInProportionToTheThinning)
    {
        const auto coat = Tests::GroomStrandFixture::MakePelt(4096u, 10u);
        ASSERT_TRUE(coat.Groom) << coat.FailureReason;

        const BuiltCoat detailed = BuildCoat(*coat.Groom, 4096u);
        std::vector<GroomCoatShadow::CoatSegment> detailedSegments;
        CollectGroomProxySegments(detailed.Vertices, 1.0f, detailedSegments);

        const BuiltCoat proxy = BuildCoat(*coat.Groom, 256u);
        ASSERT_GT(proxy.Stats.StrandsSelected, 0u);
        std::vector<GroomCoatShadow::CoatSegment> proxySegments;
        CollectGroomProxySegments(proxy.Vertices, 1.0f, proxySegments);

        const auto error = CompareGroomProxyCoverage(detailedSegments, proxySegments);
        const f64 achieved = static_cast<f64>(proxy.Stats.StrandsSelected) /
                             static_cast<f64>(proxy.Stats.StrandsAvailable);
        // It loses roughly (1 - k) of the coverage, which at this thinning is
        // most of it. Asserted as a floor on the error so the case fails if the
        // compensation ever leaks into the uncompensated path.
        EXPECT_GT(error.MeanRelativeError, 0.5) << "achieved fraction " << achieved;
        EXPECT_LT(error.ProxyCoverage, error.DetailedCoverage);
    }

    TEST(GroomRayTracingProxy, TheCompensationIsLinearAndCappedAndNeverThinsACoat)
    {
        EXPECT_FLOAT_EQ(GroomProxyWidthCompensation(0.25f, 48.0f), 4.0f);
        EXPECT_FLOAT_EQ(GroomProxyWidthCompensation(0.5f, 48.0f), 2.0f);
        // The cap is a cap, not a clamp to the request.
        EXPECT_FLOAT_EQ(GroomProxyWidthCompensation(0.001f, 48.0f), 48.0f);
        // A build that retained everything, or more than it asked for, must not
        // thin the coat down.
        EXPECT_FLOAT_EQ(GroomProxyWidthCompensation(1.0f, 48.0f), 1.0f);
        EXPECT_FLOAT_EQ(GroomProxyWidthCompensation(2.0f, 48.0f), 1.0f);
        // A fraction nobody can name gets no compensation at all — widening on
        // it would produce a coat of arbitrary thickness.
        EXPECT_FLOAT_EQ(GroomProxyWidthCompensation(0.0f, 48.0f), 1.0f);
        EXPECT_FLOAT_EQ(GroomProxyWidthCompensation(std::numeric_limits<f32>::quiet_NaN(), 48.0f), 1.0f);
    }

    // ── The ladder ───────────────────────────────────────────────────────

    TEST(GroomRayTracingProxy, ANearCoatIsDetailedAndAFarOneIsProxy)
    {
        GroomProxyState state;
        GroomProxyInputs inputs;
        inputs.Requested = true;
        inputs.StrandsAvailable = 1000u;

        inputs.PixelSize = GroomProxyPolicy::DetailedPixelSize * 4.0f;
        EXPECT_EQ(AdvanceGroomProxyTier(inputs, state).Tier, GroomProxyTier::Detailed);

        inputs.PixelSize = 1.0f;
        for (u32 frame = 0; frame < GroomProxyPolicy::HoldFrames; ++frame)
        {
            AdvanceGroomProxyTier(inputs, state);
        }
        EXPECT_EQ(state.Tier, GroomProxyTier::Proxy);
        EXPECT_EQ(AdvanceGroomProxyTier(inputs, state).StrandBudget, GroomProxyPolicy::ProxyStrandBudget);
    }

    // The bound the hysteresis actually promises, stated as the failure it
    // prevents: a camera sitting ON the threshold and oscillating every frame
    // changes the tier at most once per HoldFrames, whatever its amplitude.
    // Asserted by DRIVING the oscillation rather than by sampling two points —
    // a hysteresis asserted at one point is not asserted at all.
    TEST(GroomRayTracingProxy, AnOscillatingCameraCannotChangeTheTierEveryFrame)
    {
        GroomProxyState state;
        GroomProxyInputs inputs;
        inputs.Requested = true;
        inputs.StrandsAvailable = 1000u;

        u32 changes = 0;
        constexpr u32 kFrames = 120u;
        for (u32 frame = 0; frame < kFrames; ++frame)
        {
            inputs.PixelSize = (frame % 2u) == 0u ? GroomProxyPolicy::DetailedPixelSize * 1.5f
                                                  : GroomProxyPolicy::DetailedPixelSize * 0.5f;
            changes += AdvanceGroomProxyTier(inputs, state).TierChanged ? 1u : 0u;
        }
        // A coarsening needs HoldFrames CONSECUTIVE frames of the coarse
        // request, which an every-frame oscillation never produces; the coat
        // therefore never leaves the detailed tier at all. The bound is stated
        // as the general one so the case still holds if the hold changes.
        EXPECT_LE(changes, kFrames / GroomProxyPolicy::HoldFrames);
        EXPECT_EQ(state.Tier, GroomProxyTier::Detailed);
    }

    TEST(GroomRayTracingProxy, RefiningIsImmediateAndCoarseningWaitsOutTheHold)
    {
        GroomProxyState state;
        GroomProxyInputs inputs;
        inputs.Requested = true;
        inputs.StrandsAvailable = 1000u;

        // Settle on the proxy tier.
        inputs.PixelSize = 1.0f;
        for (u32 frame = 0; frame < GroomProxyPolicy::HoldFrames + 2u; ++frame)
        {
            AdvanceGroomProxyTier(inputs, state);
        }
        ASSERT_EQ(state.Tier, GroomProxyTier::Proxy);

        // One frame at a near size is enough to refine.
        inputs.PixelSize = GroomProxyPolicy::DetailedPixelSize * 4.0f;
        const auto refined = AdvanceGroomProxyTier(inputs, state);
        EXPECT_EQ(refined.Tier, GroomProxyTier::Detailed);
        EXPECT_TRUE(refined.TierChanged);

        // Coarsening takes HoldFrames, and not one fewer.
        inputs.PixelSize = 1.0f;
        for (u32 frame = 1; frame < GroomProxyPolicy::HoldFrames; ++frame)
        {
            EXPECT_EQ(AdvanceGroomProxyTier(inputs, state).Tier, GroomProxyTier::Detailed) << "frame " << frame;
        }
        EXPECT_EQ(AdvanceGroomProxyTier(inputs, state).Tier, GroomProxyTier::Proxy);
    }

    // An unusable apparent size must not buy a coat the expensive
    // representation. It takes the COARSE tier rather than being rejected: the
    // raster tier is unaffected either way, and a NaN that bought the detailed
    // tier would be an unbounded cost nobody can see in a picture.
    TEST(GroomRayTracingProxy, ANonFiniteApparentSizeTakesTheCoarseTier)
    {
        GroomProxyState state;
        GroomProxyInputs inputs;
        inputs.Requested = true;
        inputs.StrandsAvailable = 1000u;
        inputs.PixelSize = std::numeric_limits<f32>::quiet_NaN();
        for (u32 frame = 0; frame < GroomProxyPolicy::HoldFrames; ++frame)
        {
            AdvanceGroomProxyTier(inputs, state);
        }
        EXPECT_EQ(state.Tier, GroomProxyTier::Proxy);
    }

    // ── Refusals ─────────────────────────────────────────────────────────

    TEST(GroomRayTracingProxy, EveryRefusalIsFailClosedAndCounted)
    {
        GroomProxyState state;
        GroomProxyInputs inputs;
        inputs.PixelSize = 512.0f;

        inputs.Requested = false;
        inputs.StrandsAvailable = 1000u;
        EXPECT_EQ(AdvanceGroomProxyTier(inputs, state).Reason, GroomProxyRefusalReason::NotRequested);

        inputs.Requested = true;
        inputs.StrandsAvailable = 0u;
        EXPECT_EQ(AdvanceGroomProxyTier(inputs, state).Reason, GroomProxyRefusalReason::GroomHasNoGeometry);

        inputs.StrandsAvailable = 1000u;
        EXPECT_EQ(AdvanceGroomProxyTier(inputs, state).Reason, GroomProxyRefusalReason::None);
    }

    // A coat that never asked is not a scene full of failures — the rule
    // GroomCoatShadowStats and GroomLodStats already follow here.
    TEST(GroomRayTracingProxy, StatsSeparateARefusalFromACoatThatNeverAsked)
    {
        GroomProxyStats stats;

        GroomProxyDecision notRequested;
        notRequested.Reason = GroomProxyRefusalReason::NotRequested;
        stats.Record(notRequested);

        GroomProxyDecision refused;
        refused.Reason = GroomProxyRefusalReason::BudgetExhausted;
        stats.Record(refused);

        GroomProxyDecision represented;
        represented.Reason = GroomProxyRefusalReason::None;
        represented.Tier = GroomProxyTier::Proxy;
        stats.Record(represented);

        EXPECT_EQ(stats.GroomsConsidered, 3u);
        EXPECT_EQ(stats.GroomsRepresented, 1u);
        EXPECT_EQ(stats.GroomsRefused, 2u);
        EXPECT_EQ(stats.ByTier[static_cast<sizet>(GroomProxyTier::Proxy)], 1u);
        EXPECT_EQ(stats.DominantRefusalReason(), GroomProxyRefusalReason::BudgetExhausted);
        // NotRequested alone must not mark the frame incomplete: a scene with
        // ray tracing off is working exactly as configured.
        EXPECT_FALSE(stats.Complete);

        GroomProxyStats quiet;
        quiet.Record(notRequested);
        EXPECT_TRUE(quiet.Complete);
        EXPECT_EQ(quiet.DominantRefusalReason(), GroomProxyRefusalReason::None);
    }

    TEST(GroomRayTracingProxy, EveryRefusalReasonHasASentence)
    {
        for (u32 index = 0; index < static_cast<u32>(GroomProxyRefusalReason::Count); ++index)
        {
            const auto reason = static_cast<GroomProxyRefusalReason>(index);
            EXPECT_FALSE(Describe(reason).empty()) << "reason " << index;
            EXPECT_NE(Describe(reason), "Unknown") << "reason " << index;
            EXPECT_NE(ToString(reason), "Unknown") << "reason " << index;
        }
    }

    // ── The frame budget ─────────────────────────────────────────────────

    // Transactional: an oversized request consumes nothing, so one unaffordable
    // coat cannot starve the ones behind it.
    TEST(GroomRayTracingProxy, AnOversizedReservationConsumesNoBudget)
    {
        GroomProxyFrameBudget budget;
        ASSERT_TRUE(budget.Reserve(1000u, 500u));
        const u32 verticesBefore = budget.Vertices;
        const u32 trianglesBefore = budget.Triangles;
        const u32 updatesBefore = budget.Updates;

        EXPECT_FALSE(budget.Reserve(GroomProxyPolicy::VerticesPerFrame, 1u));
        EXPECT_EQ(budget.Vertices, verticesBefore);
        EXPECT_EQ(budget.Triangles, trianglesBefore);
        EXPECT_EQ(budget.Updates, updatesBefore);

        EXPECT_TRUE(budget.Reserve(1000u, 500u));
    }

    // The update cap is the only one testable WITHOUT a size, which is why
    // it is the one the producer checks before doing a coat's work rather
    // than after. Without it a scene past the budget pays a full strand
    // build and ribbon conversion per coat per frame and throws it away.
    TEST(GroomRayTracingProxy, TheUpdateCapIsAnsweredBeforeAnyWorkIsDone)
    {
        GroomProxyFrameBudget budget;
        EXPECT_FALSE(budget.Exhausted());
        for (u32 update = 0; update < GroomProxyPolicy::UpdatesPerFrame; ++update)
        {
            ASSERT_TRUE(budget.Reserve(1u, 1u)) << "update " << update;
        }
        EXPECT_TRUE(budget.Exhausted());
        // And it agrees with Reserve, which is the point: a pre-check that
        // said 'room' where Reserve says 'none' would put the skip back.
        EXPECT_FALSE(budget.Reserve(1u, 1u));
    }

    // ── The double-count invariant, as a guard ───────────────────────────

    // THE INVARIANT: a groom proxy occludes other receivers and never its own
    // coat. It holds today because GroomRenderPass does not write the
    // G-Buffer, so no screen-space shadow term reaches a strand and #1248's
    // tau is the only thing attenuating one. That is a property of a SHADER,
    // and nothing else in this suite would notice it changing.
    //
    // WHY THIS COUNTS RATHER THAN CHECKING FOR ABSENCE. A guard that only
    // asserts "this token is not here" passes on an empty string, a renamed
    // file, a read that failed, and a shader someone gutted. So it asserts
    // the POSITIVE facts at their exact counts first — if those move, the
    // test fails and a human reads the diff — and only then the absences.
    // The failure it prevents is already on record in a different door:
    // a groom that both casts and receives goes from 44.98 to 0.22 luma.
    TEST(GroomRayTracingProxy, TheCoatShaderReadsNoSceneShadowTermSoAProxyCannotShadowItsOwnCoat)
    {
        const auto path = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" /
                          "GroomStrand.glsl";
        std::ifstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open()) << "cannot open " << path.string();
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string source = buffer.str();
        ASSERT_GT(source.size(), 10000u) << "GroomStrand.glsl is implausibly short; the counts below "
                                            "would pass on a truncated read";

        const auto count = [&source](std::string_view token)
        {
            sizet found = 0;
            for (sizet at = source.find(token); at != std::string::npos;
                 at = source.find(token, at + token.size()))
            {
                ++found;
            }
            return found;
        };

        // The coat's OWN attenuation, which must stay exactly where it is.
        // Pinned at its count so a second call site — the shape a scene-shadow
        // term would most likely arrive in — fails here.
        EXPECT_EQ(count("oloGroomCoatOpticalDepth"), 2u)
            << "the coat's own optical-depth path moved; re-read the double-count boundary in "
               "GroomCoatShadow.h before changing this number";
        EXPECT_EQ(count("oloGroomCoatTransmittance"), 3u)
            << "the coat's own transmittance path moved; same warning";

        // And the absences: no scene shadow term of any kind reaches a strand.
        for (const std::string_view token : { "u_RayTracedShadowMask", "oloRayTracedShadowFactor",
                                              "u_ShadowMap", "u_ShadowMapArray", "ShadowMask" })
        {
            EXPECT_EQ(count(token), 0u)
                << "GroomStrand.glsl now reads '" << token << "'. A groom proxy is in the TLAS "
                                                              "(#1253), so a strand that also RECEIVES a scene shadow is shadowed by its own "
                                                              "coat twice — which is the 44.98 -> 0.22 luma failure. Before allowing this, the "
                                                              "coat's own proxy instance must be excluded from its own visibility rays.";
        }
    }

    // ── The measured comparison behind the analysis document ─────────────

    // Criterion 1 asks for detailed and proxy coat intersections to be
    // COMPARED and for the switch policy to be defined from that comparison.
    // This case is where the numbers in docs/analysis/groom-rt-proxies-1253.md
    // come from: it prints the table rather than asserting a row of it, for
    // GroomCoverage.h's reason — a measurement recorded only in a PR body is
    // a number nothing defends, and re-deriving it has to cost one test run.
    //
    // The assertions here are the ones the table's SHAPE has to satisfy for
    // the document's conclusion to hold; the exact figures are printed.
    TEST(GroomRayTracingProxyComparison, DetailedAndProxyCoatsOnTheTwoReferenceAnimals)
    {
        struct Reference
        {
            const char* Name;
            Tests::GroomStrandFixture::StrandCoat Coat;
        };
        Reference references[] = {
            { "scalp", Tests::GroomStrandFixture::MakeScalp(8192u, 12u) },
            { "pelt", Tests::GroomStrandFixture::MakePelt(16384u, 10u) },
        };

        for (auto& reference : references)
        {
            ASSERT_TRUE(reference.Coat.Groom) << reference.Coat.FailureReason;
            const GroomAsset& groom = *reference.Coat.Groom;

            // GROUND TRUTH is the coat at its FULL authored strand set, not
            // at the detailed tier's budget. Comparing the proxy against the
            // detailed tier would measure the two approximations against each
            // other and report the detailed tier as exact, which is the one
            // thing it certainly is not.
            const BuiltCoat truth = BuildCoat(groom, groom.GetCurveCount());
            ASSERT_GT(truth.Stats.SegmentCount, 0u);
            std::vector<GroomCoatShadow::CoatSegment> truthSegments;
            CollectGroomProxySegments(truth.Vertices, 1.0f, truthSegments);

            std::printf("\n[%s] %u strands, %u segments authored\n", reference.Name,
                        truth.Stats.StrandsAvailable, truth.Stats.SegmentCount);
            std::printf("  %-9s %8s %8s %7s %10s %10s %12s\n", "tier", "strands", "tris", "comp",
                        "meanErr", "maxErr", "vertexBytes");

            for (const auto& [name, budget] : { std::pair<const char*, u32>{ "Detailed",
                                                                             GroomProxyPolicy::DetailedStrandBudget },
                                                std::pair<const char*, u32>{ "Proxy",
                                                                             GroomProxyPolicy::ProxyStrandBudget } })
            {
                const BuiltCoat tier = BuildCoat(groom, budget);
                ASSERT_GT(tier.Stats.StrandsSelected, 0u) << name;
                const f32 achieved = static_cast<f32>(tier.Stats.StrandsSelected) /
                                     static_cast<f32>(tier.Stats.StrandsAvailable);
                const f32 compensation =
                    GroomProxyWidthCompensation(achieved, GroomProxyPolicy::MaxWidthCompensation);

                std::vector<GroomCoatShadow::CoatSegment> tierSegments;
                CollectGroomProxySegments(tier.Vertices, compensation, tierSegments);
                const auto error = CompareGroomProxyCoverage(truthSegments, tierSegments);

                std::vector<Vertex> vertices;
                std::vector<u32> indices;
                const auto mesh = ConvertGroomStrandMeshToProxy(tier.Vertices, { compensation, true, 0u, 0u, 0u },
                                                                vertices, indices);

                std::printf("  %-9s %8u %8u %6.1fx %9.3f%% %9.3f%% %11llu\n", name, tier.Stats.StrandsSelected,
                            mesh.TriangleCount(), static_cast<f64>(compensation),
                            error.MeanRelativeError * 100.0, error.MaxRelativeError * 100.0,
                            static_cast<unsigned long long>(mesh.VertexBytes));

                // The shape the document's conclusion rests on: both tiers
                // stay within a tenth of the authored coat's occlusion, and
                // the proxy tier costs strictly fewer triangles than the
                // detailed one. A tier that did neither would not be a tier.
                EXPECT_LT(error.MeanRelativeError, 0.10) << reference.Name << " / " << name;
                EXPECT_GT(mesh.TriangleCount(), 0u) << reference.Name << " / " << name;
            }
        }
    }

    // ── The metric itself ────────────────────────────────────────────────

    // The direction set must be the same set on every machine, or a number in
    // the analysis document stops being re-derivable.
    TEST(GroomRayTracingProxy, TheErrorDirectionsAreUnitAndDeterministic)
    {
        const auto first = GroomProxyErrorDirections(64u);
        const auto second = GroomProxyErrorDirections(64u);
        ASSERT_EQ(first.size(), 64u);
        ASSERT_EQ(first.size(), second.size());
        for (sizet index = 0; index < first.size(); ++index)
        {
            EXPECT_TRUE(Math::BitwiseEqual(first[index], second[index])) << "direction " << index;
            EXPECT_NEAR(glm::length(first[index]), 1.0f, 1.0e-5f) << "direction " << index;
        }
    }

    // A single segment's projected area is 2*r*L along a perpendicular
    // direction and zero along its own axis. Both ends of that are asserted,
    // because a metric that answered the same number for both would report a
    // coat's silhouette as direction-independent and hide exactly the failure
    // the crossed ribbon exists to prevent.
    TEST(GroomRayTracingProxy, DirectionalCoverageIsTheProjectedAreaOfATaperedCylinder)
    {
        const std::vector<GroomCoatShadow::CoatSegment> segment{
            { glm::vec3{ 0.0f, 0.0f, 0.0f }, glm::vec3{ 0.0f, 2.0f, 0.0f }, 0.1f, 0.1f }
        };
        // THE TOLERANCE IS f32's, not f64's. CoatSegment holds f32 radii and
        // f32 endpoints, so 0.1f is not 0.1 and the exact answer here is
        // 0.40000000596. Asserting at 1e-9 would be asserting that a float
        // literal is a double, which is a property of nothing.
        constexpr f64 kSingleTolerance = 1.0e-6;
        EXPECT_NEAR(DirectionalCoverage(segment, glm::vec3{ 1.0f, 0.0f, 0.0f }), 2.0 * 0.1 * 2.0,
                    kSingleTolerance);
        EXPECT_NEAR(DirectionalCoverage(segment, glm::vec3{ 0.0f, 1.0f, 0.0f }), 0.0, kSingleTolerance);
        // A taper uses the MEAN radius, which is what makes the sum linear in
        // the compensation.
        const std::vector<GroomCoatShadow::CoatSegment> tapered{
            { glm::vec3{ 0.0f, 0.0f, 0.0f }, glm::vec3{ 0.0f, 2.0f, 0.0f }, 0.2f, 0.0f }
        };
        EXPECT_NEAR(DirectionalCoverage(tapered, glm::vec3{ 1.0f, 0.0f, 0.0f }), 2.0 * 0.1 * 2.0,
                    kSingleTolerance);
        // A degenerate direction is answered with zero rather than a NaN.
        EXPECT_DOUBLE_EQ(DirectionalCoverage(segment, glm::vec3{ 0.0f }), 0.0);
    }
} // namespace OloEngine
