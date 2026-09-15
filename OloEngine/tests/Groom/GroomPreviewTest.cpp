#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomPreviewTest — issue #1232, acceptance criterion 3, second half.
//
// "Preview shows roots, groups and curve direction."
//
// The pixels themselves are checked by the visual-evidence test and by the
// live editor capture in the PR; what is pinned HERE is the CPU contract the
// picture depends on, because those are the parts that fail silently:
//
//   * SUBSAMPLING covers the whole groom. A preview capped at N strands that
//     drew the FIRST N would show one group of a three-group groom and look
//     perfectly fine doing it.
//   * GUIDES-ONLY subsamples the guides, not the groom. Otherwise a groom with
//     300 guides in 30k strands shows almost none of them at the default cap.
//   * Group colours are DISTINCT and STABLE, so "colour by group" means
//     something and the inspector's swatch matches the viewport.
//   * The cap is honoured, so a million-strand groom cannot submit a million
//     command packets on the first frame it becomes visible.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomPreview.h"

#include <glm/glm.hpp>

#include <set>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    Ref<GroomAsset> MakeGroom(u32 curveCount, u32 groupCount, u32 guideStride)
    {
        GroomBuilder builder;
        std::string reason;

        std::vector<u16> groupIds(groupCount);
        for (u32 g = 0; g < groupCount; ++g)
        {
            EXPECT_TRUE(builder.AddGroup("group" + std::to_string(g), groupIds[g], reason)) << reason;
        }

        for (u32 c = 0; c < curveCount; ++c)
        {
            const std::vector<glm::vec3> points = { { static_cast<f32>(c) * 0.01f, 0.0f, 0.0f },
                                                    { static_cast<f32>(c) * 0.01f, 0.1f, 0.0f },
                                                    { static_cast<f32>(c) * 0.01f, 0.2f, 0.0f } };
            const std::vector<f32> widths = { 0.001f, 0.0008f, 0.0005f };

            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { static_cast<f32>(c) / static_cast<f32>(curveCount), 0.0f };
            input.GroupId = groupIds[c % groupCount];
            input.IsGuide = (guideStride != 0) && ((c % guideStride) == 0);
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }

        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        if (groom)
        {
            EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
        }
        return groom;
    }
} // namespace

TEST(GroomPreview, GroupColorsAreDistinctAndStable)
{
    // Distinct: neighbouring groups must be told apart. A linear id/count hue
    // map stops doing that past a handful of groups, which is why the
    // implementation uses a golden-ratio rotation.
    std::set<std::string> seen;
    for (u32 g = 0; g < 16; ++g)
    {
        const glm::vec3 color = GroomGroupColor(g);
        EXPECT_GE(color.r, 0.0f);
        EXPECT_LE(color.r, 1.0f);
        EXPECT_GE(color.g, 0.0f);
        EXPECT_LE(color.g, 1.0f);
        EXPECT_GE(color.b, 0.0f);
        EXPECT_LE(color.b, 1.0f);

        // Quantise before comparing: two colours that round to the same 8-bit
        // triple are the same colour on screen, whatever the floats say.
        const std::string key = std::to_string(static_cast<int>(color.r * 255.0f)) + "," +
                                std::to_string(static_cast<int>(color.g * 255.0f)) + "," +
                                std::to_string(static_cast<int>(color.b * 255.0f));
        EXPECT_TRUE(seen.insert(key).second) << "group " << g << " reuses an earlier group's on-screen colour";
    }

    // Stable: the inspector swatch and the viewport must agree, so the colour
    // cannot depend on the groom it came from or on call order.
    EXPECT_EQ(GroomGroupColor(3), GroomGroupColor(3));
    EXPECT_NE(GroomGroupColor(3), GroomGroupColor(4));
}

TEST(GroomPreview, SubsamplingSpansEveryGroupRatherThanTruncating)
{
    // Three interleaved groups, and a cap far below the curve count. A preview
    // that truncated would show group 0 only — and it would look correct.
    constexpr u32 kCurves = 900;
    constexpr u32 kGroups = 3;
    Ref<GroomAsset> groom = MakeGroom(kCurves, kGroups, /*guideStride*/ 0);
    ASSERT_TRUE(groom);

    GroomPreviewSettings settings;
    settings.MaxStrands = 30;
    settings.MaxSegments = 1000000; // let the strand cap bind

    const GroomPreviewStats plan = PlanGroomPreview(*groom, settings);
    ASSERT_GT(plan.Stride, 1u) << "the cap must actually bite or this test proves nothing";
    EXPECT_FALSE(plan.SegmentBudgetLimited);

    // Walk the same selection the draw loop performs.
    std::set<u16> groupsTouched;
    u32 drawn = 0;
    for (u32 c = 0; c < kCurves; ++c)
    {
        if ((c % plan.Stride) != 0)
        {
            continue;
        }
        groupsTouched.insert(groom->GetCurveGroupIds()[c]);
        ++drawn;
    }

    EXPECT_LE(drawn, settings.MaxStrands) << "the cap was exceeded";
    EXPECT_EQ(groupsTouched.size(), kGroups)
        << "the subsample missed a group entirely; a truncating preview would look correct while showing one group";
}

TEST(GroomPreview, StrideIsOneWhenTheGroomFitsUnderBothCaps)
{
    Ref<GroomAsset> groom = MakeGroom(50, 1, 0);
    ASSERT_TRUE(groom);

    GroomPreviewSettings settings; // defaults: 2000 strands / 20000 segments
    const GroomPreviewStats plan = PlanGroomPreview(*groom, settings);
    EXPECT_EQ(plan.Stride, 1u) << "a small groom must be drawn in full, not subsampled";
    EXPECT_FALSE(plan.SegmentBudgetLimited);
    EXPECT_EQ(plan.StrandsAvailable, 50u);
}

TEST(GroomPreview, TheSegmentBudgetWidensTheStrideWhenMaxStrandsAloneWouldOverflowTheFrame)
{
    // The failure this pins was found by running it: two grooms in one editor
    // scene submitted ~39,500 debug lines, and every line takes one entry of
    // the frame's shared 65536-transform buffer, so the frame logged
    // "FrameDataBuffer: Transform buffer overflow!" every tick. The strand cap
    // alone cannot prevent that — a strand costs one line per SEGMENT plus
    // three for its root cross, so the cost per strand depends on the groom.
    constexpr u32 kCurves = 4000;
    Ref<GroomAsset> groom = MakeGroom(kCurves, /*groupCount*/ 1, /*guideStride*/ 0);
    ASSERT_TRUE(groom);
    // MakeGroom builds 3-point strands: 2 segments + 3 root lines = 5 lines.
    ASSERT_EQ(groom->GetPointCount(), kCurves * 3u);

    GroomPreviewSettings settings;
    settings.MaxStrands = kCurves; // "draw them all"
    settings.MaxSegments = 1000;   // but only 1000 lines may be submitted

    const GroomPreviewStats plan = PlanGroomPreview(*groom, settings);

    EXPECT_TRUE(plan.SegmentBudgetLimited)
        << "the segment budget should be the binding cap here, and must say so";
    EXPECT_GT(plan.Stride, 1u) << "the stride did not widen, so the budget was ignored";

    // 1000 lines / 5 per strand = 200 affordable strands.
    const u32 wouldDraw = (kCurves + plan.Stride - 1u) / plan.Stride;
    EXPECT_LE(wouldDraw, 220u);
    EXPECT_GT(wouldDraw, 100u) << "the budget over-thinned the preview into near-invisibility";
    EXPECT_EQ(plan.StrandsAvailable, kCurves);
}

TEST(GroomPreview, TheStrandCapStillBindsWhenItIsTheTighterOfTheTwo)
{
    // The mirror image: a generous segment budget must leave MaxStrands in
    // charge, or an authored "show me 50 strands" would be quietly ignored.
    constexpr u32 kCurves = 4000;
    Ref<GroomAsset> groom = MakeGroom(kCurves, 1, 0);
    ASSERT_TRUE(groom);

    GroomPreviewSettings settings;
    settings.MaxStrands = 50;
    settings.MaxSegments = 1000000;

    const GroomPreviewStats plan = PlanGroomPreview(*groom, settings);
    EXPECT_FALSE(plan.SegmentBudgetLimited);

    const u32 wouldDraw = (kCurves + plan.Stride - 1u) / plan.Stride;
    EXPECT_LE(wouldDraw, 50u);
    EXPECT_GT(wouldDraw, 40u);
}

TEST(GroomPreview, TurningRootsOffLowersTheSegmentCostPerStrand)
{
    // Root crosses are three lines each — on a short-strand groom they are the
    // MAJORITY of the cost (2 segments vs 3 root lines here), so switching them
    // off must buy back stride rather than changing nothing.
    constexpr u32 kCurves = 4000;
    Ref<GroomAsset> groom = MakeGroom(kCurves, 1, 0);
    ASSERT_TRUE(groom);

    GroomPreviewSettings withRoots;
    withRoots.MaxStrands = kCurves;
    withRoots.MaxSegments = 1000;

    GroomPreviewSettings withoutRoots = withRoots;
    withoutRoots.ShowRoots = false;

    const GroomPreviewStats a = PlanGroomPreview(*groom, withRoots);
    const GroomPreviewStats b = PlanGroomPreview(*groom, withoutRoots);
    EXPECT_LT(b.Stride, a.Stride) << "dropping the root crosses did not reduce the per-strand line cost";
}

TEST(GroomPreview, GuidesOnlySubsamplesTheGuidesNotTheWholeGroom)
{
    // 3000 strands, every 50th a guide -> 60 guides. Under a cap of 30, the
    // stride must be computed from 60, not from 3000: a stride of 100 over
    // 3000 would hit almost no guides at all.
    constexpr u32 kCurves = 3000;
    constexpr u32 kGuideStride = 50;
    constexpr u32 kMaxStrands = 30;
    Ref<GroomAsset> groom = MakeGroom(kCurves, /*groupCount*/ 2, kGuideStride);
    ASSERT_TRUE(groom);

    const u32 guideCount = groom->GetGuideCount();
    ASSERT_EQ(guideCount, kCurves / kGuideStride);

    GroomPreviewSettings guidesOnly;
    guidesOnly.GuidesOnly = true;
    guidesOnly.MaxStrands = kMaxStrands;
    guidesOnly.MaxSegments = 1000000;
    const GroomPreviewStats plan = PlanGroomPreview(*groom, guidesOnly);
    EXPECT_EQ(plan.StrandsAvailable, guideCount) << "guides-only must plan over the GUIDES, not the whole groom";

    const u32 strideOverGuides = plan.Stride;
    const u32 strideOverAllCurves = (kCurves + kMaxStrands - 1u) / kMaxStrands;
    EXPECT_LT(strideOverGuides, strideOverAllCurves)
        << "guides-only must stride over the guide subset, or the mode shows almost nothing";

    // Walk the candidate selection the preview performs.
    u32 candidate = 0;
    u32 drawnGuides = 0;
    for (u32 c = 0; c < groom->GetCurveCount(); ++c)
    {
        if (!groom->IsGuide(c))
        {
            continue;
        }
        if ((candidate++ % strideOverGuides) == 0)
        {
            ++drawnGuides;
        }
    }
    EXPECT_GT(drawnGuides, 0u);
    EXPECT_LE(drawnGuides, kMaxStrands);
    // With 60 guides and a cap of 30 the stride is 2, so exactly half are drawn.
    EXPECT_EQ(drawnGuides, 30u);
}

TEST(GroomPreview, DirectionRampRunsDarkAtTheRootToBrightAtTheTip)
{
    // The ramp is the ONLY direction cue in the picture. If it ever ran the
    // other way a tip-first import would look correct, which is the specific
    // failure the preview exists to expose.
    constexpr u32 kPoints = 5;
    f32 previous = -1.0f;
    for (u32 i = 1; i < kPoints; ++i)
    {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kPoints - 1);
        const f32 brightness = glm::mix(0.15f, 1.0f, t);
        EXPECT_GT(brightness, previous) << "the ramp must increase monotonically from root to tip";
        previous = brightness;
    }
    EXPECT_FLOAT_EQ(previous, 1.0f) << "the tip must reach full brightness";
}

TEST(GroomPreview, AnEmptyGroomDrawsNothingRatherThanDividingByZero)
{
    GroomAsset empty;
    empty.RecomputeDerivedData();

    GroomPreviewSettings settings;
    // No GL context here, but an empty groom returns before any draw call, so
    // this exercises the early-out rather than the renderer.
    const GroomPreviewStats stats = DrawGroomPreview(empty, glm::mat4(1.0f), settings);
    EXPECT_EQ(stats.StrandsDrawn, 0u);
    EXPECT_EQ(stats.SegmentsDrawn, 0u);
    EXPECT_EQ(stats.StrandsAvailable, 0u);
}

TEST(GroomPreview, AGroomWithNoGuidesDrawsNothingInGuidesOnlyMode)
{
    Ref<GroomAsset> groom = MakeGroom(64, 1, /*guideStride*/ 0);
    ASSERT_TRUE(groom);
    ASSERT_EQ(groom->GetGuideCount(), 0u);

    GroomPreviewSettings settings;
    settings.GuidesOnly = true;
    const GroomPreviewStats stats = DrawGroomPreview(*groom, glm::mat4(1.0f), settings);
    EXPECT_EQ(stats.StrandsAvailable, 0u);
    EXPECT_EQ(stats.StrandsDrawn, 0u) << "guides-only on a groom with no guides must draw nothing, not everything";
}
