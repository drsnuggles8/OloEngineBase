// OLO_TEST_LAYER: cullinglod
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "VirtualRasterCoverageMirror.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <utility>
#include <vector>

// =============================================================================
// Sub-sample-miss reject for the virtual-geometry software raster — CPU contract
// tests (issue #712).
//
// The rule under test ships in
// `OloEditor/assets/shaders/include/VirtualRasterCoverage.glsl`
// (`OloVirtualSampleRange`) and is used by
// `compute/VirtualClusterRaster.comp`. Rasterization decides coverage at PIXEL
// CENTRES, so the pixels worth visiting are only those whose centre falls inside
// the triangle's window-space bounding box:
//
//     x + 0.5 >= bbMin.x   <=>   x >= ceil (bbMin.x - 0.5)
//     x + 0.5 <= bbMax.x   <=>   x <= floor(bbMax.x - 0.5)
//
// An empty range in either axis means the triangle falls entirely between sample
// points and can be rejected outright; a non-empty one is still tighter than the
// floor/ceil box the raster used to walk.
//
// WHAT THESE TESTS ARE FOR. The reject's failure mode is silent: half a pixel
// too aggressive and thin or sliver triangles vanish from the visibility buffer,
// which no other CPU test notices and a screenshot shows only as slightly wrong
// edges. So the contract pinned here is not "the formula is this" — it is the
// strictly stronger *equivalence*:
//
//   for every triangle, the set of pixels the raster's edge test accepts is
//   IDENTICAL whether it scans the old floor/ceil box or the new sample range,
//
// checked against a brute-force scan of the old box. The rule's CPU mirror is
// VirtualRasterCoverageMirror.h; `ReferenceCoveredPixels` below mirrors
// RasterizeTriangle's edge test. The shipped GLSL itself is exercised by the L2
// probe (ShaderUnitVirtualSampleBoundsTest, which needs a GL context and
// therefore skips in CI), and the whole visibility buffer by the GPU A/B
// recorded on the PR.
//
// The mirrors are deliberate duplication in the same spirit as
// OverdrawHeatmapMathTest / PostProcess_OverdrawHeatmap.glsl: there is no CPU
// rasterizer in the engine for this rule to be shared with, and a mirror that
// the equivalence property constrains this tightly cannot drift far in silence.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    namespace Mirror = OloEngine::Tests::VirtualRasterCoverage;
    using Mirror::SampleRange;
    using Mirror::ToIvec2;

    // Shorthand for the mirror under test.
    SampleRange SampleRangeMirror(glm::vec2 bbMin, glm::vec2 bbMax, glm::vec2 viewport)
    {
        return Mirror::SampleRangeFromBox(bbMin, bbMax, viewport);
    }

    struct Triangle
    {
        glm::vec2 S0{ 0.0f };
        glm::vec2 S1{ 0.0f };
        glm::vec2 S2{ 0.0f };
        bool TwoSided{ false };
    };

    // Mirror of RasterizeTriangle's backface/degenerate guards. `orient` folds
    // the winding in so the edge test stays valid for a two-sided back face.
    // NOT named `Setup`: every TEST body inherits testing::Test's private
    // `Setup()` typo guard, which hides a type of that name.
    struct TriangleGuards
    {
        bool Rasterizes{ false };
        f32 Orient{ 1.0f };
    };

    TriangleGuards ApplyGuards(const Triangle& tri)
    {
        TriangleGuards setup;
        const f32 signedArea = Mirror::SignedArea2(tri.S0, tri.S1, tri.S2);
        if (signedArea < 0.0f && !tri.TwoSided)
            return setup; // back-facing, single-sided material
        setup.Orient = (signedArea < 0.0f) ? -1.0f : 1.0f;
        const f32 area = signedArea * setup.Orient;
        if (!(area > 0.0f))
            return setup; // zero-area / NaN degenerate
        setup.Rasterizes = true;
        return setup;
    }

    // Mirror of the raster's inner-loop edge test at one pixel centre.
    bool PixelCentreIsCovered(const Triangle& tri, f32 orient, i32 x, i32 y)
    {
        const glm::vec2 pixel(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f);
        const f32 w0 = orient * ((tri.S1.x - pixel.x) * (tri.S2.y - pixel.y) -
                                 (tri.S2.x - pixel.x) * (tri.S1.y - pixel.y));
        const f32 w1 = orient * ((tri.S2.x - pixel.x) * (tri.S0.y - pixel.y) -
                                 (tri.S0.x - pixel.x) * (tri.S2.y - pixel.y));
        const f32 w2 = orient * ((tri.S0.x - pixel.x) * (tri.S1.y - pixel.y) -
                                 (tri.S1.x - pixel.x) * (tri.S0.y - pixel.y));
        return !(w0 < 0.0f || w1 < 0.0f || w2 < 0.0f);
    }

    // The PRE-CHANGE loop bounds: floor/ceil of the box, clamped to the
    // viewport. Kept verbatim because it is the reference the equivalence is
    // measured against — and because the raster still evaluates its
    // kMaxTriangleBBox work cap on exactly this box, so that the set of
    // triangles the cap rejects did not move when the sample range arrived.
    struct PixelBox
    {
        glm::ivec2 Min{ 0, 0 };
        glm::ivec2 Max{ 0, 0 };
        bool NonEmpty{ false };
    };

    PixelBox PixelAlignedBox(glm::vec2 bbMin, glm::vec2 bbMax, glm::vec2 viewport)
    {
        PixelBox box;
        box.Min = ToIvec2(glm::max(glm::floor(bbMin), glm::vec2(0.0f)));
        box.Max = ToIvec2(glm::min(glm::ceil(bbMax), viewport - 1.0f));
        box.NonEmpty = box.Max.x >= box.Min.x && box.Max.y >= box.Min.y;
        return box;
    }

    struct Coverage
    {
        u32 Total{ 0 };        // pixels the pre-change scan accepted
        u32 OutsideRange{ 0 }; // ... that the sample range would not have visited
    };

    // Brute-force scan of the PRE-CHANGE box, classifying each accepted pixel by
    // whether the new sample range covers it. Since the range is separately
    // asserted to sit inside the box, `OutsideRange == 0` is exactly "the two
    // scans accept the same set" — proved without materialising either set,
    // which keeps a 40k-triangle corpus fast in a Debug build.
    Coverage ScanCoverage(const Triangle& tri, f32 orient, const PixelBox& box, const SampleRange& range)
    {
        Coverage coverage;
        if (!box.NonEmpty)
            return coverage;
        for (i32 y = box.Min.y; y <= box.Max.y; ++y)
        {
            for (i32 x = box.Min.x; x <= box.Max.x; ++x)
            {
                if (!PixelCentreIsCovered(tri, orient, x, y))
                    continue;
                ++coverage.Total;
                const bool inRange = range.Covers && x >= range.Min.x && x <= range.Max.x &&
                                     y >= range.Min.y && y <= range.Max.y;
                if (!inRange)
                    ++coverage.OutsideRange;
            }
        }
        return coverage;
    }

    struct CorpusStats
    {
        u32 Rasterized{ 0 };   // survived the backface / degenerate guards
        u32 Rejected{ 0 };     // the sub-sample-miss reject fired
        u32 Tightened{ 0 };    // survived, but with a smaller box than before
        u64 PixelsBefore{ 0 }; // inner-loop iterations, old bounds
        u64 PixelsAfter{ 0 };  // inner-loop iterations, new bounds
    };

    // The one assertion body every corpus runs: the covered set is identical
    // either way, the new range never reaches outside the old box, and a reject
    // never discards coverage.
    void ExpectEquivalent(const Triangle& tri, glm::vec2 viewport, CorpusStats& stats,
                          const char* what)
    {
        const TriangleGuards setup = ApplyGuards(tri);
        if (!setup.Rasterizes)
            return;
        ++stats.Rasterized;

        const glm::vec2 bbMin = glm::min(tri.S0, glm::min(tri.S1, tri.S2));
        const glm::vec2 bbMax = glm::max(tri.S0, glm::max(tri.S1, tri.S2));

        const PixelBox box = PixelAlignedBox(bbMin, bbMax, viewport);
        const SampleRange range = SampleRangeMirror(bbMin, bbMax, viewport);

        const auto describe = [&]()
        {
            return ::testing::Message()
                   << what << " tri (" << tri.S0.x << ", " << tri.S0.y << ") ("
                   << tri.S1.x << ", " << tri.S1.y << ") (" << tri.S2.x << ", " << tri.S2.y
                   << ") viewport " << viewport.x << "x" << viewport.y;
        };

        // The equivalence itself: no pixel the old scan accepted falls outside
        // the new bounds.
        const Coverage coverage = ScanCoverage(tri, setup.Orient, box, range);
        EXPECT_EQ(coverage.OutsideRange, 0u)
            << "the sample range would skip " << coverage.OutsideRange << " of " << coverage.Total
            << " covered pixel(s); " << describe();

        if (box.NonEmpty)
        {
            const u64 before = static_cast<u64>(box.Max.x - box.Min.x + 1) *
                               static_cast<u64>(box.Max.y - box.Min.y + 1);
            stats.PixelsBefore += before;
        }

        if (!range.Covers)
        {
            ++stats.Rejected;
            // The load-bearing half: a reject must never drop coverage.
            EXPECT_EQ(coverage.Total, 0u) << "sub-sample reject dropped " << coverage.Total
                                          << " covered pixel(s); " << describe();
            return;
        }

        const u64 after = static_cast<u64>(range.Max.x - range.Min.x + 1) *
                          static_cast<u64>(range.Max.y - range.Min.y + 1);
        stats.PixelsAfter += after;

        if (box.NonEmpty)
        {
            // Never looser than what it replaced — the change may only remove
            // inner-loop iterations, never add them.
            EXPECT_GE(range.Min.x, box.Min.x) << describe();
            EXPECT_GE(range.Min.y, box.Min.y) << describe();
            EXPECT_LE(range.Max.x, box.Max.x) << describe();
            EXPECT_LE(range.Max.y, box.Max.y) << describe();
            const u64 before = static_cast<u64>(box.Max.x - box.Min.x + 1) *
                               static_cast<u64>(box.Max.y - box.Min.y + 1);
            if (after < before)
                ++stats.Tightened;
        }
    }
} // namespace

// A triangle wholly inside one pixel but missing its centre covers nothing, and
// the reject fires. This is the case the issue is about: at micropoly density
// the raster used to run three edge functions over a 2x2 or 3x3 box for it.
TEST(VirtualRasterCoverageTest, TriangleBetweenSamplePointsIsRejected)
{
    const glm::vec2 viewport(64.0f, 64.0f);
    // Fits in the top-left quarter of pixel (10, 10): all of it is below and
    // left of the centre at (10.5, 10.5).
    const Triangle tri{ { 10.05f, 10.05f }, { 10.40f, 10.05f }, { 10.05f, 10.40f } };

    const glm::vec2 bbMin = glm::min(tri.S0, glm::min(tri.S1, tri.S2));
    const glm::vec2 bbMax = glm::max(tri.S0, glm::max(tri.S1, tri.S2));
    const SampleRange range = SampleRangeMirror(bbMin, bbMax, viewport);
    EXPECT_FALSE(range.Covers);

    // ... and the pre-change scan agrees it covers nothing, so the reject is
    // free rather than a behaviour change.
    const TriangleGuards setup = ApplyGuards(tri);
    ASSERT_TRUE(setup.Rasterizes);
    const PixelBox box = PixelAlignedBox(bbMin, bbMax, viewport);
    EXPECT_TRUE(box.NonEmpty); // the old code DID walk it
    EXPECT_EQ(ScanCoverage(tri, setup.Orient, box, range).Total, 0u);
}

// A pixel centre lying exactly on the bounding-box boundary is a covered sample:
// the edge test accepts a zero edge function, so both range bounds are
// inclusive. Half a pixel of over-eagerness here is exactly the silent failure
// this reject risks.
TEST(VirtualRasterCoverageTest, BoxBoundaryExactlyOnAPixelCentreStaysInRange)
{
    const glm::vec2 viewport(64.0f, 64.0f);
    // Vertices on pixel centres: the box is [4.5, 8.5] in both axes, so pixels
    // 4..8 inclusive are in range at both ends.
    const SampleRange range = SampleRangeMirror({ 4.5f, 4.5f }, { 8.5f, 8.5f }, viewport);
    ASSERT_TRUE(range.Covers);
    EXPECT_EQ(range.Min, glm::ivec2(4, 4));
    EXPECT_EQ(range.Max, glm::ivec2(8, 8));

    // A hair inside on each side drops exactly the two boundary pixels.
    const SampleRange inside = SampleRangeMirror({ 4.5001f, 4.5001f }, { 8.4999f, 8.4999f }, viewport);
    ASSERT_TRUE(inside.Covers);
    EXPECT_EQ(inside.Min, glm::ivec2(5, 5));
    EXPECT_EQ(inside.Max, glm::ivec2(7, 7));
}

// A box that spans less than a pixel still covers a sample when a centre falls
// inside it — the reject must be about sample points, not about size.
TEST(VirtualRasterCoverageTest, SubPixelBoxStraddlingACentreIsKept)
{
    const glm::vec2 viewport(64.0f, 64.0f);
    const SampleRange range = SampleRangeMirror({ 10.49f, 10.49f }, { 10.51f, 10.51f }, viewport);
    ASSERT_TRUE(range.Covers);
    EXPECT_EQ(range.Min, glm::ivec2(10, 10));
    EXPECT_EQ(range.Max, glm::ivec2(10, 10));
}

// Off-screen boxes reject outright instead of scanning a clamped edge pixel, and
// the clamps keep both bounds inside the int range for coordinates that a
// grazing projection blew up.
TEST(VirtualRasterCoverageTest, OffScreenAndOutOfRangeBoxesReject)
{
    const glm::vec2 viewport(1920.0f, 1080.0f);

    EXPECT_FALSE(SampleRangeMirror({ -50.0f, 10.0f }, { -10.0f, 20.0f }, viewport).Covers);
    EXPECT_FALSE(SampleRangeMirror({ 2000.0f, 10.0f }, { 2100.0f, 20.0f }, viewport).Covers);
    EXPECT_FALSE(SampleRangeMirror({ 10.0f, -50.0f }, { 20.0f, -10.0f }, viewport).Covers);
    EXPECT_FALSE(SampleRangeMirror({ 10.0f, 2000.0f }, { 20.0f, 2100.0f }, viewport).Covers);

    // Beyond int range in both directions, and infinite.
    const f32 huge = 1.0e30f;
    const f32 inf = std::numeric_limits<f32>::infinity();
    for (const auto& [lo, hi] : std::vector<std::pair<glm::vec2, glm::vec2>>{
             { { -huge, -huge }, { -huge * 0.5f, -huge * 0.5f } },
             { { huge, huge }, { huge * 2.0f, huge * 2.0f } },
             { { -inf, -inf }, { inf, inf } },
             { { -huge, -huge }, { huge, huge } } })
    {
        const SampleRange range = SampleRangeMirror(lo, hi, viewport);
        EXPECT_GE(range.Min.x, 0);
        EXPECT_LE(range.Min.x, static_cast<i32>(viewport.x));
        EXPECT_GE(range.Max.x, -1);
        EXPECT_LE(range.Max.x, static_cast<i32>(viewport.x) - 1);
        EXPECT_GE(range.Min.y, 0);
        EXPECT_LE(range.Min.y, static_cast<i32>(viewport.y));
        EXPECT_GE(range.Max.y, -1);
        EXPECT_LE(range.Max.y, static_cast<i32>(viewport.y) - 1);
    }

    // A whole-screen box is kept and spans the whole screen.
    const SampleRange full = SampleRangeMirror({ -inf, -inf }, { inf, inf }, viewport);
    ASSERT_TRUE(full.Covers);
    EXPECT_EQ(full.Min, glm::ivec2(0, 0));
    EXPECT_EQ(full.Max, glm::ivec2(1919, 1079));
}

// The equivalence, over a corpus dense in the interesting cases: micro-triangles
// at every subpixel offset, where a half-pixel error in the rule would drop real
// coverage.
TEST(VirtualRasterCoverageTest, MicroTrianglesCoverTheSamePixelsAsTheOldBounds)
{
    const glm::vec2 viewport(32.0f, 32.0f);
    CorpusStats stats;

    std::mt19937 rng(0x712u); // fixed seed: the corpus is a fixture, not a fuzz run
    std::uniform_real_distribution<f32> origin(-2.0f, 33.0f);
    std::uniform_real_distribution<f32> extent(-2.5f, 2.5f);
    std::bernoulli_distribution twoSided(0.25);

    for (u32 i = 0; i < 20000u; ++i)
    {
        const glm::vec2 base(origin(rng), origin(rng));
        Triangle tri;
        tri.S0 = base;
        tri.S1 = base + glm::vec2(extent(rng), extent(rng));
        tri.S2 = base + glm::vec2(extent(rng), extent(rng));
        tri.TwoSided = twoSided(rng);
        ExpectEquivalent(tri, viewport, stats, "random micro");
    }

    // Sweep a fixed sliver across a whole pixel in 1/64ths, which walks it
    // through every relationship with the sample grid — including landing
    // exactly on centres.
    for (u32 sy = 0; sy < 64u; ++sy)
    {
        for (u32 sx = 0; sx < 64u; ++sx)
        {
            const glm::vec2 base(8.0f + static_cast<f32>(sx) / 64.0f,
                                 8.0f + static_cast<f32>(sy) / 64.0f);
            ExpectEquivalent({ base, base + glm::vec2(0.9f, 0.05f), base + glm::vec2(0.1f, 0.6f) },
                             viewport, stats, "sliver sweep");
            ExpectEquivalent({ base, base + glm::vec2(1.7f, 0.0f), base + glm::vec2(0.0f, 1.7f) },
                             viewport, stats, "unit sweep");
        }
    }

    // Anti-vacuous: the corpus must actually exercise both outcomes, or the
    // equivalence above is a statement about an empty set.
    GTEST_LOG_(INFO) << "rasterized " << stats.Rasterized << ", sub-sample rejected "
                     << stats.Rejected << ", tightened " << stats.Tightened
                     << ", inner-loop pixels " << stats.PixelsBefore << " -> "
                     << stats.PixelsAfter;
    EXPECT_GT(stats.Rasterized, 5000u);
    EXPECT_GT(stats.Rejected, 100u) << "no triangle in the corpus missed every sample point";
    EXPECT_GT(stats.Tightened, 1000u) << "the range never tightened — the bounds are not new";
    EXPECT_LT(stats.PixelsAfter, stats.PixelsBefore);
}

// The same equivalence for triangles big enough to have interior pixels, and for
// triangles that hang off every edge of the viewport — where the two clamping
// schemes differ and an off-by-one would show up as a missing screen-edge row.
TEST(VirtualRasterCoverageTest, LargeAndClippedTrianglesCoverTheSamePixels)
{
    const glm::vec2 viewport(37.0f, 23.0f); // deliberately not a round size
    CorpusStats stats;

    std::mt19937 rng(0x629u);
    std::uniform_real_distribution<f32> coord(-20.0f, 55.0f);
    std::bernoulli_distribution twoSided(0.5);

    for (u32 i = 0; i < 6000u; ++i)
    {
        Triangle tri;
        tri.S0 = { coord(rng), coord(rng) };
        tri.S1 = { coord(rng), coord(rng) };
        tri.S2 = { coord(rng), coord(rng) };
        tri.TwoSided = twoSided(rng);
        ExpectEquivalent(tri, viewport, stats, "random large");
    }

    // Axis-aligned triangles pinned to the screen edges, at half-pixel offsets:
    // the clamp boundaries themselves.
    for (i32 step = -2; step <= 2; ++step)
    {
        const f32 d = static_cast<f32>(step) * 0.5f;
        ExpectEquivalent({ { d, d }, { 12.0f + d, d }, { d, 9.0f + d } }, viewport, stats, "edge min");
        ExpectEquivalent({ { viewport.x + d, viewport.y + d },
                           { viewport.x - 12.0f + d, viewport.y + d },
                           { viewport.x + d, viewport.y - 9.0f + d } },
                         viewport, stats, "edge max");
    }

    GTEST_LOG_(INFO) << "rasterized " << stats.Rasterized << ", sub-sample rejected "
                     << stats.Rejected << ", tightened " << stats.Tightened
                     << ", inner-loop pixels " << stats.PixelsBefore << " -> "
                     << stats.PixelsAfter;
    EXPECT_GT(stats.Rasterized, 2000u);
    EXPECT_GT(stats.Tightened, 1000u);
    EXPECT_LT(stats.PixelsAfter, stats.PixelsBefore);
}
