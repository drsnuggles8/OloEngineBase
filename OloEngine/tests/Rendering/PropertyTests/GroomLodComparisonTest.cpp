#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomLodComparisonTest — issue #1252, THE comparison. Acceptance criteria 1
// and 2, and the confidence-0.5 spike the issue's delivery contract demands
// before any production implementation.
//
// THE QUESTION. A coat at a distance can be drawn three ways: all its strands,
// fewer strands made wider, or a coarser representation cooked for the job.
// Criterion 1 says the hand-over must preserve silhouette, apparent density,
// colour and highlight response. Colour and highlight are preserved BY
// CONSTRUCTION here — every tier is a curve set drawn by one pass through one
// shader with one BCSDF (GroomLod.h says why that was the design) — so what is
// left to measure, and what this file measures, is DENSITY and SILHOUETTE.
//
// WHY IT IS A TEST AND NOT A SCRIPT, and why the numbers it prints are the
// numbers in docs/analysis/groom-representation-lod-1252.md: the same reason
// GroomCoveragePropertyTests gives for #1246's comparison. A measurement in a
// PR body is defended by nothing; the first change to the width projection
// moves it and nobody finds out.
//
// THE THREE THINGS IT DECIDES.
//
//   1. WHETHER THE WIDTH COMPENSATION WORKS, and whether it is 1/k rather than
//      1/sqrt(k). A thinned coat is measured with and without it, and the
//      uncompensated arm is what makes the compensated one a result rather
//      than a tautology — without the A/B, "coverage is preserved" is a claim
//      about a number nobody varied.
//
//   2. WHETHER A COOKED CARD LEVEL BEATS THINNING AT THE SAME COST, and which
//      of two card constructions to use. Neither is obvious: a card costs a
//      cook, a format section and resident memory, and the strand budget was
//      already free. Both arms are therefore measured against a stride thinned
//      to the SAME CURVE COUNT — an earlier version of this file compared 102
//      cards against 2 500 strands and proved only that 102 curves cover less
//      than 2 500 do.
//
//      The answer turned out to be "yes, but only past the compensation cap",
//      and it changed the builder: see findings 2 and 3 in the analysis.
//
//   3. WHETHER THE SHELL ("mesh") TIER IS HONEST ON ANY OF THESE COATS. A
//      shell claims a coverage of 1 everywhere inside the silhouette, so it is
//      only defensible where the real coat has SATURATED. That is measurable
//      without building a shell at all: the fraction of the coat's footprint
//      whose true coverage is already above 0.9. If that fraction stays low at
//      every plausible distance, a shell would be replacing a see-through coat
//      with a solid lump, and the issue's own instruction is to report the
//      evidence and rescope rather than ship it.
//
// WHAT IT CANNOT SEE. Whether the GPU agrees, and what any of it looks like in
// motion. GroomLodVisualEvidenceTest and the live editor captures are what tie
// these numbers to real pixels; GroomLodContractTest is what pins the
// hysteresis that only exists in motion.
// =============================================================================

#include <gtest/gtest.h>

#include "../../Groom/GroomLodFixture.h"
#include "../../Groom/GroomStrandFixture.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomCoverage.h"
#include "OloEngine/Groom/GroomLod.h"
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::GroomCoverage;

namespace
{
    // 256 square at 8x8 samples per pixel. The reference quantises to 1/64,
    // which is an order of magnitude finer than the differences being decided
    // (a halving of density is a factor of two).
    // #1246's comparison used 16x16 because it was separating modes whose error
    // is a few hundredths; this one separates factors of two.
    constexpr u32 kWidth = 256;
    constexpr u32 kHeight = 256;
    constexpr u32 kSupersample = 8;

    // A pixel is SOLID when the true coat already covers more than this much of
    // it. The shell question is what fraction of the footprint is solid: a
    // shell replaces the footprint with coverage 1, so the error it introduces
    // is bounded below by (1 - mean coverage) over the pixels it claims.
    constexpr f32 kSolidThreshold = 0.9f;

    struct Camera
    {
        glm::mat4 View{ 1.0f };
        glm::mat4 Projection{ 1.0f };
        f32 FovYRadians = 0.0f;
    };

    // The eye distance that makes `groom` project to `pixelSize` pixels of the
    // frame's HEIGHT — the same quantity GroomLodPolicy's thresholds are in, so
    // the table's rows are directly readable as policy numbers rather than as
    // metres that have to be converted by whoever reads the analysis.
    [[nodiscard]] f32 DistanceForPixelSize(const GroomAsset& groom, f32 pixelSize, f32 fovYRadians) noexcept
    {
        const f32 radius = glm::length(groom.GetBoundsMax() - groom.GetBoundsMin()) * 0.5f;
        const f32 cotHalfFov = 1.0f / std::tan(fovYRadians * 0.5f);
        return (2.0f * radius) * cotHalfFov * (static_cast<f32>(kHeight) * 0.5f) / pixelSize;
    }

    [[nodiscard]] Camera MakeCameraAt(const GroomAsset& groom, f32 pixelSize)
    {
        Camera camera;
        camera.FovYRadians = glm::radians(45.0f);
        const glm::vec3 centre = (groom.GetBoundsMin() + groom.GetBoundsMax()) * 0.5f;
        const f32 distance = DistanceForPixelSize(groom, pixelSize, camera.FovYRadians);
        // Looking along -Z from in front, and slightly above, so a scalp is
        // seen across its silhouette rather than down its parting. The angle is
        // fixed across every row so a distance sweep varies one thing.
        const glm::vec3 eye = centre + glm::normalize(glm::vec3{ 0.0f, 0.25f, 1.0f }) * distance;
        camera.View = glm::lookAt(eye, centre, glm::vec3{ 0.0f, 1.0f, 0.0f });
        camera.Projection = glm::perspective(camera.FovYRadians, static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                             std::max(distance * 0.01f, 1.0e-3f), distance * 4.0f);
        return camera;
    }

    // One representation, measured.
    struct Row
    {
        std::string Label;
        u32 Curves = 0;
        u32 Segments = 0;
        /// Analytic covered area of THIS geometry, divided by the full coat's.
        /// The geometry claim: 1.0 means the representation carries the same
        /// apparent density, before any composition mode is applied.
        f64 GeometryAreaRatio = 0.0;
        /// The same ratio through the SHIPPED composition (stochastic alpha,
        /// converged over eight frames). This is the one that includes the
        /// one-pixel width floor and the widened-alpha clamp, so it is the
        /// number a screenshot would show.
        f64 ShippedAreaRatio = 0.0;
        /// Per-pixel error of this representation's analytic coverage against
        /// the full coat's. The SILHOUETTE claim.
        CoverageError Geometry;
        u64 VertexBytes = 0;
    };

    [[nodiscard]] std::vector<ScreenSegment> Project(const GroomAsset& groom, const Camera& camera, f32 widthScale,
                                                     ProjectionStats& outStats)
    {
        std::vector<ScreenSegment> segments;
        outStats = ProjectGroom(groom, glm::mat4(1.0f), camera.View, camera.Projection, kWidth, kHeight, widthScale,
                                segments);
        return segments;
    }

    [[nodiscard]] u64 VertexBytesFor(const GroomAsset& groom)
    {
        GroomStrandBuildSettings settings;
        settings.MaxStrands = 8u * 1024u * 1024u;
        const GroomStrandMeshStats plan = PlanGroomStrandMesh(groom, settings, nullptr);
        return plan.VertexBytes + plan.IndexBytes;
    }

    // The covered area this geometry SHOWS through the shipped composition,
    // relative to the full coat's analytic area.
    //
    // Eight temporal frames because that is TAA's effective history: a single
    // stochastic frame is an estimate whose error is noise, and the number a
    // screenshot shows is the converged one (groom-strand-visibility.md rule 6).
    [[nodiscard]] f64 ShippedAreaRatio(const std::vector<ScreenSegment>& segments, f64 fullArea)
    {
        ModeParameters shipped;
        shipped.TemporalFrames = 8;
        const std::vector<f32> composed =
            ModeCoverage(segments, kWidth, kHeight, GroomCompositionMode::StochasticAlpha, shipped);
        return fullArea > 0.0 ? TotalCoverage(composed) / fullArea : 0.0;
    }

    [[nodiscard]] Row Measure(const std::string& label, const GroomAsset& groom, const Camera& camera, f32 widthScale,
                              const std::vector<f32>& fullReference, f64 fullArea)
    {
        Row row;
        row.Label = label;
        row.Curves = groom.GetCurveCount();

        ProjectionStats stats;
        const std::vector<ScreenSegment> segments = Project(groom, camera, widthScale, stats);
        row.Segments = stats.SegmentsProjected;

        const std::vector<f32> geometry = ReferenceCoverage(segments, kWidth, kHeight, kSupersample);
        if (geometry.size() != fullReference.size())
        {
            ADD_FAILURE() << label << ": ReferenceCoverage rejected its parameters";
            return row;
        }
        row.Geometry = CompareCoverage(geometry, fullReference);
        row.GeometryAreaRatio = fullArea > 0.0 ? TotalCoverage(geometry) / fullArea : 0.0;

        row.ShippedAreaRatio = ShippedAreaRatio(segments, fullArea);
        row.VertexBytes = VertexBytesFor(groom);
        return row;
    }

    void PrintRow(const Row& row)
    {
        std::printf("[groom-lod] %-28s %8u %9u %9.3f %9.3f %10.5f %10.5f %9.2f\n", row.Label.c_str(), row.Curves,
                    row.Segments, row.GeometryAreaRatio, row.ShippedAreaRatio, row.Geometry.MeanAbsolute,
                    row.Geometry.Rmse, static_cast<f64>(row.VertexBytes) / (1024.0 * 1024.0));
    }

    void PrintHeader(const char* coat, f32 pixelSize, const std::vector<f32>& reference)
    {
        f64 solid = 0.0;
        f64 covered = 0.0;
        f64 sum = 0.0;
        for (const f32 value : reference)
        {
            if (value > 0.0f)
            {
                covered += 1.0;
                sum += static_cast<f64>(value);
                if (value > kSolidThreshold)
                {
                    solid += 1.0;
                }
            }
        }
        std::printf("\n[groom-lod] === %s @ %.0f px === footprint %.0f px, mean coverage %.3f, solid fraction %.3f\n",
                    coat, static_cast<f64>(pixelSize), covered, covered > 0.0 ? sum / covered : 0.0,
                    covered > 0.0 ? solid / covered : 0.0);
        std::printf("[groom-lod] %-28s %8s %9s %9s %9s %10s %10s %9s\n", "representation", "curves", "segments",
                    "geo-area", "shown", "mean|e|", "rmse", "MiB");
    }

    // The fraction of the coat's footprint whose true coverage has saturated.
    // THE shell number: see the file header.
    [[nodiscard]] f64 SolidFraction(const std::vector<f32>& reference) noexcept
    {
        f64 solid = 0.0;
        f64 covered = 0.0;
        for (const f32 value : reference)
        {
            if (value > 0.0f)
            {
                covered += 1.0;
                if (value > kSolidThreshold)
                {
                    solid += 1.0;
                }
            }
        }
        return covered > 0.0 ? solid / covered : 0.0;
    }

    // The curves the strand budget keeps at a given halving step, and the
    // fraction it ACTUALLY retained. Through SelectGroomStrandCurves — the real
    // selection the renderer uses — rather than a stride computed here, because
    // the whole point of measuring the achieved fraction is that it is not the
    // requested one (GroomLodWidthCompensation says why).
    struct Thinned
    {
        Ref<GroomAsset> Groom;
        f32 AchievedFraction = 1.0f;
        f32 Compensation = 1.0f;
    };

    // The curves the strand budget keeps for a target COUNT, and the fraction
    // it actually retained.
    //
    // A TARGET COUNT rather than a halving step, because the card arms below
    // have to be compared against a thinning of the SAME SIZE. "Cards beat
    // thinning" is not a claim anyone can check if the two arms cost different
    // amounts — the first version of this file compared 102 cards against 2500
    // strands and proved only that 102 curves cover less than 2500 do.
    [[nodiscard]] Thinned ThinToCount(const GroomAsset& base, u32 targetCount, f32 maxCompensation)
    {
        Thinned out;
        GroomStrandBuildSettings settings;
        const u32 available = base.GetCurveCount();
        settings.MaxStrands = std::max(1u, targetCount);

        TArray<u32> curves;
        SelectGroomStrandCurves(base, settings, curves, nullptr);
        out.AchievedFraction = available > 0u ? static_cast<f32>(curves.Num()) / static_cast<f32>(available) : 0.0f;
        out.Compensation = GroomLodWidthCompensation(out.AchievedFraction, maxCompensation);

        std::string reason;
        out.Groom = Tests::GroomLodFixture::MakeSubsetGroom(base, std::span{ curves.GetData(), static_cast<sizet>(curves.Num()) }, reason);
        EXPECT_TRUE(out.Groom) << "thinning to " << targetCount << " failed: " << reason;
        return out;
    }

    [[nodiscard]] Thinned ThinBy(const GroomAsset& base, u32 step, f32 maxCompensation)
    {
        return ThinToCount(base,
                           std::max(1u, static_cast<u32>(static_cast<f32>(base.GetCurveCount()) *
                                                         GroomLodStepFraction(step))),
                           maxCompensation);
    }

    struct Coat
    {
        std::string Name;
        Ref<GroomAsset> Groom;
        /// Card cell sizes to cook and measure, coarse first. Several, because
        /// the cell IS the card tier's one authoring knob and a single value
        /// would report a point rather than a trade.
        std::vector<f32> CardCells;
    };

    // ONE coat, by index. Per-coat rather than all-three, because each
    // comparison case below is its own gtest SUITE and therefore its own ctest
    // entry -- so a case that built all three would pay for two coats it never
    // measures, three times over.
    constexpr sizet kCoatCount = 3;

    [[nodiscard]] Coat MakeCoat(sizet index)
    {
        using namespace Tests::GroomStrandFixture;

        // THE FIXTURE IS SELECTED BEFORE IT IS GENERATED. An earlier revision
        // built all three and returned one, which meant each split case paid
        // for two coats it never measures — and MakeCoats() below paid for nine
        // to keep three. That is the opposite of what the split was for.
        const char* name = nullptr;
        StrandCoat source;
        std::vector<f32> cells{ 0.05f, 0.02f };
        switch (index)
        {
            case 0:
                // A human scalp: long, sparse, 70 um. The hardest case for any
                // aggregation, because the strands are nowhere near each other.
                name = "human-scalp";
                source = MakeScalp(20000u, 8u);
                break;
            case 1:
                // A short coat: dense, 25 mm, 110 um. The case a shell would be
                // for, if a shell were for anything.
                name = "short-coat";
                source = MakePelt(30000u, 4u);
                break;
            case 2:
                // A long coat: 90 mm guard hairs, coarser and sparser than the
                // short one. Its silhouette is the thing a LOD is most likely
                // to lose.
                name = "long-coat";
                source = MakePelt(20000u, 6u, 3u, 0.12f, 0.09f, 1.4e-4f);
                break;
            default:
                ADD_FAILURE() << "no coat with index " << index;
                return Coat{};
        }

        EXPECT_TRUE(source.Groom) << source.FailureReason;
        if (!source.Groom)
        {
            return Coat{};
        }

        // REBUILT WITH WRAPPED ROOT UVs. #1246's generators record an unwrapped
        // `phi / 2pi`, which reaches ~11 459 on a 20 000-strand scalp, and the
        // clump-cell addressing clamps a UV to +/-16 — so clustering the raw
        // fixture puts most of the animal in one cell and measures the fixture
        // instead of the representation. See RebuildWithWrappedRootUVs for why
        // the fold happens here rather than upstream.
        std::string reason;
        Ref<GroomAsset> wrapped = Tests::GroomLodFixture::RebuildWithWrappedRootUVs(*source.Groom, reason);
        EXPECT_TRUE(wrapped) << reason;
        if (!wrapped)
        {
            return Coat{};
        }
        return Coat{ name, wrapped, std::move(cells) };
    }

    [[nodiscard]] std::vector<Coat> MakeCoats()
    {
        std::vector<Coat> coats;
        for (sizet i = 0; i < kCoatCount; ++i)
        {
            coats.push_back(MakeCoat(i));
        }
        return coats;
    }
} // namespace

// =============================================================================
// The comparison
// =============================================================================

namespace
{
    // The comparison for ONE coat. Called from three separate gtest SUITES
    // below, which is a cost decision rather than a taste one: ctest registers
    // one entry per suite and times each entry out at 600 s, and the single
    // combined suite ran past that under every sanitizer. Three entries share
    // the budget and run in parallel.
    void RunComparisonForCoat(const Coat& coat)
    {
        // Apparent sizes, in pixels of the frame height, INSIDE the card
        // regime. 256 px was in this sweep and is not any more: cost scales
        // with the coat's pixel FOOTPRINT, so the 256 px row alone was about
        // three quarters of the whole measurement -- and it is the one size at
        // which none of the deciding claims apply, because they are gated to
        // `pixelSize < CardPixelSize` (256). It bought one boundary observation
        // for three quarters of the runtime. The shell case still sweeps down
        // from 128 px to 4 px, so the wide-size behaviour is still measured.
        const std::array<f32, 3> pixelSizes{ 128.0f, 64.0f, 32.0f };
        constexpr f32 kMaxCompensation = 8.0f;

        ASSERT_TRUE(coat.Groom) << coat.Name;

        // The card levels, cooked once per coat: a level is a property of the
        // ASSET rather than of the camera, which is the whole reason it is
        // cooked rather than derived per frame.
        struct CardArm
        {
            f32 Cell = 0.0f;
            GroomCardAggregation Aggregation = GroomCardAggregation::MeanCentreline;
            Ref<GroomAsset> Groom;
            GroomCardBuildStats Stats;
        };
        std::vector<CardArm> cardArms;
        std::string reason;
        // BOTH constructions, at both cells. The question this answers is not
        // "do cards work" but "which aggregation, if either, beats the free
        // budget stride at the same curve count" — and an average and a kept
        // sample are the two candidates that differ in the one way that could
        // matter.
        const std::array<GroomCardAggregation, 2> aggregations{ GroomCardAggregation::MeanCentreline,
                                                                GroomCardAggregation::RepresentativeStrand };
        for (const f32 cell : coat.CardCells)
            for (const GroomCardAggregation aggregation : aggregations)
            {
                GroomCardSettings settings;
                settings.CellSize = cell;
                settings.PointsPerCard = 6;
                settings.Aggregation = aggregation;

                GroomLodLevel level;
                GroomCardBuildStats stats;
                ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*coat.Groom, settings, level, reason, &stats))
                    << coat.Name << " @ cell " << cell << ": " << reason;
                Ref<GroomAsset> asGroom = Tests::GroomLodFixture::MakeGroomFromLevel(*coat.Groom, level, reason);
                ASSERT_TRUE(asGroom) << coat.Name << ": " << reason;

                std::printf("\n[groom-lod] ### %s cell %.4f: %u strands -> %u cards (cluster min %u mean %.1f max %u), "
                            "member width sum %.6f, covered %.6f (%.3f), card width sum %.6f\n",
                            coat.Name.c_str(), static_cast<f64>(cell), stats.CurvesConsidered, stats.CardsBuilt,
                            stats.SmallestCluster, static_cast<f64>(stats.MeanCluster), stats.LargestCluster,
                            stats.MemberWidthSum, stats.MemberCoveredWidthSum,
                            stats.MemberCoveredWidthSum / std::max(stats.MemberWidthSum, 1.0e-30), stats.CardWidthSum);

                // -- The cook's own density claim --------------------------
                //
                // Giving the card the width its members COVER (#1428) is what is
                // supposed to make it carry the cluster's covered area. Asserted
                // on the COOK's numbers, before any camera is involved, because a
                // clustering bug that dropped a member would show here as a width
                // deficit and everywhere else as a slightly thin coat that looks
                // plausible.
                ASSERT_GT(stats.MemberCoveredWidthSum, 0.0) << coat.Name;
                EXPECT_NEAR(stats.CardWidthSum / stats.MemberCoveredWidthSum, 1.0, 1.0e-3)
                    << coat.Name << ": the cards do not carry the width their members cover";
                EXPECT_LT(stats.CardsBuilt, stats.CurvesConsidered) << coat.Name << ": the card level reduced nothing";

                cardArms.push_back(CardArm{ cell, aggregation, asGroom, stats });
            }
        ASSERT_FALSE(cardArms.empty());

        for (const f32 pixelSize : pixelSizes)
        {
            const Camera camera = MakeCameraAt(*coat.Groom, pixelSize);

            ProjectionStats fullStats;
            const std::vector<ScreenSegment> fullSegments = Project(*coat.Groom, camera, 1.0f, fullStats);
            ASSERT_GT(fullStats.SegmentsProjected, 0u) << coat.Name << " @ " << pixelSize << ": nothing projected";
            const std::vector<f32> fullReference = ReferenceCoverage(fullSegments, kWidth, kHeight, kSupersample);
            ASSERT_EQ(fullReference.size(), static_cast<sizet>(kWidth) * kHeight);
            const f64 fullArea = TotalCoverage(fullReference);
            ASSERT_GT(fullArea, 0.0) << coat.Name << " @ " << pixelSize;

            PrintHeader(coat.Name.c_str(), pixelSize, fullReference);

            // ASSEMBLED, NOT RE-MEASURED. `fullReference` IS the full coat's
            // analytic coverage — it was just computed, at the most expensive
            // density in the table — so calling Measure() for it projected and
            // rasterised the identical geometry a second time. Its row is
            // 1.000 area and zero error against itself by definition; the only
            // thing that needed computing is the SHOWN column, which is a
            // composition pass rather than a 64-sample rasterisation.
            Row full;
            full.Label = "strand-full";
            full.Curves = coat.Groom->GetCurveCount();
            full.Segments = fullStats.SegmentsProjected;
            full.GeometryAreaRatio = 1.0;
            full.ShippedAreaRatio = ShippedAreaRatio(fullSegments, fullArea);
            full.VertexBytes = VertexBytesFor(*coat.Groom);
            PrintRow(full);

            // -- 1. Does the compensation work, and is it 1/k? ----------
            for (u32 step = 1; step <= 3; ++step)
            {
                const Thinned thinned = ThinBy(*coat.Groom, step, kMaxCompensation);
                ASSERT_TRUE(thinned.Groom);

                const Row bare = Measure(std::format("strand-1/{}-bare", 1u << step), *thinned.Groom, camera, 1.0f,
                                         fullReference, fullArea);
                const Row compensated =
                    Measure(std::format("strand-1/{}-compensated", 1u << step), *thinned.Groom, camera,
                            thinned.Compensation, fullReference, fullArea);
                PrintRow(bare);
                PrintRow(compensated);

                const std::string tag =
                    std::format("{}_{:.0f}px_step{}", coat.Name, static_cast<f64>(pixelSize), step);
                ::testing::Test::RecordProperty(tag + "_achieved_fraction", std::format("{:.6f}", thinned.AchievedFraction));
                ::testing::Test::RecordProperty(tag + "_compensation", std::format("{:.6f}", thinned.Compensation));
                ::testing::Test::RecordProperty(tag + "_bare_area", std::format("{:.6f}", bare.GeometryAreaRatio));
                ::testing::Test::RecordProperty(tag + "_compensated_area", std::format("{:.6f}", compensated.GeometryAreaRatio));

                // THE A/B. Without the bare arm, "the compensated coat has the
                // right area" is a statement about a number nobody varied.
                EXPECT_LT(bare.GeometryAreaRatio, 0.75)
                    << tag << ": thinning by 2^" << step << " did not remove area, so this case decides nothing";
                EXPECT_LT(std::abs(compensated.GeometryAreaRatio - 1.0), std::abs(bare.GeometryAreaRatio - 1.0))
                    << tag << ": the width compensation did not bring the apparent density back";
                // THE BAND. A tenth is comfortably inside every measured value
                // and comfortably outside what the uncompensated arm scores
                // (0.19 to 0.64 across the sweep), so it separates the two
                // rather than merely bounding one.
                EXPECT_NEAR(compensated.GeometryAreaRatio, 1.0, 0.10)
                    << tag << ": the compensated coat is not the density it started at";

                // And the compensation is LINEAR. 1/sqrt(k) would leave a
                // 2^step thinning short by a factor of sqrt(2^step), which at
                // step 3 is 2.8x -- far outside this band, so the band is what
                // separates the two rules rather than merely bounding one.
                EXPECT_NEAR(static_cast<f64>(thinned.Compensation * thinned.AchievedFraction), 1.0, 0.05)
                    << tag << ": compensation x achieved fraction must be 1 (the linear rule)";
            }

            // Keyed by card count and rebuilt per DISTANCE: a Row is a
            // measurement through one camera, so it cannot outlive the pose it
            // was taken at.
            std::map<u32, Row> matchedByCount;

            // -- 2. Does a cooked card beat thinning AT THE SAME COST? --
            //
            // The matched arm is the whole of this comparison. A card level is
            // only worth a cook, a format section and resident memory if it
            // buys something a free budget stride does not, and the two are
            // only comparable at the same curve count.
            for (const CardArm& arm : cardArms)
            {
                const u32 cardCount = arm.Groom->GetCurveCount();
                const Row cards =
                    Measure(std::format("{}-cell{:.3f}", arm.Aggregation == GroomCardAggregation::MeanCentreline ? "card-mean" : "card-repr",
                                        arm.Cell),
                            *arm.Groom, camera, 1.0f, fullReference, fullArea);
                PrintRow(cards);

                // MEASURED ONCE PER CARD COUNT, not once per card arm. The two
                // aggregations at one cell produce the SAME number of cards, so
                // they share one matched stride — measuring it twice rasterised
                // identical geometry for an identical answer.
                auto matchedIt = matchedByCount.find(cardCount);
                if (matchedIt == matchedByCount.end())
                {
                    const Thinned matched = ThinToCount(*coat.Groom, cardCount, kMaxCompensation);
                    ASSERT_TRUE(matched.Groom);
                    matchedIt = matchedByCount
                                    .emplace(cardCount, Measure(std::format("strand-matched({})", cardCount),
                                                                *matched.Groom, camera, matched.Compensation,
                                                                fullReference, fullArea))
                                    .first;
                }
                const Row& matchedRow = matchedIt->second;
                PrintRow(matchedRow);

                const std::string tag = std::format("{}_{:.0f}px_cell{:.3f}_{}", coat.Name,
                                                    static_cast<f64>(pixelSize), arm.Cell,
                                                    ToString(arm.Aggregation));
                ::testing::Test::RecordProperty(tag + "_cards", std::to_string(cardCount));
                ::testing::Test::RecordProperty(tag + "_card_area", std::format("{:.6f}", cards.GeometryAreaRatio));
                ::testing::Test::RecordProperty(tag + "_card_mean_abs", std::format("{:.6f}", cards.Geometry.MeanAbsolute));
                ::testing::Test::RecordProperty(tag + "_matched_area", std::format("{:.6f}", matchedRow.GeometryAreaRatio));
                ::testing::Test::RecordProperty(tag + "_matched_mean_abs", std::format("{:.6f}", matchedRow.Geometry.MeanAbsolute));
                ::testing::Test::RecordProperty(tag + "_card_mib",
                                                std::format("{:.4f}", static_cast<f64>(cards.VertexBytes) / 1048576.0));

                std::printf("[groom-lod]    ^ vs matched strands: card mean|e| %.5f, strand mean|e| %.5f -> %s\n",
                            cards.Geometry.MeanAbsolute, matchedRow.Geometry.MeanAbsolute,
                            cards.Geometry.MeanAbsolute < matchedRow.Geometry.MeanAbsolute ? "CARD" : "STRAND");

                // ── THE DECIDING CLAIMS, as assertions ──────────────
                //
                // Only at the COARSE cell, and that restriction is the finding
                // rather than a convenience. At a modest reduction the runtime
                // stride's 1/k widening is inside the compensation cap, so it
                // ties the card within noise and there is nothing to decide.
                // Past the cap it cannot widen far enough, and the card — whose
                // width is BAKED — is the only arm that can still carry the
                // density. A cell that reduces by less than the cap is a cell
                // not worth cooking.
                //
                // AND only at sizes where the tier would actually be selected,
                // i.e. below the policy's default hand-over. At 256 px a 25x
                // reduction is visibly coarse whichever way it is built — the
                // card wins on DENSITY there (0.96 against 0.41) and loses on
                // per-pixel error (0.325 against 0.281), because at that size
                // the error is dominated by where the coverage is rather than
                // by how much of it there is. That is not a defect in the card;
                // it is why the tier is distance-gated, and asserting the claim
                // at a size the tier is never used at would be asserting
                // something the feature does not do.
                const f32 reduction = static_cast<f32>(coat.Groom->GetCurveCount()) / static_cast<f32>(cardCount);
                const bool insideTheCardRegime = pixelSize < GroomLodPolicy{}.CardPixelSize;
                // ...and only for the SELECTED construction. MeanCentreline is
                // the measured-and-rejected alternative — it loses on both
                // statistics, which is the finding — so asserting that it wins
                // would be asserting the opposite of the result.
                const bool selectedConstruction =
                    arm.Aggregation == GroomCardAggregation::RepresentativeStrand;
                if (reduction > kMaxCompensation * 1.5f && insideTheCardRegime && selectedConstruction)
                {
                    // 1. The card keeps the coat's apparent density...
                    EXPECT_NEAR(cards.GeometryAreaRatio, 1.0, 0.10)
                        << tag << ": the card level does not carry the coat's density at a " << reduction
                        << "x reduction";
                    // 2. ...and the matched stride demonstrably CANNOT, because
                    //    the compensation it would need is past the cap. This
                    //    is the arm that makes claim 1 a result.
                    EXPECT_LT(matchedRow.GeometryAreaRatio, 0.75)
                        << tag << ": the strand stride kept the density at a " << reduction
                        << "x reduction, so the cap is not binding and this comparison decides nothing";
                    // 3. And the card's silhouette is at least as close to the
                    //    full coat's — stated as TWO claims, because the data
                    //    supports two different strengths and asserting the
                    //    strong one everywhere would be asserting a coincidence.
                    //
                    //    The card's advantage GROWS as the coat shrinks. On the
                    //    scalp the STRIDE's error is 2.2x the card's at 128 px,
                    //    2.8x at 64 and 3.7x at 32 -- the card is the smaller
                    //    number, and stating it the other way round says the
                    //    opposite of the finding. That growth is itself why the
                    //    tier is distance-gated. At
                    //    the very TOP of the card band the two are within noise
                    //    — the short coat scores 0.2567 against 0.2602 at 128 px,
                    //    a 1.3% margin that a different float summation order on
                    //    another platform could flip either way. So the claim
                    //    held everywhere in the band is "never materially
                    //    worse", and the strict one is held where the margin is
                    //    a factor rather than a percent.
                    EXPECT_LT(cards.Geometry.MeanAbsolute, matchedRow.Geometry.MeanAbsolute * 1.05)
                        << tag << ": the card level is materially WORSE than a free budget stride at the same "
                                  "curve count, so it is not worth cooking";
                    if (pixelSize <= GroomLodPolicy{}.CardPixelSize * 0.25f)
                    {
                        EXPECT_LT(cards.Geometry.MeanAbsolute, matchedRow.Geometry.MeanAbsolute)
                            << tag << ": the card level did not beat a free budget stride at a size the tier is "
                                      "squarely inside, where its measured advantage is a factor and not a margin";
                    }
                }
            }

            // -- 3. Is a shell honest on this coat at this size? --------
            const f64 solid = SolidFraction(fullReference);
            ::testing::Test::RecordProperty(std::format("{}_{:.0f}px_solid_fraction", coat.Name, static_cast<f64>(pixelSize)),
                                            std::format("{:.6f}", solid));
        }
    }
} // namespace

// =============================================================================
// One SUITE per coat, and the suite boundary is the point
// =============================================================================
//
// ctest registers one entry per gtest SUITE and times each entry out at 600 s.
// As one combined suite this measurement ran past that under ASan, TSan and
// UBSan alike — three red jobs, one cause. Three suites are three entries that
// share the budget and run in parallel, and splitting by COAT is the split that
// costs nothing: the coats are independent measurements that were only ever in
// one case because they were written in one loop.

TEST(GroomLodComparisonHumanScalp, TheMeasuredComparisonBehindTheRepresentationLadder)
{
    RunComparisonForCoat(MakeCoat(0));
}

TEST(GroomLodComparisonShortCoat, TheMeasuredComparisonBehindTheRepresentationLadder)
{
    RunComparisonForCoat(MakeCoat(1));
}

TEST(GroomLodComparisonLongCoat, TheMeasuredComparisonBehindTheRepresentationLadder)
{
    RunComparisonForCoat(MakeCoat(2));
}

// =============================================================================
// The shell question, on its own, so its answer is a named case
// =============================================================================

TEST(GroomLodShellTier, TheShellTierIsMeasuredBeforeItIsRefused)
{
    const std::vector<Coat> coats = MakeCoats();
    ASSERT_FALSE(coats.empty());

    // Down to four pixels: past this a whole animal is a smudge and no
    // representation question survives. If a coat were ever going to saturate,
    // it would have done so by here.
    const std::array<f32, 6> pixelSizes{ 128.0f, 64.0f, 32.0f, 16.0f, 8.0f, 4.0f };

    f64 worstSolid = 0.0;
    std::string worstLabel;

    for (const Coat& coat : coats)
    {
        ASSERT_TRUE(coat.Groom) << coat.Name;
        for (const f32 pixelSize : pixelSizes)
        {
            const Camera camera = MakeCameraAt(*coat.Groom, pixelSize);
            ProjectionStats stats;
            const std::vector<ScreenSegment> segments = Project(*coat.Groom, camera, 1.0f, stats);
            if (stats.SegmentsProjected == 0u)
            {
                continue;
            }
            const std::vector<f32> reference = ReferenceCoverage(segments, kWidth, kHeight, kSupersample);
            ASSERT_EQ(reference.size(), static_cast<sizet>(kWidth) * kHeight);

            f64 sum = 0.0;
            f64 covered = 0.0;
            for (const f32 value : reference)
            {
                if (value > 0.0f)
                {
                    covered += 1.0;
                    sum += static_cast<f64>(value);
                }
            }
            const f64 solid = SolidFraction(reference);
            const f64 mean = covered > 0.0 ? sum / covered : 0.0;

            std::printf("[groom-lod-shell] %-12s @ %5.0f px: footprint %7.0f px, mean coverage %.3f, solid %.3f\n",
                        coat.Name.c_str(), static_cast<f64>(pixelSize), covered, mean, solid);
            ::testing::Test::RecordProperty(std::format("shell_{}_{:.0f}px_mean_coverage", coat.Name, static_cast<f64>(pixelSize)),
                                            std::format("{:.6f}", mean));
            ::testing::Test::RecordProperty(std::format("shell_{}_{:.0f}px_solid", coat.Name, static_cast<f64>(pixelSize)),
                                            std::format("{:.6f}", solid));

            if (solid > worstSolid)
            {
                worstSolid = solid;
                worstLabel = std::format("{} @ {:.0f} px", coat.Name, static_cast<f64>(pixelSize));
            }
        }
    }

    std::printf("[groom-lod-shell] highest solid fraction anywhere in the sweep: %.3f (%s)\n", worstSolid,
                worstLabel.c_str());
    ::testing::Test::RecordProperty("shell_worst_solid_fraction", std::format("{:.6f}", worstSolid));

    // THE REFUSAL, AS AN ASSERTION. A shell replaces the footprint with
    // coverage 1. It is defensible only where the coat has already saturated,
    // and this bound says that none of the engine's reference coats does at any
    // distance a camera would use. If a future coat DOES — a dense short pelt
    // authored at ten times this density, say — this case fails, which is the
    // correct signal: the refusal recorded in GroomLodFallbackReason and in
    // docs/analysis/groom-representation-lod-1252.md would then be out of date
    // and the shell tier would be worth building.
    EXPECT_LT(worstSolid, 0.5)
        << "a reference coat now saturates its own footprint at " << worstLabel
        << "; the measured refusal of the shell tier (GroomLodFallbackReason::MeshTierNotSelected) is stale and "
           "docs/analysis/groom-representation-lod-1252.md must be re-run";
}
