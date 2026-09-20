// OLO_TEST_LAYER: unit
//
// =============================================================================
// GroomGuideInfluenceTest.cpp — how a rendered strand learns what its guides
// did. Issue #1250.
//
// The solver is tested next door. This file is the OTHER half of the feature,
// and it is the half where a correct solver still looks wrong: a strand that
// takes its motion from a guide on the wrong side of the animal, or that is
// dragged toward a longer guide's tip, produces a coat that splays under motion
// while every particle in the simulation is perfectly inextensible.
// =============================================================================

#include "OloEngine/Groom/GroomGuideInfluence.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"

#include "Groom/GroomStrandFixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    /// A groom of `count` strands laid along +X, every `guideEvery`-th flagged
    /// as a guide, all in one group unless `groupStride` splits them. Straight
    /// and evenly spaced so "the nearest guide" is a fact anyone can check by
    /// counting rather than a property of a hash.
    [[nodiscard]] Ref<GroomAsset> MakeLine(u32 count, u32 guideEvery, u32 groupCount = 1u, u32 points = 6u)
    {
        GroomBuilder builder;
        std::string reason;
        std::vector<u16> groups(groupCount);
        for (u32 g = 0; g < groupCount; ++g)
        {
            EXPECT_TRUE(builder.AddGroup("g" + std::to_string(g), groups[g], reason)) << reason;
        }

        std::vector<glm::vec3> pts(points);
        std::vector<f32> widths(points, 1.0e-4f);
        for (u32 s = 0; s < count; ++s)
        {
            const glm::vec3 root{ static_cast<f32>(s), 0.0f, 0.0f };
            for (u32 p = 0; p < points; ++p)
            {
                pts[p] = root + glm::vec3(0.0f, static_cast<f32>(p) * 0.1f, 0.0f);
            }
            GroomCurveInput input;
            input.Points = pts;
            input.Widths = widths;
            input.RootUV = { static_cast<f32>(s) / static_cast<f32>(count), 0.5f };
            // Interleaved, so "same group" is not the same thing as "nearby".
            input.GroupId = groups[s % groupCount];
            input.IsGuide = (s % guideEvery) == 0u;
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }
        auto groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        if (groom)
        {
            EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
        }
        return groom;
    }

    /// A simulation view over `table` in which EVERY guide is simulated, with
    /// the given per-guide-point displacements. The identity mapping is what
    /// most of these cases want; the budget cases build their own.
    struct SimulationFixture
    {
        Ref<GroomGuideInfluenceTable> Table;
        std::vector<u32> Offsets;
        std::vector<u32> GuideOfSlot;
        std::vector<u32> SlotOfGuide;
        std::vector<glm::vec3> Displacements;
        std::vector<glm::vec3> PrevDisplacements;

        [[nodiscard]] GroomStrandSimulation View() const
        {
            GroomStrandSimulation sim;
            sim.Influence = Table.Raw();
            sim.GuideOfSlot = GuideOfSlot;
            sim.Displacements.GuideOffsets = Offsets;
            sim.Displacements.Displacements = Displacements;
            sim.Displacements.PrevDisplacements = PrevDisplacements;
            sim.Displacements.SlotOfGuide = SlotOfGuide;
            return sim;
        }
    };

    /// Build a fixture over every guide of `groom`, with each guide's every
    /// point displaced by `perGuide(slot)`.
    template<typename Fn>
    [[nodiscard]] SimulationFixture MakeSimulation(const GroomAsset& groom,
                                                   const Ref<GroomGuideInfluenceTable>& table, Fn perGuide)
    {
        SimulationFixture fixture;
        fixture.Table = table;
        fixture.Offsets.push_back(0u);
        for (u32 slot = 0; slot < table->GetGuideCount(); ++slot)
        {
            const u32 curve = table->GetGuideCurves()[slot];
            const u32 points = groom.GetCurvePointCount(curve);
            for (u32 p = 0; p < points; ++p)
            {
                fixture.Displacements.push_back(perGuide(slot));
            }
            fixture.Offsets.push_back(static_cast<u32>(fixture.Displacements.size()));
            fixture.GuideOfSlot.push_back(slot);
            fixture.SlotOfGuide.push_back(slot);
        }
        return fixture;
    }
} // namespace

// -----------------------------------------------------------------------------
// The table
// -----------------------------------------------------------------------------

// A guide drives ITSELF and nothing else drives it. Blending a guide toward its
// neighbours would make the rendered guide disagree with the particle the solver
// moved, so the debug overlay and the coat would draw two different curves and
// only one of them would be the simulation.
TEST(GroomGuideInfluence, AGuideIsItsOwnSoleInfluence)
{
    auto groom = MakeLine(40, 5);
    ASSERT_TRUE(groom);
    auto table = BuildGroomGuideInfluence(*groom);
    ASSERT_TRUE(table);
    ASSERT_GT(table->GetGuideCount(), 0u);

    for (u32 slot = 0; slot < table->GetGuideCount(); ++slot)
    {
        const u32 curve = table->GetGuideCurves()[slot];
        const GroomGuideWeights& weights = table->GetWeights()[curve];
        EXPECT_EQ(weights.Guides[0], slot) << "curve " << curve;
        EXPECT_EQ(weights.Weights[0], 1.0f) << "curve " << curve;
        for (u32 k = 1; k < GroomGuideInfluenceCount; ++k)
        {
            EXPECT_EQ(weights.Guides[k], GroomNoGuide);
            EXPECT_EQ(weights.Weights[k], 0.0f);
        }
    }
}

// The weights are a partition of unity over the slots that are used. Anything
// else scales the whole coat's motion by an arbitrary factor that moves with
// how many guides happened to be nearby.
TEST(GroomGuideInfluence, WeightsSumToOneOverTheUsedSlots)
{
    auto groom = MakeLine(64, 7);
    ASSERT_TRUE(groom);
    auto table = BuildGroomGuideInfluence(*groom);

    u32 weighted = 0;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        const GroomGuideWeights& weights = table->GetWeights()[curve];
        f32 total = 0.0f;
        u32 used = 0;
        for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
        {
            if (weights.Guides[k] == GroomNoGuide)
            {
                EXPECT_EQ(weights.Weights[k], 0.0f) << "an unused slot must carry no weight";
                continue;
            }
            EXPECT_LT(weights.Guides[k], table->GetGuideCount());
            EXPECT_GT(weights.Weights[k], 0.0f);
            total += weights.Weights[k];
            ++used;
        }
        if (used == 0u)
        {
            continue;
        }
        ++weighted;
        EXPECT_NEAR(total, 1.0f, 1.0e-5f) << "curve " << curve;
    }
    EXPECT_EQ(weighted, groom->GetCurveCount()) << "every strand of a single-group groom has guides";
}

// A strand is influenced only by guides in its OWN group. The failure this
// prevents is a whisker blended toward the undercoat, which is what a purely
// geometric nearest-neighbour search on a muzzle does.
TEST(GroomGuideInfluence, InfluenceNeverCrossesAGroup)
{
    // Three interleaved groups, and a guide stride COPRIME with the group
    // count. With `guideEvery` 6 and 3 groups every guide lands on s % 3 == 0,
    // so groups 1 and 2 get no guides at all and the loop below only ever sees
    // their empty slots -- it would still catch a group-blind search that handed
    // group-0 guides to them, but it would never exercise the case where all
    // three groups HAVE guides and the wrong one could be chosen. 5 and 3 are
    // coprime, so the guides cycle through all three groups.
    auto groom = MakeLine(90, 5, 3);
    ASSERT_TRUE(groom);
    auto table = BuildGroomGuideInfluence(*groom);

    const auto& groups = groom->GetCurveGroupIds();

    // EVERY group must actually own guides, or this case silently degrades into
    // the weaker one the comment above describes.
    std::set<u16> groupsWithGuides;
    for (const u32 guideCurve : table->GetGuideCurves())
    {
        groupsWithGuides.insert(groups[guideCurve]);
    }
    ASSERT_EQ(groupsWithGuides.size(), 3u) << "the stride must spread guides across all three groups";

    u32 checked = 0;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        const GroomGuideWeights& weights = table->GetWeights()[curve];
        for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
        {
            if (weights.Guides[k] == GroomNoGuide)
            {
                continue;
            }
            const u32 guideCurve = table->GetGuideCurves()[weights.Guides[k]];
            EXPECT_EQ(groups[guideCurve], groups[curve]) << "curve " << curve << " slot " << k;
            ++checked;
        }
    }
    EXPECT_GT(checked, 0u) << "an assertion that examined no slots proves nothing";
}

// A group groomed with no guide leaves its strands at the groomed rest shape —
// a coat that visibly does not move, which is diagnosable, rather than one that
// moves with somebody else's guides. Counted, so the editor can say so.
TEST(GroomGuideInfluence, AGroupWithNoGuideLeavesItsStrandsUnguidedAndCountsThem)
{
    GroomBuilder builder;
    std::string reason;
    u16 withGuides = 0;
    u16 without = 0;
    ASSERT_TRUE(builder.AddGroup("guided", withGuides, reason)) << reason;
    ASSERT_TRUE(builder.AddGroup("bare", without, reason)) << reason;

    const std::vector<glm::vec3> pts{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.1f, 0.0f }, { 0.0f, 0.2f, 0.0f } };
    const std::vector<f32> widths{ 1.0e-4f, 1.0e-4f, 1.0e-4f };
    for (u32 s = 0; s < 20; ++s)
    {
        GroomCurveInput input;
        std::vector<glm::vec3> moved = pts;
        for (glm::vec3& p : moved)
        {
            p.x += static_cast<f32>(s);
        }
        input.Points = moved;
        input.Widths = widths;
        input.RootUV = { 0.5f, 0.5f };
        input.GroupId = (s < 10u) ? withGuides : without;
        input.IsGuide = (s < 10u) && (s % 4u == 0u);
        ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;
    }
    auto groom = builder.Build(reason);
    ASSERT_TRUE(groom) << reason;
    ASSERT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;

    auto table = BuildGroomGuideInfluence(*groom);
    EXPECT_EQ(table->GetUnguidedStrands(), 10u);

    const auto& groups = groom->GetCurveGroupIds();
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        if (groups[curve] != without)
        {
            continue;
        }
        EXPECT_EQ(table->GetWeights()[curve].Guides[0], GroomNoGuide) << "curve " << curve;
    }
}

TEST(GroomGuideInfluence, AGroomWithNoGuidesProducesATableRatherThanANull)
{
    // MakeLine always flags curve 0, so this case builds its own groom.
    GroomBuilder builder;
    std::string reason;
    u16 group = 0;
    ASSERT_TRUE(builder.AddGroup("g", group, reason)) << reason;
    const std::vector<glm::vec3> pts{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.1f, 0.0f } };
    const std::vector<f32> widths{ 1.0e-4f, 1.0e-4f };
    for (u32 s = 0; s < 5; ++s)
    {
        GroomCurveInput input;
        input.Points = pts;
        input.Widths = widths;
        input.RootUV = { 0.5f, 0.5f };
        input.GroupId = group;
        input.IsGuide = false;
        ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;
    }
    auto bare = builder.Build(reason);
    ASSERT_TRUE(bare) << reason;

    auto table = BuildGroomGuideInfluence(*bare);
    ASSERT_TRUE(table) << "never null: a guide-free groom is a description, not an error";
    EXPECT_EQ(table->GetGuideCount(), 0u);
    EXPECT_EQ(table->GetUnguidedStrands(), bare->GetCurveCount());
    EXPECT_EQ(table->GetWeights().size(), bare->GetCurveCount());
}

// The table is a pure function of the asset. Two builds must agree, or the
// cached table and a freshly built one describe different coats.
TEST(GroomGuideInfluence, IsDeterministic)
{
    const auto coat = Tests::GroomStrandFixture::MakePelt(2000, 8);
    ASSERT_TRUE(coat.Groom) << coat.FailureReason;
    auto a = BuildGroomGuideInfluence(*coat.Groom);
    auto b = BuildGroomGuideInfluence(*coat.Groom);
    EXPECT_EQ(a->GetGuideCurves(), b->GetGuideCurves());
    EXPECT_EQ(a->GetWeights(), b->GetWeights());
    EXPECT_EQ(a->GetUnguidedStrands(), b->GetUnguidedStrands());
}

// -----------------------------------------------------------------------------
// The sample
// -----------------------------------------------------------------------------

// The claim the whole "off costs nothing" story rests on: guides that did not
// move contribute exactly zero, so a coat whose solver is idle is the coat
// #1251 built, bit for bit.
TEST(GroomGuideInfluence, UndisplacedGuidesContributeExactlyZero)
{
    auto groom = MakeLine(32, 4);
    ASSERT_TRUE(groom);
    auto table = BuildGroomGuideInfluence(*groom);
    const SimulationFixture fixture =
        MakeSimulation(*groom, table, [](u32)
                       { return glm::vec3(0.0f); });
    const GroomStrandSimulation sim = fixture.View();
    ASSERT_TRUE(sim.IsUsable(groom->GetCurveCount()));

    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        for (const f32 t : { 0.0f, 0.5f, 1.0f })
        {
            EXPECT_EQ(SampleGroomGuideDisplacement(sim, curve, t, false), glm::vec3(0.0f));
        }
    }
}

// A uniform displacement over every guide must arrive at every strand at FULL
// amplitude. A partition of unity that is renormalised wrongly shows up here as
// a coat that moves less than its guides, which reads as extra damping.
TEST(GroomGuideInfluence, AUniformGuideDisplacementArrivesUnattenuated)
{
    auto groom = MakeLine(48, 5);
    ASSERT_TRUE(groom);
    auto table = BuildGroomGuideInfluence(*groom);
    const glm::vec3 shift{ 0.3f, -0.2f, 0.7f };
    const SimulationFixture fixture = MakeSimulation(*groom, table, [&](u32)
                                                     { return shift; });
    const GroomStrandSimulation sim = fixture.View();

    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        const glm::vec3 sample = SampleGroomGuideDisplacement(sim, curve, 0.5f, false);
        EXPECT_NEAR(sample.x, shift.x, 1.0e-5f) << "curve " << curve;
        EXPECT_NEAR(sample.y, shift.y, 1.0e-5f) << "curve " << curve;
        EXPECT_NEAR(sample.z, shift.z, 1.0e-5f) << "curve " << curve;
    }
}

// SAMPLED BY PARAMETER, never by index. A guide with many points and a strand
// with few must agree about where "halfway up" is, or the short strand is
// dragged toward the long guide's tip and the coat splays under motion.
TEST(GroomGuideInfluence, SamplingIsByParameterNotByIndex)
{
    // One guide with 9 points, whose displacement ramps linearly from zero at
    // the root to one at the tip. A strand reading it at t must get t.
    GroomBuilder builder;
    std::string reason;
    u16 group = 0;
    ASSERT_TRUE(builder.AddGroup("g", group, reason)) << reason;

    const auto addCurve = [&](u32 points, bool isGuide, f32 x)
    {
        std::vector<glm::vec3> pts(points);
        std::vector<f32> widths(points, 1.0e-4f);
        for (u32 p = 0; p < points; ++p)
        {
            pts[p] = { x, static_cast<f32>(p) * 0.05f, 0.0f };
        }
        GroomCurveInput input;
        input.Points = pts;
        input.Widths = widths;
        input.RootUV = { 0.5f, 0.5f };
        input.GroupId = group;
        input.IsGuide = isGuide;
        ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;
    };
    addCurve(9, true, 0.0f);
    addCurve(3, false, 0.001f); // a short strand right beside the guide

    auto groom = builder.Build(reason);
    ASSERT_TRUE(groom) << reason;
    auto table = BuildGroomGuideInfluence(*groom);
    ASSERT_EQ(table->GetGuideCount(), 1u);

    SimulationFixture fixture;
    fixture.Table = table;
    const u32 guideCurve = table->GetGuideCurves()[0];
    const u32 guidePoints = groom->GetCurvePointCount(guideCurve);
    fixture.Offsets = { 0u, guidePoints };
    fixture.GuideOfSlot = { 0u };
    fixture.SlotOfGuide = { 0u };
    for (u32 p = 0; p < guidePoints; ++p)
    {
        const f32 u = static_cast<f32>(p) / static_cast<f32>(guidePoints - 1u);
        fixture.Displacements.push_back(glm::vec3(u, 0.0f, 0.0f));
    }
    const GroomStrandSimulation sim = fixture.View();

    const u32 strand = (guideCurve == 0u) ? 1u : 0u;
    for (const f32 t : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        EXPECT_NEAR(SampleGroomGuideDisplacement(sim, strand, t, false).x, t, 1.0e-5f) << "t " << t;
    }
}

// A slot the budget did not simulate is SKIPPED and the remaining weights are
// renormalised. Dropping the strand entirely would make lowering the guide
// budget freeze random strands rather than coarsen the motion, and scaling by
// the surviving weight without renormalising would make raising the budget look
// like raising the simulation's strength.
TEST(GroomGuideInfluence, ASlotOutsideTheBudgetIsSkippedAndTheRestRenormalised)
{
    auto groom = MakeLine(40, 4);
    ASSERT_TRUE(groom);
    auto table = BuildGroomGuideInfluence(*groom);
    ASSERT_GT(table->GetGuideCount(), 2u);

    const glm::vec3 shift{ 1.0f, 0.0f, 0.0f };
    SimulationFixture fixture = MakeSimulation(*groom, table, [&](u32)
                                               { return shift; });
    // Drop every odd slot from this frame's budget.
    for (u32 slot = 1; slot < fixture.GuideOfSlot.size(); slot += 2u)
    {
        fixture.GuideOfSlot[slot] = GroomNoGuide;
    }
    const GroomStrandSimulation sim = fixture.View();

    u32 sampled = 0;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        if (!HasGroomGuideInfluence(sim, curve))
        {
            continue;
        }
        ++sampled;
        const glm::vec3 sample = SampleGroomGuideDisplacement(sim, curve, 0.5f, false);
        EXPECT_NEAR(sample.x, shift.x, 1.0e-5f)
            << "curve " << curve << " must follow its surviving guides at full amplitude";
    }
    EXPECT_GT(sampled, 0u);
}

// An absent previous frame is "the same as current", which makes the velocity a
// shader derives EXACTLY zero rather than approximately zero — both ends go
// through identical arithmetic, the rule GroomStrandVertex::PrevPosition is
// written under.
TEST(GroomGuideInfluence, AnAbsentPreviousFrameAliasesTheCurrentOne)
{
    auto groom = MakeLine(16, 4);
    ASSERT_TRUE(groom);
    auto table = BuildGroomGuideInfluence(*groom);
    SimulationFixture fixture =
        MakeSimulation(*groom, table, [](u32 slot)
                       { return glm::vec3(static_cast<f32>(slot) * 0.1f, 0.2f, 0.0f); });
    fixture.PrevDisplacements.clear();
    const GroomStrandSimulation sim = fixture.View();

    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        EXPECT_EQ(SampleGroomGuideDisplacement(sim, curve, 0.5f, true),
                  SampleGroomGuideDisplacement(sim, curve, 0.5f, false))
            << "curve " << curve;
    }
}

TEST(GroomGuideInfluence, ANonFiniteParameterYieldsNoDisplacement)
{
    auto groom = MakeLine(8, 4);
    ASSERT_TRUE(groom);
    auto table = BuildGroomGuideInfluence(*groom);
    const SimulationFixture fixture =
        MakeSimulation(*groom, table, [](u32)
                       { return glm::vec3(1.0f, 1.0f, 1.0f); });
    const GroomStrandSimulation sim = fixture.View();

    EXPECT_EQ(SampleGroomGuideDisplacement(sim, 0u, std::numeric_limits<f32>::quiet_NaN(), false),
              glm::vec3(0.0f));
    EXPECT_EQ(SampleGroomGuideDisplacement(sim, groom->GetCurveCount(), 0.5f, false), glm::vec3(0.0f));
}
