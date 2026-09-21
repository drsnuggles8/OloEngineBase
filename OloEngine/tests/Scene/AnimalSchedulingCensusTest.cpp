#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// AnimalSchedulingCensusTest — issue #1258, criterion 2.
//
// The criterion is an instruction NOT TO ASSUME: "integrate existing animated
// batching work and measure which costs dominate rather than assuming draw
// calls are the limit." This file is that measurement, and it ships as a test
// rather than as a table in a PR body for geometry-lod-measure-the-unreachable-
// cost.md rule 10's reason: the measurement that picks between designs is the
// part most likely to be re-litigated, and a PR-body table cannot be re-run.
//
// WHAT IS MEASURED HERE, AND WHAT IS NOT.
//
// This is a STRUCTURAL census — a count of the work each axis submits for a
// representative population at a range of distances — priced through
// AnimalCostModel. It is not a wall-clock measurement, and deliberately so:
// this binary runs Debug in CI, a Debug build cannot measure CPU scheduling at
// all, and three sibling worktrees build concurrently on this box and swing GPU
// timings by up to 4x. A timing assertion here would be a flake generator that
// told nobody anything.
//
// The COEFFICIENTS in AnimalCostModel are NOT a wall-clock regression either,
// and this file must not be read as if they were. They are structural ratios —
// what one bone, one guide particle, one strand and one voxel cost RELATIVE to
// each other as this engine implements them — recorded with their honest status
// in docs/analysis/multi-animal-scheduling-budgets-1258.md section 4.
//
// That makes the shape of the answer a claim that has to survive the model
// being wrong, which is what TheDrawCallConclusionSurvivesALargeErrorInItsOwn-
// Coefficient measures rather than assumes. What this file asserts:
//
//   * draw calls are a small share at every tested distance, AND that holds
//     until PerDrawCall is off by more than an order of magnitude — so
//     scheduling is the right lever and batching (#1031) is not the missing
//     piece here;
//   * the dominant share MOVES with distance, which is why one global quality
//     scalar cannot be right at both ends;
//   * the share the budget CANNOT reach is bounded and named.
//
// THE RULE THIS FILE EXISTS TO ENFORCE: separate what the budget can reach from
// what it cannot. A schedule that redistributes work can only ever touch the
// first, and a census that did not split them would let a change that moved
// cost into the unreachable half look like an improvement.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Scene/AnimalScheduler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    constexpr sizet kDeform = static_cast<sizet>(AnimalWorkAxis::Deformation);
    constexpr sizet kSim = static_cast<sizet>(AnimalWorkAxis::Simulation);
    constexpr sizet kVis = static_cast<sizet>(AnimalWorkAxis::Visibility);
    constexpr sizet kShadow = static_cast<sizet>(AnimalWorkAxis::Shadow);

    // The three subjects the issue names, sized from the committed reference
    // grooms and rigs rather than invented. A census over made-up numbers
    // measures the numbers.
    //
    //   short coat  — reference-shortcoat-animal.ologroom, a Fox-scale rig
    //   long coat   — reference-longcoat-animal.ologroom, the dense subject
    //   hero        — the close-up: the long coat at full strand count with a
    //                 finer rig, which is what a hero actually costs
    struct Subject
    {
        const char* Name;
        u32 BoneCount;
        u32 GuidePointCount;
        u32 StrandCount;
        u32 ShadowVoxelCount;
        u32 DrawCallCount;
    };

    constexpr Subject kShortCoat{ "short-coat", 58u, 480u, 4000u, 32u * 32u * 32u, 2u };
    constexpr Subject kLongCoat{ "long-coat", 72u, 1600u, 24000u, 64u * 64u * 64u, 3u };
    constexpr Subject kHero{ "hero", 128u, 3200u, 60000u, 96u * 96u * 96u, 4u };

    [[nodiscard]] AnimalWorkItem MakeItem(u64 id, const Subject& subject, AnimalRole role, f32 pixelSize)
    {
        AnimalWorkItem item;
        item.Id = UUID{ id };
        item.Role = role;
        item.PixelSize = pixelSize;
        item.BoneCount = subject.BoneCount;
        item.GuidePointCount = subject.GuidePointCount;
        item.StrandCount = subject.StrandCount;
        item.ShadowVoxelCount = subject.ShadowVoxelCount;
        item.DrawCallCount = subject.DrawCallCount;
        item.SimulationSubsteps = 1u;
        item.Visible = true;
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            item.DesiredStep[a] = 0u;
            item.MaxStep[a] = 4u;
        }
        return item;
    }

    /// The population the whole issue is about: a close-up hero, a few featured
    /// animals at mid distance, and a herd receding into the background.
    ///
    /// The DISTANCES are the point. A census taken at one distance cannot show
    /// the dominant axis moving, and "which cost dominates" has no single
    /// answer for a population spread through a scene — which is itself one of
    /// the findings.
    [[nodiscard]] std::vector<AnimalWorkItem> MixedDistancePopulation(u32 herdCount, f32 herdPixelSize)
    {
        std::vector<AnimalWorkItem> items;
        items.reserve(herdCount + 5u);
        items.push_back(MakeItem(1u, kHero, AnimalRole::Hero, 720.0f));
        for (u32 i = 0; i < 4u; ++i)
        {
            items.push_back(MakeItem(10u + i, kLongCoat, AnimalRole::Featured, 260.0f));
        }
        for (u32 i = 0; i < herdCount; ++i)
        {
            const Subject& subject = (i % 2u == 0u) ? kShortCoat : kLongCoat;
            items.push_back(MakeItem(100u + i, subject, AnimalRole::Background, herdPixelSize));
        }
        return items;
    }

    struct CensusRow
    {
        std::string Label;
        std::array<f32, AnimalWorkAxisCount> AxisShare{};
        f32 DrawCallShare = 0.0f;
        f32 TotalUnits = 0.0f;
        sizet DominantAxis = 0u;
    };

    /// Price a population at full rate — the "what it would cost unbudgeted"
    /// arm, which is the only arm in which the shares describe the WORKLOAD
    /// rather than the scheduler's response to it.
    [[nodiscard]] CensusRow Census(const std::string& label, const std::vector<AnimalWorkItem>& items)
    {
        const AnimalCostModel model;
        CensusRow row;
        row.Label = label;

        std::array<f32, AnimalWorkAxisCount> axisUnits{};
        f32 drawUnits = 0.0f;
        for (const AnimalWorkItem& item : items)
        {
            const std::array<u32, AnimalWorkAxisCount> full{ 0u, 0u, 0u, 0u };
            drawUnits += model.PerDrawCall * static_cast<f32>(item.DrawCallCount);
            // Each axis on its own, by differencing against a step so coarse it
            // contributes nothing — so the split uses the SAME arithmetic the
            // scheduler prices with rather than a second copy that could drift.
            for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
            {
                std::array<u32, AnimalWorkAxisCount> off{ 30u, 30u, 30u, 30u };
                off[a] = 0u;
                std::array<u32, AnimalWorkAxisCount> none{ 30u, 30u, 30u, 30u };
                axisUnits[a] += EstimateAnimalCostUnits(model, item, off) - EstimateAnimalCostUnits(model, item, none);
            }
            row.TotalUnits += EstimateAnimalCostUnits(model, item, full);
        }

        row.DrawCallShare = row.TotalUnits > 0.0f ? drawUnits / row.TotalUnits : 0.0f;
        f32 best = -1.0f;
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            row.AxisShare[a] = row.TotalUnits > 0.0f ? axisUnits[a] / row.TotalUnits : 0.0f;
            if (row.AxisShare[a] > best)
            {
                best = row.AxisShare[a];
                row.DominantAxis = a;
            }
        }
        return row;
    }

    void PrintCensus(const CensusRow& row)
    {
        std::printf("  %-22s total=%10.0f u | deform=%5.1f%% sim=%5.1f%% vis=%5.1f%% shadow=%5.1f%% draw=%5.2f%% | "
                    "dominant=%s\n",
                    row.Label.c_str(), static_cast<f64>(row.TotalUnits),
                    static_cast<f64>(row.AxisShare[kDeform] * 100.0f), static_cast<f64>(row.AxisShare[kSim] * 100.0f),
                    static_cast<f64>(row.AxisShare[kVis] * 100.0f), static_cast<f64>(row.AxisShare[kShadow] * 100.0f),
                    static_cast<f64>(row.DrawCallShare * 100.0f),
                    ToString(static_cast<AnimalWorkAxis>(row.DominantAxis)).data());
    }
} // namespace

// -----------------------------------------------------------------------------
// The census itself — this is the number the analysis document reports
// -----------------------------------------------------------------------------

TEST(AnimalSchedulingCensus, TheCostBreakdownAcrossPopulationSizes)
{
    std::printf("\n[census] full-rate cost share by population, AnimalCostModel defaults\n");
    for (const u32 herd : { 8u, 32u, 96u, 256u })
    {
        const CensusRow row =
            Census("herd=" + std::to_string(herd), MixedDistancePopulation(herd, 90.0f));
        PrintCensus(row);

        // Reported, then asserted. The assertion is on the SHAPE, because the
        // exact percentages move whenever a coefficient is recalibrated and
        // pinning them would make a recalibration look like a regression.
        EXPECT_GT(row.TotalUnits, 0.0f);
        f32 sum = row.DrawCallShare;
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            sum += row.AxisShare[a];
        }
        EXPECT_NEAR(sum, 1.0f, 1e-3f) << "the shares must account for the whole frame or the census misleads";
    }
}

TEST(AnimalSchedulingCensus, DrawCallsAreNotTheLimitAtAnyTestedPopulationSize)
{
    // CRITERION 2, STATED AS THE ASSERTION IT IMPLIES. The issue tells us not
    // to assume draw calls are the limit; this is the measurement that settles
    // it, and it is written so it could have come out the other way. If a
    // future change made draw calls dominant this fails, and the correct
    // response would be batching (#1031) rather than more scheduling — which
    // is exactly the decision the criterion exists to keep honest.
    std::printf("\n[census] draw-call share — the cost no step can reach\n");
    for (const u32 herd : { 8u, 32u, 96u, 256u })
    {
        const CensusRow row = Census("herd=" + std::to_string(herd), MixedDistancePopulation(herd, 90.0f));
        std::printf("  herd=%-4u draw-call share %5.2f%%\n", herd, static_cast<f64>(row.DrawCallShare * 100.0f));

        EXPECT_LT(row.DrawCallShare, 0.05f)
            << "draw calls now carry more than 5% of a full-rate animal frame: scheduling is no longer the right "
               "lever and this feature's premise needs re-examining";
        EXPECT_NE(row.DominantAxis, AnimalWorkAxisCount) << "some axis must dominate";
    }
}

TEST(AnimalSchedulingCensus, TheDominantAxisMovesWithApparentSizeSoOneQualityScalarCannotBeRight)
{
    // The finding that justifies four independent axes rather than a single
    // "animal quality" slider: which work dominates is a function of how big
    // the animal is on screen, so a scalar that was right for the herd would be
    // wrong for the hero and vice versa.
    std::printf("\n[census] dominant axis by apparent size\n");

    std::vector<sizet> dominant;
    for (const f32 pixels : { 16.0f, 48.0f, 120.0f, 320.0f, 720.0f })
    {
        std::vector<AnimalWorkItem> items;
        items.push_back(MakeItem(1u, kLongCoat, AnimalRole::Background, pixels));
        // The ladder each animal would have asked for on its own, so the census
        // reflects what the frame REALLY submits rather than an unbudgeted
        // fiction: at 16 px a coat is already several halvings down.
        const u32 step = pixels >= 512.0f ? 0u : (pixels >= 256.0f ? 1u : (pixels >= 128.0f ? 2u : (pixels >= 64.0f ? 3u : 4u)));
        items[0].DesiredStep[kVis] = step;
        items[0].DesiredStep[kSim] = step;

        const AnimalCostModel model;
        std::array<u32, AnimalWorkAxisCount> steps{ 0u, step, step, 0u };
        const f32 total = EstimateAnimalCostUnits(model, items[0], steps);

        std::array<f32, AnimalWorkAxisCount> share{};
        f32 best = -1.0f;
        sizet bestAxis = 0u;
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            std::array<u32, AnimalWorkAxisCount> off{ 30u, 30u, 30u, 30u };
            off[a] = steps[a];
            std::array<u32, AnimalWorkAxisCount> none{ 30u, 30u, 30u, 30u };
            share[a] = (EstimateAnimalCostUnits(model, items[0], off) -
                        EstimateAnimalCostUnits(model, items[0], none)) /
                       total;
            if (share[a] > best)
            {
                best = share[a];
                bestAxis = a;
            }
        }
        dominant.push_back(bestAxis);
        std::printf("  %6.0f px  deform=%5.1f%% sim=%5.1f%% vis=%5.1f%% shadow=%5.1f%%  dominant=%s\n",
                    static_cast<f64>(pixels), static_cast<f64>(share[kDeform] * 100.0f),
                    static_cast<f64>(share[kSim] * 100.0f), static_cast<f64>(share[kVis] * 100.0f),
                    static_cast<f64>(share[kShadow] * 100.0f),
                    ToString(static_cast<AnimalWorkAxis>(bestAxis)).data());
    }

    const bool moved = std::adjacent_find(dominant.begin(), dominant.end(), std::not_equal_to<>()) != dominant.end();
    EXPECT_TRUE(moved) << "the dominant axis is the same at 16 px and at 720 px, which would mean one quality scalar "
                          "could have served the whole population and the four-axis design is unjustified";
}

// -----------------------------------------------------------------------------
// The reachable / unreachable split
// -----------------------------------------------------------------------------

TEST(AnimalSchedulingCensus, TheBudgetReachesTheMajorityOfTheFrameButNeverAllOfIt)
{
    // geometry-lod-measure-the-unreachable-cost.md rule 1, applied here: before
    // trusting a scheme that redistributes work, count the work it CANNOT
    // reach. For this population that is the draw calls plus whatever the
    // floors and the pose-step bound refuse to give up — and the second part is
    // the one that grows as the population does.
    const std::vector<AnimalWorkItem> items = MixedDistancePopulation(96u, 90.0f);
    const AnimalCostModel model;

    f32 full = 0.0f;
    f32 floorBound = 0.0f;
    for (const AnimalWorkItem& item : items)
    {
        const std::array<u32, AnimalWorkAxisCount> atFull{ 0u, 0u, 0u, 0u };
        full += EstimateAnimalCostUnits(model, item, atFull);

        // The cheapest this animal is ALLOWED to be: every axis at its cap,
        // with the visibility cap coming from the strand floor rather than from
        // the authored maximum.
        std::array<u32, AnimalWorkAxisCount> atCap{};
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            atCap[a] = item.MaxStep[a];
        }
        atCap[kVis] = MaxVisibilityStepForStrandFloor(item.StrandCount, 256u, item.MaxStep[kVis]);
        floorBound += EstimateAnimalCostUnits(model, item, atCap);
    }

    const f32 reachable = full - floorBound;
    std::printf("\n[census] full=%.0f u  irreducible=%.0f u  reachable=%.1f%% of the frame\n",
                static_cast<f64>(full), static_cast<f64>(floorBound), static_cast<f64>(100.0f * reachable / full));

    EXPECT_GT(reachable / full, 0.5f)
        << "the budget can reach less than half the frame, so scheduling cannot be the primary lever for this "
           "population and the caps or the floors need revisiting";
    EXPECT_GT(floorBound, 0.0f) << "an irreducible cost of zero means the floors are not being applied at all";
}

TEST(AnimalSchedulingCensus, APopulationThatFitsIsNotCoarsenedAtAll)
{
    // The control arm. A census is only meaningful beside the case where the
    // budget does nothing — otherwise "the scheduler changed the frame" is
    // indistinguishable from "the scheduler always changes the frame".
    const std::vector<AnimalWorkItem> items = MixedDistancePopulation(8u, 90.0f);

    AnimalBudgetPolicy policy;
    policy.Enabled = true;
    policy.FrameBudgetUnits = 1.0e7f; // comfortably above anything this costs

    std::vector<AnimalScheduleState> states(items.size());
    std::vector<AnimalScheduleSlot> slots;
    slots.reserve(items.size());
    for (sizet i = 0; i < items.size(); ++i)
    {
        AnimalScheduleSlot slot;
        slot.Item = items[i];
        slot.State = &states[i];
        slots.push_back(slot);
    }

    AnimalSchedulerStats stats;
    const std::vector<AnimalSchedule> schedules = ScheduleAnimalPopulation(policy, AnimalCostModel{}, slots, &stats);

    EXPECT_FALSE(stats.BudgetExceeded);
    EXPECT_EQ(stats.AnimalsCoarsened, 0u);
    EXPECT_EQ(stats.AnimalsAtDesired, static_cast<u32>(items.size()));
    for (const AnimalSchedule& schedule : schedules)
    {
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            EXPECT_EQ(schedule.Step[a], 0u) << "an affordable population must be left exactly alone";
        }
    }
}

// -----------------------------------------------------------------------------
// How wrong may the model be before its conclusion flips?
// -----------------------------------------------------------------------------

TEST(AnimalSchedulingCensus, TheDrawCallConclusionSurvivesALargeErrorInItsOwnCoefficient)
{
    // THE CENSUS IS PRICED THROUGH A MODEL, AND THE MODEL IS APPROXIMATE. §4 of
    // the analysis document says so: the coefficients are structural ratios, not
    // a per-axis wall-clock regression. So "draw calls are not the limit" is
    // only worth anything if it survives the coefficient it depends on being
    // wrong — and by how much is a number, not an opinion.
    //
    // This measures the MARGIN: the factor `PerDrawCall` would have to be scaled
    // by before draw calls became the single largest line in the frame. A
    // conclusion that survives a 20x error in its own input is robust at
    // order-of-magnitude accuracy, which is the accuracy actually claimed. One
    // that survived only a 1.5x error would not be, and this case would say so.
    const std::vector<AnimalWorkItem> items = MixedDistancePopulation(96u, 90.0f);
    const AnimalCostModel model;

    std::array<f32, AnimalWorkAxisCount> axisUnits{};
    f32 drawUnits = 0.0f;
    for (const AnimalWorkItem& item : items)
    {
        drawUnits += model.PerDrawCall * static_cast<f32>(item.DrawCallCount);
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            std::array<u32, AnimalWorkAxisCount> off{ 30u, 30u, 30u, 30u };
            off[a] = 0u;
            const std::array<u32, AnimalWorkAxisCount> none{ 30u, 30u, 30u, 30u };
            axisUnits[a] += EstimateAnimalCostUnits(model, item, off) - EstimateAnimalCostUnits(model, item, none);
        }
    }

    ASSERT_GT(drawUnits, 0.0f);
    const f32 largestAxis = *std::max_element(axisUnits.begin(), axisUnits.end());
    const f64 margin = static_cast<f64>(largestAxis) / static_cast<f64>(drawUnits);

    std::printf("\n[census] draw-call conclusion margin: PerDrawCall would have to be %.1fx larger (%.0f units "
                "instead of %.2f) before draw calls were the largest line\n",
                margin, static_cast<f64>(model.PerDrawCall) * margin, static_cast<f64>(model.PerDrawCall));

    EXPECT_GT(margin, 10.0)
        << "the 'draw calls are not the limit' conclusion now survives less than a 10x error in PerDrawCall. At the "
           "order-of-magnitude accuracy the cost model actually claims, that is no longer a safe conclusion — "
           "re-calibrate before trusting it, or the answer may be batching (#1031) after all";
}
