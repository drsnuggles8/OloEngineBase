// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure JSON shaping behind olo_perf_pass_timings (issue #316:
// split whole-frame GPU time by render-graph pass). The shaping lives in a header
// over engine-free input structs (MCP/McpPassTimings.h), so it is exercised here
// against synthetic pass/frame data — the test binary deliberately does NOT
// compile McpTools.cpp (the editor-backed handler). The live tool's
// GPUPassTimerPool -> JSON path is verified over the MCP attach loop; this pins
// the join/rounding/totals shape.
#include "MCP/McpPassTimings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace
{
    using OloEngine::MCP::PassTimings::BuildPassTimings;
    using OloEngine::MCP::PassTimings::CpuPassEntry;
    using OloEngine::MCP::PassTimings::FrameTotals;
    using OloEngine::MCP::PassTimings::GpuPassEntry;
    using OloEngine::MCP::PassTimings::Round3;
    using Json = OloEngine::MCP::PassTimings::Json;

    FrameTotals MakeTotals()
    {
        FrameTotals totals;
        totals.FrameTimeMs = 12.65;
        totals.CpuMs = 4.2;
        totals.GpuMs = 8.1;
        totals.GpuWaitMs = 3.4;
        totals.GpuResultsAgeFrames = 2;
        return totals;
    }
} // namespace

TEST(McpPassTimingsTest, JoinsGpuAndCpuByPassName)
{
    const std::vector<GpuPassEntry> gpu = {
        { "ShadowPass", 1.25 },
        { "ScenePass", 5.5 },
        { "GTAOPass", 0.7 },
    };
    const std::vector<CpuPassEntry> cpu = {
        { "ScenePass", 2.0 },
        { "ShadowPass", 0.5 },
        { "GTAOPass", 0.1 },
    };

    const Json o = BuildPassTimings(gpu, cpu, MakeTotals());

    ASSERT_TRUE(o.contains("passes"));
    ASSERT_EQ(o["passes"].size(), 3u);

    // GPU execution order is preserved as the primary ordering.
    EXPECT_EQ(o["passes"][0]["pass"], "ShadowPass");
    EXPECT_DOUBLE_EQ(o["passes"][0]["gpuMs"].get<double>(), 1.25);
    EXPECT_DOUBLE_EQ(o["passes"][0]["cpuMs"].get<double>(), 0.5);

    EXPECT_EQ(o["passes"][1]["pass"], "ScenePass");
    EXPECT_DOUBLE_EQ(o["passes"][1]["gpuMs"].get<double>(), 5.5);
    EXPECT_DOUBLE_EQ(o["passes"][1]["cpuMs"].get<double>(), 2.0);

    EXPECT_DOUBLE_EQ(o["passGpuTotalMs"].get<double>(), 7.45);
}

TEST(McpPassTimingsTest, AppendsCpuOnlyPassesWithZeroGpu)
{
    const std::vector<GpuPassEntry> gpu = { { "ScenePass", 5.0 } };
    const std::vector<CpuPassEntry> cpu = {
        { "ScenePass", 2.0 },
        { "BloomPass", 0.3 }, // ran on CPU this frame, not in the resolved GPU frame
    };

    const Json o = BuildPassTimings(gpu, cpu, MakeTotals());

    ASSERT_EQ(o["passes"].size(), 2u);
    EXPECT_EQ(o["passes"][1]["pass"], "BloomPass");
    EXPECT_DOUBLE_EQ(o["passes"][1]["gpuMs"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(o["passes"][1]["cpuMs"].get<double>(), 0.3);
}

TEST(McpPassTimingsTest, DuplicatePassNamesJoinPositionally)
{
    // Two passes with the same name (e.g. a pass that runs twice): each GPU
    // entry consumes a distinct CPU entry instead of double-counting the first.
    const std::vector<GpuPassEntry> gpu = {
        { "BlurPass", 1.0 },
        { "BlurPass", 2.0 },
    };
    const std::vector<CpuPassEntry> cpu = {
        { "BlurPass", 0.25 },
        { "BlurPass", 0.75 },
    };

    const Json o = BuildPassTimings(gpu, cpu, MakeTotals());

    ASSERT_EQ(o["passes"].size(), 2u);
    EXPECT_DOUBLE_EQ(o["passes"][0]["cpuMs"].get<double>(), 0.25);
    EXPECT_DOUBLE_EQ(o["passes"][1]["cpuMs"].get<double>(), 0.75);
}

TEST(McpPassTimingsTest, FrameTotalsAndUnattributedGpu)
{
    const std::vector<GpuPassEntry> gpu = {
        { "ScenePass", 5.0 },
        { "ToneMapPass", 1.0 },
    };

    const Json o = BuildPassTimings(gpu, {}, MakeTotals());

    EXPECT_DOUBLE_EQ(o["frame"]["frameTimeMs"].get<double>(), 12.65);
    EXPECT_DOUBLE_EQ(o["frame"]["cpuMs"].get<double>(), 4.2);
    EXPECT_DOUBLE_EQ(o["frame"]["gpuMs"].get<double>(), 8.1);
    EXPECT_DOUBLE_EQ(o["frame"]["gpuWaitMs"].get<double>(), 3.4);
    EXPECT_EQ(o["gpuResultsAgeFrames"].get<std::uint64_t>(), 2u);

    // 8.1 total - 6.0 attributed = 2.1 unattributed (barriers, HZB rebuild, ...).
    EXPECT_NEAR(o["unattributedGpuMs"].get<double>(), 2.1, 1e-9);
}

TEST(McpPassTimingsTest, UnattributedGpuClampsToZeroWhenPassesExceedFrameSpan)
{
    FrameTotals totals = MakeTotals();
    totals.GpuMs = 4.0;
    const std::vector<GpuPassEntry> gpu = { { "ScenePass", 5.0 } }; // overlap artifact

    const Json o = BuildPassTimings(gpu, {}, totals);

    EXPECT_DOUBLE_EQ(o["unattributedGpuMs"].get<double>(), 0.0);
}

TEST(McpPassTimingsTest, EmptyInputsProduceEmptyPassListAndZeroTotals)
{
    FrameTotals totals; // all zeros
    const Json o = BuildPassTimings({}, {}, totals);

    EXPECT_TRUE(o["passes"].empty());
    EXPECT_DOUBLE_EQ(o["passGpuTotalMs"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(o["unattributedGpuMs"].get<double>(), 0.0);
    EXPECT_EQ(o["gpuResultsAgeFrames"].get<std::uint64_t>(), 0u);
}

TEST(McpPassTimingsTest, RoundsToThreeDecimals)
{
    EXPECT_DOUBLE_EQ(Round3(0.0004999), 0.0);
    EXPECT_DOUBLE_EQ(Round3(0.0005001), 0.001);
    EXPECT_DOUBLE_EQ(Round3(1.23456), 1.235);
}

// ---- sub-pass grouping ("Parent/Sub" GPU entries, #316) ---------------------

// A "Parent/Sub" GPU entry attaches to its parent's subPasses instead of the
// top-level list, and does NOT count toward passGpuTotalMs (its time is inside
// the parent's bracket).
TEST(McpPassTimingsTest, GroupsSubPassEntriesUnderParent)
{
    const std::vector<GpuPassEntry> gpu = {
        { "ShadowPass", 1.0 },
        { "ScenePass", 46.6 },
        { "ScenePass/DepthPrepass", 21.9 },
        { "ScenePass/Color", 24.2 },
        { "GTAOPass", 0.7 },
    };
    const std::vector<CpuPassEntry> cpu = { { "ScenePass", 2.0 } };

    const Json o = BuildPassTimings(gpu, cpu, MakeTotals());

    // Sub entries are folded into the parent: 3 top-level passes remain.
    ASSERT_EQ(o["passes"].size(), 3u);
    const Json& scene = o["passes"][1];
    EXPECT_EQ(scene["pass"], "ScenePass");
    EXPECT_DOUBLE_EQ(scene["cpuMs"].get<double>(), 2.0);

    ASSERT_TRUE(scene.contains("subPasses"));
    ASSERT_EQ(scene["subPasses"].size(), 2u);
    EXPECT_EQ(scene["subPasses"][0]["name"], "DepthPrepass");
    EXPECT_DOUBLE_EQ(scene["subPasses"][0]["gpuMs"].get<double>(), 21.9);
    EXPECT_EQ(scene["subPasses"][1]["name"], "Color");
    EXPECT_DOUBLE_EQ(scene["subPasses"][1]["gpuMs"].get<double>(), 24.2);

    // Passes without sub brackets carry no subPasses key at all.
    EXPECT_FALSE(o["passes"][0].contains("subPasses"));

    // Total counts each top-level pass once — sub times are already inside.
    EXPECT_NEAR(o["passGpuTotalMs"].get<double>(), 1.0 + 46.6 + 0.7, 1e-9);
}

// An orphan sub entry (parent not GPU-timed this frame — pool overflow) stays
// a top-level entry under its full name and counts toward the total, rather
// than being dropped silently.
TEST(McpPassTimingsTest, OrphanSubPassEntryStaysTopLevel)
{
    const std::vector<GpuPassEntry> gpu = {
        { "ScenePass/Color", 24.2 },
    };

    const Json o = BuildPassTimings(gpu, {}, MakeTotals());

    ASSERT_EQ(o["passes"].size(), 1u);
    EXPECT_EQ(o["passes"][0]["pass"], "ScenePass/Color");
    EXPECT_DOUBLE_EQ(o["passes"][0]["gpuMs"].get<double>(), 24.2);
    EXPECT_NEAR(o["passGpuTotalMs"].get<double>(), 24.2, 1e-9);
}

// With duplicate parent names (a pass that runs twice), a sub entry attaches to
// the MOST RECENT parent — the pool allocates parent-before-sub in execution
// order, so the nearest preceding entry is the owning bracket.
TEST(McpPassTimingsTest, SubPassAttachesToMostRecentParent)
{
    const std::vector<GpuPassEntry> gpu = {
        { "BlurPass", 1.0 },
        { "BlurPass", 2.0 },
        { "BlurPass/Horizontal", 0.9 },
    };

    const Json o = BuildPassTimings(gpu, {}, MakeTotals());

    ASSERT_EQ(o["passes"].size(), 2u);
    EXPECT_FALSE(o["passes"][0].contains("subPasses"));
    ASSERT_TRUE(o["passes"][1].contains("subPasses"));
    EXPECT_EQ(o["passes"][1]["subPasses"][0]["name"], "Horizontal");
}
