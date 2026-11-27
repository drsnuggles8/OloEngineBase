// OLO_TEST_LAYER: unit
// =============================================================================
// The device-free half of the lifecycle soak (issue #1796, LifecycleSoak.h):
// the frame comparison, the control-floor judgement, the growth verdict and the
// control-frame file. Each case pins one decision the device soak takes on its
// word, so a change to a threshold or a guard shows up here before it changes
// what the soak can catch.
// =============================================================================

#include "OloEnginePCH.h"

#include "LifecycleSoak.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

namespace OloEngine::Tests::Lifecycle
{
    namespace
    {
        [[nodiscard]] Frame Solid(u32 w, u32 h, u8 value)
        {
            Frame frame;
            frame.Width = w;
            frame.Height = h;
            frame.Rgba.assign(static_cast<sizet>(w) * h * 4u, value);
            return frame;
        }
    } // namespace

    TEST(LifecycleSoakStats, IdenticalFramesHaveNoDifference)
    {
        const Frame a = Solid(64, 32, 100);
        const FrameDiff diff = CompareFrames(a, a);
        EXPECT_FALSE(diff.ShapeMismatch);
        EXPECT_EQ(diff.MaxDelta, 0u);
        EXPECT_DOUBLE_EQ(diff.MaxTileShift, 0.0);
        EXPECT_DOUBLE_EQ(diff.DifferingFraction, 0.0);
    }

    // One 16x16 tile brightened by 40: its tile mean moves by exactly 40, and
    // 256 of 2048 pixels (12.5 %) differ by more than the threshold.
    TEST(LifecycleSoakStats, ARegionThatChangedMovesItsTileMean)
    {
        const Frame a = Solid(64, 32, 100);
        Frame b = a;
        for (u32 y = 0; y < 16u; ++y)
        {
            for (u32 x = 16; x < 32u; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * 64u + x) * 4u;
                b.Rgba[i] = 140;
            }
        }
        const FrameDiff diff = CompareFrames(a, b);
        EXPECT_DOUBLE_EQ(diff.MaxTileShift, 40.0);
        EXPECT_DOUBLE_EQ(diff.DifferingFraction, 256.0 / 2048.0);
        EXPECT_EQ(diff.MaxDelta, 40u);
    }

    // Per-pixel jitter that cancels inside a tile (a temporal pass's
    // checkerboard) leaves the tile means where they were: the tile term is
    // what lets a TAA'd frame be compared at all.
    TEST(LifecycleSoakStats, JitterThatCancelsInsideATileLeavesTheTileMean)
    {
        const Frame a = Solid(32, 32, 100);
        Frame b = a;
        for (u32 y = 0; y < 32u; ++y)
        {
            for (u32 x = 0; x < 32u; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * 32u + x) * 4u;
                b.Rgba[i] = ((x + y) % 2u == 0u) ? 103 : 97;
            }
        }
        const FrameDiff diff = CompareFrames(a, b);
        EXPECT_DOUBLE_EQ(diff.MaxTileShift, 0.0);
        EXPECT_EQ(diff.MaxDelta, 3u);
        EXPECT_DOUBLE_EQ(diff.DifferingFraction, 0.0) << "a 3/255 wobble is under the pixel threshold";
    }

    TEST(LifecycleSoakStats, FramesOfDifferentShapesNeverCompareEqual)
    {
        const FrameDiff diff = CompareFrames(Solid(64, 32, 0), Solid(32, 64, 0));
        EXPECT_TRUE(diff.ShapeMismatch);
        EXPECT_FALSE(JudgeAgainstControl(diff, FrameDiff{}).Held);

        Frame truncated = Solid(8, 8, 0);
        truncated.Rgba.pop_back();
        EXPECT_TRUE(CompareFrames(truncated, truncated).ShapeMismatch) << "a buffer short of its extent is rejected";
    }

    // A bit-exact control still allows the absolute minimums; a noisy control
    // scales the limit by kFloorScale, never below the minimums.
    TEST(LifecycleSoakStats, TheJudgementScalesTheControlFloorWithAbsoluteMinimums)
    {
        const FrameDiff exactFloor{};
        FrameDiff small;
        small.MaxTileShift = kMinTileShift;
        small.DifferingFraction = kMinDifferingFraction;
        EXPECT_TRUE(JudgeAgainstControl(small, exactFloor).Held);

        FrameDiff over = small;
        over.MaxTileShift = kMinTileShift * 1.01;
        EXPECT_FALSE(JudgeAgainstControl(over, exactFloor).Held);

        FrameDiff noisyFloor;
        noisyFloor.MaxTileShift = 2.0;
        noisyFloor.DifferingFraction = 0.01;
        FrameDiff withinNoise;
        withinNoise.MaxTileShift = 2.0 * kFloorScale;
        withinNoise.DifferingFraction = 0.01 * kFloorScale;
        EXPECT_TRUE(JudgeAgainstControl(withinNoise, noisyFloor).Held);
        withinNoise.DifferingFraction = 0.01 * kFloorScale * 1.01;
        EXPECT_FALSE(JudgeAgainstControl(withinNoise, noisyFloor).Held);
    }

    TEST(LifecycleSoakStats, AnUnmeasurableControlIsNotATolerance)
    {
        FrameDiff floor;
        floor.ShapeMismatch = true;
        EXPECT_FALSE(JudgeAgainstControl(FrameDiff{}, floor).Held);
        floor = FrameDiff{};
        floor.MaxTileShift = std::numeric_limits<f64>::quiet_NaN();
        EXPECT_FALSE(JudgeAgainstControl(FrameDiff{}, floor).Held);
    }

    // A warm cache: fills, then stays (a dip and a return are not growth). A
    // leak: adds every cycle, and its slope is the per-cycle increment.
    TEST(LifecycleSoakStats, GrowthSeparatesAWarmCacheFromALeak)
    {
        constexpr u64 MiB = 1024ull * 1024ull;
        const std::array<u64, 4> warm{ 500 * MiB, 498 * MiB, 501 * MiB, 500 * MiB };
        const GrowthVerdict cache = JudgeGrowth(warm, 4 * MiB);
        EXPECT_TRUE(cache.Bounded) << cache.Describe("warm cache");
        EXPECT_EQ(cache.Peak, 501 * MiB);

        const std::array<u64, 4> leaking{ 500 * MiB, 503 * MiB, 506 * MiB, 509 * MiB };
        const GrowthVerdict leak = JudgeGrowth(leaking, 4 * MiB);
        EXPECT_FALSE(leak.Bounded) << leak.Describe("leak");
        EXPECT_EQ(leak.Growth, static_cast<i64>(9 * MiB));
        EXPECT_NEAR(leak.SlopePerCycle, static_cast<f64>(3 * MiB), 1.0);

        const std::array<u64, 2> shrinking{ 600 * MiB, 400 * MiB };
        EXPECT_TRUE(JudgeGrowth(shrinking, 0).Bounded) << "shrinking is never growth";
        EXPECT_TRUE(JudgeGrowth(std::span<const u64>{}, 0).Bounded) << "no samples, no verdict against";
    }

    TEST(LifecycleSoakStats, AControlFrameRoundTripsAndATamperedOneIsRefused)
    {
        Frame frame = Solid(5, 3, 0);
        for (sizet i = 0; i < frame.Rgba.size(); ++i)
            frame.Rgba[i] = static_cast<u8>(i * 7u);
        const std::string path = TempFile("control.olosoak").string();
        ASSERT_TRUE(SaveFrame(path, frame));
        const auto loaded = LoadFrame(path);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->Width, 5u);
        EXPECT_EQ(loaded->Height, 3u);
        EXPECT_EQ(loaded->Rgba, frame.Rgba);

        {
            std::ofstream append(path, std::ios::binary | std::ios::app);
            append.put('x');
        }
        EXPECT_FALSE(LoadFrame(path).has_value()) << "trailing bytes mean it is not a frame this soak wrote";
        EXPECT_FALSE(LoadFrame(TempFile("absent.olosoak").string()).has_value());

        Frame wrongSize = frame;
        wrongSize.Rgba.pop_back();
        EXPECT_FALSE(SaveFrame(TempFile("bad.olosoak").string(), wrongSize));
    }

    TEST(LifecycleSoakStats, TheReportIsJsonWithEveryAcceptanceField)
    {
        Report report;
        report.Leg = "editor-loose";
        report.Backend = "GL";
        report.Build = "abc \"quoted\"\n";
        report.Cycles = 2;
        report.Operations = { "open A", "play/stop" };
        report.Skipped = { "msaa: not in this sequence" };
        report.Executed = { { "open A", 3u } };
        CycleRecord record;
        record.Cycle = 1;
        report.Records.push_back(record);
        report.GpuGrowth = JudgeGrowth(std::array<u64, 2>{ 1, 2 }, 0);
        FrameDiff mismatch;
        mismatch.ShapeMismatch = true;
        mismatch.MaxTileShift = std::numeric_limits<f64>::infinity();
        report.AfterRestart = mismatch;

        std::ostringstream out;
        WriteJson(out, report);
        const std::string json = out.str();
        for (const char* key : { "\"schema\"", "\"operations\"", "\"skipped\"", "\"executed\"", "\"frameTimes\"",
                                 "\"p99Ms\"", "\"gpuGrowth\"", "\"processGrowth\": null", "\"afterRestart\"",
                                 "\"cycleRecords\"", "\"frameIndexAtReturn\"", "\"validationDiagnostics\"" })
        {
            EXPECT_NE(json.find(key), std::string::npos) << key << " missing from\n"
                                                         << json;
        }
        EXPECT_NE(json.find(R"("abc \"quoted\"\n")"), std::string::npos) << "strings are escaped";
        EXPECT_EQ(json.find("inf"), std::string::npos) << "JSON has no infinity";
        EXPECT_NE(json.find("\"maxTileShift\":null"), std::string::npos);
    }
} // namespace OloEngine::Tests::Lifecycle
