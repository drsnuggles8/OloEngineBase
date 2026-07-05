// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/OverdrawHeatmap.h"

#include <glm/glm.hpp>

// =============================================================================
// Overdraw heatmap — CPU contract tests (issue #519).
//
// These pin the count->colour ramp implemented in
// PostProcess_OverdrawHeatmap.glsl WITHOUT a GL context (so they run in headless
// CI), mirroring CASMathTest / ContactShadowMathTest. The rendered frame is
// checked separately by the GPU OverdrawVisualEvidenceTest. Per the CLAUDE.md
// rendering rule, math/contract tests prove the formula; the visual test proves
// the frame looks right.
//
// The C++ reference under test (OverdrawHeatmap::HeatColor) is a line-for-line
// mirror of the GLSL HeatColor(). If you change one, change both.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    constexpr f32 kMax = OverdrawHeatmap::kOverdrawHeatmapMaxLayers;
    constexpr f32 kEps = 1e-4f;
} // namespace

// Zero fragments -> pure black (nothing was drawn at this pixel).
TEST(OverdrawHeatmapMathTest, ZeroCountIsBlack)
{
    const glm::vec3 c = OverdrawHeatmap::HeatColor(0.0f);
    EXPECT_NEAR(c.r, 0.0f, kEps);
    EXPECT_NEAR(c.g, 0.0f, kEps);
    EXPECT_NEAR(c.b, 0.0f, kEps);
}

// Saturating count (at / above the max-layers cap) -> pure red (hottest).
TEST(OverdrawHeatmapMathTest, SaturatedCountIsRed)
{
    const glm::vec3 atMax = OverdrawHeatmap::HeatColor(kMax);
    EXPECT_NEAR(atMax.r, 1.0f, kEps);
    EXPECT_NEAR(atMax.g, 0.0f, kEps);
    EXPECT_NEAR(atMax.b, 0.0f, kEps);

    // Beyond the cap clamps to the same red — never wraps or overshoots.
    const glm::vec3 beyond = OverdrawHeatmap::HeatColor(kMax * 4.0f);
    EXPECT_NEAR(beyond.r, 1.0f, kEps);
    EXPECT_NEAR(beyond.g, 0.0f, kEps);
    EXPECT_NEAR(beyond.b, 0.0f, kEps);
}

// The red channel is monotonically non-decreasing in the layer count: a pixel
// with more overlapping layers always reads at least as red as one with fewer.
// This is the property the visual test relies on ("stacked region reads hotter").
TEST(OverdrawHeatmapMathTest, RednessIsMonotonicInCount)
{
    f32 prevRed = -1.0f;
    for (f32 count = 0.0f; count <= kMax * 1.5f; count += kMax / 64.0f)
    {
        const f32 red = OverdrawHeatmap::HeatColor(count).r;
        EXPECT_GE(red, prevRed - kEps)
            << "Red channel decreased as overdraw count rose (count=" << count << ")";
        prevRed = red;
    }
}

// A clearly heavier overlap must read strictly hotter (redder) than a light one —
// the core promise of the heatmap. 1 layer is deep in the cool half; a stack near
// the cap is in the hot half.
TEST(OverdrawHeatmapMathTest, MoreLayersReadHotter)
{
    const glm::vec3 oneLayer = OverdrawHeatmap::HeatColor(1.0f);
    const glm::vec3 heavy = OverdrawHeatmap::HeatColor(kMax * 0.9f);
    EXPECT_GT(heavy.r, oneLayer.r + 0.25f)
        << "A near-saturated stack is not meaningfully redder than a single layer";
}

// The ramp walks the documented stops (black -> blue -> green -> yellow -> red)
// at the quarter marks, so the mid-range is legibly coloured (not a grey wash).
TEST(OverdrawHeatmapMathTest, HitsExpectedStopColors)
{
    // t = 0.25 -> blue
    const glm::vec3 blue = OverdrawHeatmap::HeatColor(0.25f * kMax);
    EXPECT_NEAR(blue.r, 0.0f, kEps);
    EXPECT_NEAR(blue.g, 0.0f, kEps);
    EXPECT_NEAR(blue.b, 1.0f, kEps);

    // t = 0.5 -> green
    const glm::vec3 green = OverdrawHeatmap::HeatColor(0.5f * kMax);
    EXPECT_NEAR(green.r, 0.0f, kEps);
    EXPECT_NEAR(green.g, 1.0f, kEps);
    EXPECT_NEAR(green.b, 0.0f, kEps);

    // t = 0.75 -> yellow
    const glm::vec3 yellow = OverdrawHeatmap::HeatColor(0.75f * kMax);
    EXPECT_NEAR(yellow.r, 1.0f, kEps);
    EXPECT_NEAR(yellow.g, 1.0f, kEps);
    EXPECT_NEAR(yellow.b, 0.0f, kEps);
}

// A custom max-layers scales the ramp: doubling maxLayers halves the effective t
// for a fixed count, so the colour cools. Also guards the degenerate maxLayers.
TEST(OverdrawHeatmapMathTest, MaxLayersScalesTheRamp)
{
    // At count == maxLayers the ramp saturates regardless of the max value.
    EXPECT_NEAR(OverdrawHeatmap::HeatColor(4.0f, 4.0f).r, 1.0f, kEps);
    EXPECT_NEAR(OverdrawHeatmap::HeatColor(20.0f, 20.0f).r, 1.0f, kEps);

    // A degenerate (<= 0) max never divides by zero and saturates immediately.
    const glm::vec3 degenerate = OverdrawHeatmap::HeatColor(1.0f, 0.0f);
    EXPECT_TRUE(std::isfinite(degenerate.r));
    EXPECT_NEAR(degenerate.r, 1.0f, kEps);
}
