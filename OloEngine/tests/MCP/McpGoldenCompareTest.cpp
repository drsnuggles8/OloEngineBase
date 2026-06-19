#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure image-comparison core behind olo_render_compare_golden
// (issue #316 Part 4). The math lives in a header-only free function with no GL /
// renderer / editor / stb dependencies precisely so it can be exercised here
// without a live editor or GPU — the test binary compiles the MCP diff core but
// deliberately NOT McpTools.cpp (the editor-backed handler). The live tool is
// verified separately over the MCP attach loop; this pins the numeric verdict
// that maps two RGBA8 buffers -> similarity + pass/fail.
//
// The metrics mirror OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp
// so the MCP verdict agrees with the OLOENGINE_GOLDEN_REBASE test-suite goldens.
#include "MCP/McpGoldenCompare.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace
{
    using OloEngine::MCP::GoldenCompare::Compare;
    using OloEngine::MCP::GoldenCompare::CompareResult;
    using OloEngine::MCP::GoldenCompare::kRmseFailAbove;
    using OloEngine::MCP::GoldenCompare::kSsimPassThreshold;

    // u8/u32/f32 are global typedefs from Core/Base.h (pulled in via the PCH).

    // A solid-colour RGBA8 image (deterministic, no GPU). 4 bytes/pixel.
    std::vector<u8> MakeSolid(u32 width, u32 height, u8 r, u8 g, u8 b, u8 a = 255)
    {
        std::vector<u8> out(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i + 3 < out.size(); i += 4)
        {
            out[i + 0] = r;
            out[i + 1] = g;
            out[i + 2] = b;
            out[i + 3] = a;
        }
        return out;
    }

    // A deterministic checkerboard (structure for SSIM to chew on).
    std::vector<u8> MakeCheckerboard(u32 width, u32 height, u8 lo, u8 hi, u32 cell)
    {
        std::vector<u8> out(static_cast<std::size_t>(width) * height * 4, 255);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                const bool light = ((x / cell) + (y / cell)) % 2 == 0;
                const u8 v = light ? hi : lo;
                const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4;
                out[idx + 0] = v;
                out[idx + 1] = v;
                out[idx + 2] = v;
                out[idx + 3] = 255;
            }
        }
        return out;
    }
} // namespace

TEST(McpGoldenCompare, IdenticalImagesAreAPerfectMatch)
{
    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const auto img = MakeCheckerboard(kW, kH, 30, 220, 8);
    const CompareResult r = Compare(img, kW, kH, img, kW, kH, std::nullopt);

    EXPECT_TRUE(r.DimensionsMatch);
    EXPECT_TRUE(r.Pass);
    EXPECT_NEAR(r.Similarity, 1.0f, 1e-5f);
    EXPECT_NEAR(r.Ssim, 1.0f, 1e-5f);
    EXPECT_NEAR(r.Rmse, 0.0f, 1e-6f);
    EXPECT_NEAR(r.Mse, 0.0f, 1e-6f);
    EXPECT_EQ(0u, r.MismatchPixels);
    EXPECT_EQ(0u, r.MaxChannelDelta);
    EXPECT_EQ(kW * kH, r.TotalPixels);
    // Bit-identical short-circuit message.
    EXPECT_NE(std::string::npos, r.Message.find("Bit-identical"));
}

TEST(McpGoldenCompare, DimensionMismatchShortCircuits)
{
    const auto a = MakeSolid(64, 64, 10, 20, 30);
    const auto b = MakeSolid(32, 48, 10, 20, 30);
    const CompareResult r = Compare(a, 64, 64, b, 32, 48, std::nullopt);

    EXPECT_FALSE(r.DimensionsMatch);
    EXPECT_FALSE(r.Pass);
    EXPECT_EQ(64u, r.ActualWidth);
    EXPECT_EQ(48u, r.GoldenHeight);
    // Metrics are left undefined (not a meaningless number) on a size mismatch.
    EXPECT_NE(std::string::npos, r.Message.find("dimension mismatch"));
}

TEST(McpGoldenCompare, MalformedBufferIsRejected)
{
    // Dimensions agree but a buffer is the wrong size (decode bug) — must not
    // index out of bounds; report the mismatch and fail.
    const auto good = MakeSolid(16, 16, 0, 0, 0);
    std::vector<u8> truncated(good.begin(), good.begin() + 100);
    const CompareResult r = Compare(good, 16, 16, truncated, 16, 16, std::nullopt);

    EXPECT_TRUE(r.DimensionsMatch); // dims matched...
    EXPECT_FALSE(r.Pass);           // ...but the buffer was malformed
    EXPECT_NE(std::string::npos, r.Message.find("buffer size mismatch"));
}

TEST(McpGoldenCompare, TinyUniformShiftStillPasses)
{
    // A 2-LSB uniform brightness bump: RMSE is tiny (well under the fast-pass
    // bound) and SSIM stays ~1 — the suite considers this a pass.
    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const auto golden = MakeCheckerboard(kW, kH, 40, 200, 8);
    auto actual = golden;
    for (std::size_t i = 0; i + 3 < actual.size(); i += 4)
    {
        actual[i + 0] = static_cast<u8>(std::min<u32>(actual[i + 0] + 2u, 255u));
        actual[i + 1] = static_cast<u8>(std::min<u32>(actual[i + 1] + 2u, 255u));
        actual[i + 2] = static_cast<u8>(std::min<u32>(actual[i + 2] + 2u, 255u));
    }
    const CompareResult r = Compare(actual, kW, kH, golden, kW, kH, std::nullopt);

    EXPECT_TRUE(r.Pass);
    EXPECT_LT(r.Rmse, 0.01f);
    EXPECT_GT(r.Similarity, 0.99f);
    // Every pixel shifted by 2 LSB — below the 4-LSB mismatch epsilon, so none
    // count as "differing".
    EXPECT_EQ(0u, r.MismatchPixels);
    EXPECT_EQ(2u, r.MaxChannelDelta);
}

TEST(McpGoldenCompare, LargeKnownDeltaFails)
{
    // A 60-LSB uniform shift drives RMSE past the hard-fail bound regardless of
    // SSIM — the suite-cascade verdict must fail.
    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const auto golden = MakeSolid(kW, kH, 100, 100, 100);
    const auto actual = MakeSolid(kW, kH, 160, 160, 160);
    const CompareResult r = Compare(actual, kW, kH, golden, kW, kH, std::nullopt);

    EXPECT_FALSE(r.Pass);
    EXPECT_GE(r.Rmse, kRmseFailAbove);
    // 60/255 ≈ 0.235 expected RMSE for a flat 60-LSB shift on all RGB channels.
    EXPECT_NEAR(r.Rmse, 60.0f / 255.0f, 1e-3f);
    EXPECT_EQ(60u, r.MaxChannelDelta);
    EXPECT_EQ(kW * kH, r.MismatchPixels); // every pixel differs by > 4 LSB
}

TEST(McpGoldenCompare, ExplicitThresholdGatesOnSimilarity)
{
    // Two structurally different images: a checkerboard vs solid grey. SSIM is
    // low. An explicit threshold above the SSIM value fails; below it passes.
    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const auto golden = MakeCheckerboard(kW, kH, 0, 255, 8);
    const auto actual = MakeSolid(kW, kH, 128, 128, 128);

    const CompareResult strict = Compare(actual, kW, kH, golden, kW, kH, std::optional<f32>(0.95f));
    EXPECT_EQ("explicit", strict.ThresholdMode);
    EXPECT_FALSE(strict.Pass);
    EXPECT_FLOAT_EQ(0.95f, strict.Threshold);

    const CompareResult lax = Compare(actual, kW, kH, golden, kW, kH, std::optional<f32>(0.0f));
    EXPECT_EQ("explicit", lax.ThresholdMode);
    EXPECT_TRUE(lax.Pass); // any similarity >= 0
}

TEST(McpGoldenCompare, ExplicitThresholdBoundaryIsInclusive)
{
    // Pass is `Similarity >= Threshold`. Setting the threshold exactly at the
    // measured similarity of an identical pair (1.0) must still pass.
    constexpr u32 kW = 32;
    constexpr u32 kH = 32;
    const auto img = MakeCheckerboard(kW, kH, 10, 240, 4);
    const CompareResult r = Compare(img, kW, kH, img, kW, kH, std::optional<f32>(1.0f));
    EXPECT_TRUE(r.Pass);
    EXPECT_FLOAT_EQ(1.0f, r.Threshold);
}

TEST(McpGoldenCompare, ThresholdModeReportedWithoutExplicitThreshold)
{
    constexpr u32 kW = 32;
    constexpr u32 kH = 32;
    const auto img = MakeSolid(kW, kH, 50, 60, 70);
    const CompareResult r = Compare(img, kW, kH, img, kW, kH, std::nullopt);
    EXPECT_EQ("suite-cascade", r.ThresholdMode);
    EXPECT_FLOAT_EQ(kSsimPassThreshold, r.Threshold);
}

TEST(McpGoldenCompare, WorstPixelLocationIsReported)
{
    // Plant a single divergent pixel and confirm the worst-pixel locator finds it.
    constexpr u32 kW = 16;
    constexpr u32 kH = 16;
    const auto golden = MakeSolid(kW, kH, 0, 0, 0);
    auto actual = golden;
    constexpr u32 kPx = 11;
    constexpr u32 kPy = 7;
    const std::size_t idx = (static_cast<std::size_t>(kPy) * kW + kPx) * 4;
    actual[idx + 1] = 200; // green spike at (11, 7)

    const CompareResult r = Compare(actual, kW, kH, golden, kW, kH, std::nullopt);
    EXPECT_EQ(200u, r.MaxChannelDelta);
    EXPECT_EQ(kPx, r.WorstX);
    EXPECT_EQ(kPy, r.WorstY);
    EXPECT_EQ(1u, r.MismatchPixels);
}
