// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the selection behind olo_perf_frame_history's 'worst' list
// (#1533). The selection lives in a header over an engine-free struct
// (MCP/McpFrameHistory.h); the test binary deliberately does NOT compile
// McpToolsPerf.cpp. The live tool's RendererProfiler ring -> JSON path is
// verified over the MCP attach loop; this pins which frames are listed and in
// what order.
#include "MCP/McpFrameHistory.h"

#include <cstddef>
#include <vector>

namespace
{
    using OloEngine::MCP::FrameHistory::FrameSample;
    using OloEngine::MCP::FrameHistory::SlowestFrames;

    [[nodiscard]] std::vector<FrameSample> Steady(std::size_t n, double ms)
    {
        std::vector<FrameSample> frames(n);
        for (FrameSample& f : frames)
        {
            f.FrameTimeMs = ms;
        }
        return frames;
    }
} // namespace

TEST(McpFrameHistoryTest, TheSlowestFramesComeBackOldestFirst)
{
    std::vector<FrameSample> frames = Steady(10, 15.0);
    frames[7].FrameTimeMs = 44.0;
    frames[2].FrameTimeMs = 30.0;
    frames[5].FrameTimeMs = 40.0;
    const std::vector<std::size_t> worst = SlowestFrames(frames, 2);
    ASSERT_EQ(worst.size(), 2u);
    EXPECT_EQ(worst[0], 5u) << "the two slowest are frames 5 and 7, listed in ring order";
    EXPECT_EQ(worst[1], 7u);
}

// The point of the list: a periodic hitch shows its period as the gaps.
TEST(McpFrameHistoryTest, APeriodicHitchReadsOffAsItsPeriod)
{
    std::vector<FrameSample> frames = Steady(1024, 14.6);
    for (std::size_t i = 13; i < frames.size(); i += 14)
    {
        frames[i].FrameTimeMs = 44.0;
    }
    const std::vector<std::size_t> worst = SlowestFrames(frames, 20);
    ASSERT_EQ(worst.size(), 20u);
    for (std::size_t k = 1; k < worst.size(); ++k)
    {
        EXPECT_EQ(worst[k] - worst[k - 1], 14u) << "listing " << k;
    }
}

TEST(McpFrameHistoryTest, AFillingRingNeverCountsItsEmptyFrames)
{
    std::vector<FrameSample> frames = Steady(8, 0.0); // a ring with no time yet
    frames[3].FrameTimeMs = 12.0;
    const std::vector<std::size_t> worst = SlowestFrames(frames, 5);
    ASSERT_EQ(worst.size(), 1u) << "only frames with a time can be slow";
    EXPECT_EQ(worst[0], 3u);
    EXPECT_TRUE(SlowestFrames({}, 5).empty());
    EXPECT_TRUE(SlowestFrames(frames, 0).empty());
}

TEST(McpFrameHistoryTest, ATieKeepsTheOlderFrame)
{
    const std::vector<FrameSample> frames = Steady(6, 20.0);
    const std::vector<std::size_t> worst = SlowestFrames(frames, 3);
    ASSERT_EQ(worst.size(), 3u);
    EXPECT_EQ(worst[0], 0u);
    EXPECT_EQ(worst[1], 1u);
    EXPECT_EQ(worst[2], 2u);
}
