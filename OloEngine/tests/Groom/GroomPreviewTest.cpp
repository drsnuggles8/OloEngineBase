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

    // One group, `count` curves, where every `longEvery`'th curve has
    // `longPoints` control points and the rest have two. The planner's
    // lines-per-strand is an AVERAGE, so a layout whose long strands land on
    // stride-aligned indices makes that average understate the real cost of the
    // curves actually selected — which is the only thing the exact submission
    // guard is there to catch.
    Ref<GroomAsset> MakeGroomWithVaryingLengths(u32 count, u32 longEvery, u32 longPoints)
    {
        GroomBuilder builder;
        std::string reason;
        u16 group = 0;
        EXPECT_TRUE(builder.AddGroup("varying", group, reason)) << reason;

        for (u32 c = 0; c < count; ++c)
        {
            const u32 points = ((c % longEvery) == 0) ? longPoints : 2u;
            std::vector<glm::vec3> positions;
            std::vector<f32> widths;
            positions.reserve(points);
            widths.reserve(points);
            for (u32 i = 0; i < points; ++i)
            {
                positions.emplace_back(static_cast<f32>(c) * 0.01f, static_cast<f32>(i) * 0.01f, 0.0f);
                widths.push_back(0.001f);
            }

            GroomCurveInput input;
            input.Points = positions;
            input.Widths = widths;
            input.GroupId = group;
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

    // Groups of EXPLICIT, unequal sizes. MakeGroom above splits curves evenly
    // (c % groupCount), and an even split is exactly the case where a global
    // modulo stride happens to hit every group — so it cannot show the bug
    // per-group phasing fixes.
    Ref<GroomAsset> MakeGroomWithGroupSizes(const std::vector<u32>& groupSizes)
    {
        GroomBuilder builder;
        std::string reason;

        std::vector<u16> groupIds(groupSizes.size());
        for (sizet g = 0; g < groupSizes.size(); ++g)
        {
            EXPECT_TRUE(builder.AddGroup("group" + std::to_string(g), groupIds[g], reason)) << reason;
        }

        u32 curve = 0;
        for (sizet g = 0; g < groupSizes.size(); ++g)
        {
            for (u32 i = 0; i < groupSizes[g]; ++i, ++curve)
            {
                const std::vector<glm::vec3> points = { { static_cast<f32>(curve) * 0.01f, 0.0f, 0.0f },
                                                        { static_cast<f32>(curve) * 0.01f, 0.1f, 0.0f },
                                                        { static_cast<f32>(curve) * 0.01f, 0.2f, 0.0f } };
                const std::vector<f32> widths = { 0.001f, 0.0008f, 0.0005f };

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { 0.0f, 0.0f };
                input.GroupId = groupIds[g];
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }
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
    // cannot depend on the groom it came from or on call order. Per component
    // rather than EXPECT_EQ on the vector — this repo forbids == / != on glm
    // types, and the per-component form also names which channel drifted.
    const glm::vec3 a = GroomGroupColor(3);
    const glm::vec3 again = GroomGroupColor(3);
    EXPECT_FLOAT_EQ(a.r, again.r);
    EXPECT_FLOAT_EQ(a.g, again.g);
    EXPECT_FLOAT_EQ(a.b, again.b);

    // And a different group is a different colour by a margin that survives
    // 8-bit quantisation, which is what "distinguishable on screen" means.
    const glm::vec3 other = GroomGroupColor(4);
    EXPECT_GT(glm::length(a - other), 1.0f / 255.0f);
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

    // The REAL selection, not a re-derivation of it: a test that reimplements
    // the rule keeps passing while the implementation drifts away from it.
    GroomPreviewStats stats = plan;
    std::vector<u32> selected;
    SelectGroomPreviewCurves(*groom, settings, stats, selected);

    std::set<u16> groupsTouched;
    for (const u32 c : selected)
    {
        groupsTouched.insert(groom->GetCurveGroupIds()[c]);
    }

    EXPECT_LE(selected.size(), settings.MaxStrands) << "the cap was exceeded";
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
    // 3000 strands, every 25th a guide -> 120 guides. Under a cap of 30, the
    // stride must be computed from 120, not from 3000: a stride of 100 over
    // 3000 would hit almost no guides at all.
    constexpr u32 kCurves = 3000;
    // ODD on purpose. MakeGroom splits the two groups by parity (c % 2) and
    // marks every kGuideStride'th curve a guide, so an EVEN stride puts every
    // guide in group 0 and leaves group 1 with none — which made the
    // "guides span every group" assertion below unsatisfiable for a reason
    // that had nothing to do with the code under test.
    constexpr u32 kGuideStride = 25;
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

    // The real selection. Every selected curve must be a guide, the cap must
    // hold, and — since the stride is phased per group on a cooked groom — both
    // groups must be represented among the guides.
    GroomPreviewStats stats = plan;
    std::vector<u32> selected;
    SelectGroomPreviewCurves(*groom, guidesOnly, stats, selected);

    EXPECT_GT(selected.size(), 0u);
    EXPECT_LE(selected.size(), kMaxStrands);
    std::set<u16> groupsTouched;
    for (const u32 c : selected)
    {
        EXPECT_TRUE(groom->IsGuide(c)) << "guides-only selected a non-guide curve " << c;
        groupsTouched.insert(groom->GetCurveGroupIds()[c]);
    }
    EXPECT_EQ(groupsTouched.size(), 2u) << "guides-only must still span every group that has guides";
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

TEST(GroomPreview, EveryRepresentedGroupContributesAtLeastOneStrand)
{
    // The bug: a GLOBAL modulo stride skips a group outright when none of its
    // curve indices happen to be divisible by the stride. A five-curve group
    // sitting after 500 curves of another group is exactly that case, and the
    // result looks perfectly fine on screen — it just silently hides a group,
    // which is the failure the subsampling is supposed to prevent.
    //
    // The even-split fixture used by the test above cannot show this: with
    // equal groups the global stride happens to land in all of them.
    Ref<GroomAsset> groom = MakeGroomWithGroupSizes({ 500u, 5u, 500u });
    ASSERT_TRUE(groom);
    ASSERT_EQ(groom->GetGroupCount(), 3u);

    GroomPreviewSettings settings;
    settings.MaxStrands = 20;       // stride ~51 over 1005 curves
    settings.MaxSegments = 1000000; // let the strand cap bind

    GroomPreviewStats stats = PlanGroomPreview(*groom, settings);
    ASSERT_GT(stats.Stride, 5u) << "the stride must exceed the small group's size or this proves nothing";
    ASSERT_TRUE(stats.GroupPhasedSelection) << "a canonicalised groom must use per-group phasing";

    std::vector<u32> selected;
    SelectGroomPreviewCurves(*groom, settings, stats, selected);

    std::set<u16> groupsTouched;
    for (const u32 c : selected)
    {
        groupsTouched.insert(groom->GetCurveGroupIds()[c]);
    }
    EXPECT_EQ(groupsTouched.size(), 3u)
        << "a represented group was skipped entirely; that hides authored data and looks correct doing it";
}

TEST(GroomPreview, AnUncanonicalisedGroomFallsBackToAGlobalStride)
{
    // Per-group phasing needs the cook's contiguous ranges. On interleaved ids
    // the per-group counter would reset on nearly every curve and select
    // everything, so such a groom must keep the global stride — and say so,
    // rather than quietly drawing 100x what was asked for.
    GroomBuilder builder;
    std::string reason;
    std::vector<u16> ids(2);
    ASSERT_TRUE(builder.AddGroup("a", ids[0], reason)) << reason;
    ASSERT_TRUE(builder.AddGroup("b", ids[1], reason)) << reason;
    for (u32 c = 0; c < 200; ++c)
    {
        const std::vector<glm::vec3> points = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.1f, 0.0f } };
        const std::vector<f32> widths = { 0.001f, 0.001f };
        GroomCurveInput input;
        input.Points = points;
        input.Widths = widths;
        input.GroupId = ids[c % 2]; // interleaved, and deliberately NOT cooked
        ASSERT_TRUE(builder.AddCurve(input, reason)) << reason;
    }
    Ref<GroomAsset> groom = builder.Build(reason);
    ASSERT_TRUE(groom) << reason;

    GroomPreviewSettings settings;
    settings.MaxStrands = 10;
    settings.MaxSegments = 1000000;

    GroomPreviewStats stats = PlanGroomPreview(*groom, settings);
    EXPECT_FALSE(stats.GroupPhasedSelection) << "interleaved group ids must not be treated as contiguous";

    std::vector<u32> selected;
    SelectGroomPreviewCurves(*groom, settings, stats, selected);
    EXPECT_LE(selected.size(), settings.MaxStrands)
        << "the global-stride fallback still has to honour the strand cap";
}

TEST(GroomPreview, TheExactLineBudgetStopsSubmissionAndSaysSo)
{
    // The stride is derived from the AVERAGE strand length, which on a uniform
    // groom is exact — so the guard rightly never fires there, and an earlier
    // version of this test asserted it would.
    //
    // This is the layout the average cannot see: 200 curves where every 10th has
    // 200 control points and the rest have two. The average lines-per-strand
    // lands near 24, which yields a stride of 10 — and every stride-aligned
    // index is one of the LONG curves, at 202 lines each. The estimate says 20
    // curves fit in 500 lines; the reality is 4040. Without the exact check at
    // submission the frame's shared transform buffer overflows.
    Ref<GroomAsset> groom = MakeGroomWithVaryingLengths(/*count*/ 200, /*longEvery*/ 10, /*longPoints*/ 200);
    ASSERT_TRUE(groom);

    GroomPreviewSettings settings;
    settings.MaxStrands = 200; // ask for all of them
    settings.MaxSegments = 500;
    settings.ShowStrands = true;
    settings.ShowRoots = true;

    GroomPreviewStats stats = PlanGroomPreview(*groom, settings);
    ASSERT_EQ(stats.Stride, 10u) << "the fixture depends on the stride aligning with the long curves";

    std::vector<u32> selected;
    SelectGroomPreviewCurves(*groom, settings, stats, selected);

    EXPECT_TRUE(stats.SegmentBudgetExhausted)
        << "the exact budget was not enforced, so the frame's transform buffer would overflow silently";
    EXPECT_LE(stats.SegmentsDrawn, settings.MaxSegments) << "submitted more lines than the cap allows";
    EXPECT_EQ(selected.size(), stats.StrandsDrawn);
    EXPECT_GT(selected.size(), 0u) << "the guard stopped everything, which is not a preview";
    // Every selected curve is one of the long ones, which is what makes the
    // average-based estimate wrong here.
    for (const u32 c : selected)
    {
        EXPECT_EQ(groom->GetCurvePointCount(c), 200u);
    }
}

TEST(GroomPreview, TheReportedCountsMatchTheSelection)
{
    // StrandsDrawn / SegmentsDrawn are what the editor shows. They have to be
    // the selection's own numbers, not an estimate alongside it.
    Ref<GroomAsset> groom = MakeGroomWithGroupSizes({ 60u, 40u });
    ASSERT_TRUE(groom);

    GroomPreviewSettings settings;
    settings.MaxStrands = 25;
    settings.MaxSegments = 1000000;

    GroomPreviewStats stats = PlanGroomPreview(*groom, settings);
    std::vector<u32> selected;
    SelectGroomPreviewCurves(*groom, settings, stats, selected);

    EXPECT_EQ(stats.StrandsDrawn, selected.size());
    u32 expectedLines = 0;
    for (const u32 c : selected)
    {
        expectedLines += (groom->GetCurvePointCount(c) - 1u) + 3u;
    }
    EXPECT_EQ(stats.SegmentsDrawn, expectedLines);
    EXPECT_FALSE(stats.SegmentBudgetExhausted);
}
