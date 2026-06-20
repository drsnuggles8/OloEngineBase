#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure JSON shaping behind olo_render_frame_breakdown (issue
// #306 item D, bullet 2: surface the CommandPacketDebugger / FrameCaptureManager
// data over MCP). The shaping lives in a header that touches ONLY the already-
// captured frame data (MCP/McpFrameBreakdown.h), so it is exercised here against a
// synthetic CapturedFrameData — the test binary deliberately does NOT compile
// McpTools.cpp (the editor-backed handler) nor CommandPacketDebugger.h (ImGui).
// The live tool's capture-then-poll + markdown path is verified over the MCP
// attach loop; this pins the per-command / per-stage breakdown shape.
#include "MCP/McpFrameBreakdown.h"

#include <string>

namespace
{
    using OloEngine::CapturedCommandData;
    using OloEngine::CapturedFrameData;
    using OloEngine::CommandType;
    using OloEngine::DrawKey;
    using OloEngine::RenderMode;
    using OloEngine::ViewLayerType;
    using OloEngine::MCP::FrameBreakdown::BuildBreakdown;
    using OloEngine::MCP::FrameBreakdown::ParseViewMode;
    using OloEngine::MCP::FrameBreakdown::ViewMode;
    using Json = OloEngine::MCP::FrameBreakdown::Json;

    // Build one synthetic captured command. rawData is null (the breakdown only
    // reads metadata + the DrawKey, never the deep-copied command bytes), so a POD
    // payload is unnecessary for these tests.
    CapturedCommandData MakeCmd(CommandType type, const char* name, u32 shaderId, u32 materialId,
                                u32 depth, u32 execOrder, u32 groupId, bool isStatic,
                                u32 originalIndex)
    {
        const DrawKey key = DrawKey::CreateOpaque(/*viewportID*/ 0, ViewLayerType::ThreeD, shaderId, materialId, depth);
        return CapturedCommandData(type, /*rawData*/ nullptr, /*dataSize*/ 0, key, groupId, execOrder,
                                   isStatic, /*dependsOnPrevious*/ false, name, originalIndex);
    }

    // A frame with three post-sort draws (two meshes + a state command) so the
    // command-list, histogram and field-mapping assertions have something to chew.
    CapturedFrameData MakeFrame()
    {
        CapturedFrameData frame;
        frame.FrameNumber = 42;
        frame.TimestampSeconds = 1.5;
        frame.Stats.TotalCommands = 3;
        frame.Stats.DrawCalls = 2;
        frame.Stats.BatchedCommands = 1;
        frame.Stats.StateChanges = 1;
        frame.Stats.ShaderBinds = 2;
        frame.Stats.TextureBinds = 4;
        frame.Stats.SortTimeMs = 0.123;
        frame.Stats.BatchTimeMs = 0.045;
        frame.Stats.ExecuteTimeMs = 0.678;
        frame.Stats.TotalFrameTimeMs = 1.234;

        frame.PreSortCommands.push_back(MakeCmd(CommandType::SetViewport, "viewport", 0, 0, 0, 0, 0, false, 0));
        frame.PreSortCommands.push_back(MakeCmd(CommandType::DrawMesh, "cube", 7, 11, 100, 1, 1, true, 1));
        frame.PreSortCommands.push_back(MakeCmd(CommandType::DrawMesh, "sphere", 7, 12, 200, 2, 2, false, 2));

        // Post-sort: same commands but reordered (state first, draws sorted).
        frame.PostSortCommands.push_back(MakeCmd(CommandType::SetViewport, "viewport", 0, 0, 0, 0, 0, false, 0));
        frame.PostSortCommands.push_back(MakeCmd(CommandType::DrawMesh, "cube", 7, 11, 100, 1, 1, true, 1));
        frame.PostSortCommands.push_back(MakeCmd(CommandType::DrawMesh, "sphere", 7, 12, 200, 2, 2, false, 2));
        return frame;
    }
} // namespace

TEST(McpFrameBreakdown, ParseViewModeMapsStringsAndDefaults)
{
    EXPECT_EQ(ViewMode::PreSort, ParseViewMode("presort"));
    EXPECT_EQ(ViewMode::PostSort, ParseViewMode("postsort"));
    EXPECT_EQ(ViewMode::PostBatch, ParseViewMode("postbatch"));
    // Unknown / empty -> PostBatch (the "what actually ran" default).
    EXPECT_EQ(ViewMode::PostBatch, ParseViewMode("nonsense"));
    EXPECT_EQ(ViewMode::PostBatch, ParseViewMode(""));
}

TEST(McpFrameBreakdown, EmptyFrameProducesValidShape)
{
    CapturedFrameData frame;
    frame.FrameNumber = 5;
    const Json j = BuildBreakdown(frame, ViewMode::PostBatch, 200);

    EXPECT_EQ(5u, j["frameNumber"].get<u32>());
    EXPECT_EQ(0u, j["commandCount"].get<sizet>());
    EXPECT_EQ(0u, j["returnedCommands"].get<sizet>());
    EXPECT_FALSE(j["truncated"].get<bool>());
    EXPECT_TRUE(j["commands"].is_array());
    EXPECT_TRUE(j["commands"].empty());
    EXPECT_TRUE(j["commandTypeHistogram"].is_object());
    EXPECT_TRUE(j.contains("stats"));
    EXPECT_TRUE(j.contains("stageCounts"));
}

TEST(McpFrameBreakdown, PostSortCommandsAreListedInStageOrder)
{
    const CapturedFrameData frame = MakeFrame();
    // PostBatch is empty -> falls back to PostSort.
    const Json j = BuildBreakdown(frame, ViewMode::PostBatch, 200);

    EXPECT_EQ("postsort", j["viewMode"].get<std::string>());
    EXPECT_EQ("postbatch", j["requestedViewMode"].get<std::string>());
    EXPECT_EQ(3u, j["commandCount"].get<sizet>());
    EXPECT_EQ(3u, j["returnedCommands"].get<sizet>());
    EXPECT_FALSE(j["truncated"].get<bool>());

    const Json& cmds = j["commands"];
    ASSERT_EQ(3u, cmds.size());
    EXPECT_EQ("SetViewport", cmds[0]["type"].get<std::string>());
    EXPECT_EQ("DrawMesh", cmds[1]["type"].get<std::string>());
    EXPECT_EQ("sphere", cmds[2]["debugName"].get<std::string>());

    // index reflects position in the listed stage; isDraw distinguishes draws.
    EXPECT_EQ(0u, cmds[0]["index"].get<sizet>());
    EXPECT_FALSE(cmds[0]["isDraw"].get<bool>());
    EXPECT_TRUE(cmds[1]["isDraw"].get<bool>());
}

TEST(McpFrameBreakdown, DrawKeyAndMetadataFieldsDecoded)
{
    const CapturedFrameData frame = MakeFrame();
    const Json j = BuildBreakdown(frame, ViewMode::PostSort, 200);

    const Json& cube = j["commands"][1];
    EXPECT_EQ("cube", cube["debugName"].get<std::string>());
    EXPECT_TRUE(cube["static"].get<bool>());
    EXPECT_FALSE(cube["dependsOnPrevious"].get<bool>());
    EXPECT_EQ(1u, cube["executionOrder"].get<u32>());
    EXPECT_EQ(1u, cube["groupId"].get<u32>());
    EXPECT_EQ(1u, cube["originalIndex"].get<u32>());

    const Json& key = cube["drawKey"];
    EXPECT_EQ(7u, key["shaderId"].get<u32>());
    EXPECT_EQ(11u, key["materialId"].get<u32>());
    EXPECT_EQ(100u, key["depth"].get<u32>());
    EXPECT_EQ(0u, key["viewportId"].get<u32>());
    EXPECT_EQ("3D", key["viewLayer"].get<std::string>());
    EXPECT_EQ("Opaque", key["renderMode"].get<std::string>());
}

TEST(McpFrameBreakdown, GpuTimeIsReportedRounded)
{
    CapturedFrameData frame;
    CapturedCommandData cmd = MakeCmd(CommandType::DrawMesh, "timed", 1, 1, 0, 0, 0, false, 0);
    cmd.SetGpuTimeMs(0.123456);
    frame.PostSortCommands.push_back(std::move(cmd));

    const Json j = BuildBreakdown(frame, ViewMode::PostSort, 200);
    EXPECT_NEAR(0.1235, j["commands"][0]["gpuMs"].get<double>(), 1e-9);
}

TEST(McpFrameBreakdown, MaxCommandsTruncatesButReportsFullCountAndHistogram)
{
    CapturedFrameData frame;
    for (u32 i = 0; i < 5; ++i)
        frame.PostSortCommands.push_back(MakeCmd(CommandType::DrawMesh, "m", 1, i, i, i, i, false, i));
    frame.PostSortCommands.push_back(MakeCmd(CommandType::SetViewport, "vp", 0, 0, 0, 5, 5, false, 5));

    const Json j = BuildBreakdown(frame, ViewMode::PostSort, /*maxCommands*/ 2);

    EXPECT_EQ(6u, j["commandCount"].get<sizet>());
    EXPECT_EQ(2u, j["returnedCommands"].get<sizet>());
    EXPECT_TRUE(j["truncated"].get<bool>());
    EXPECT_EQ(2u, j["commands"].size());

    // Histogram covers the FULL stage, not just the returned slice.
    const Json& hist = j["commandTypeHistogram"];
    EXPECT_EQ(5u, hist["DrawMesh"].get<u32>());
    EXPECT_EQ(1u, hist["SetViewport"].get<u32>());
}

TEST(McpFrameBreakdown, ViewModeFallsBackToPreSortWhenLaterStagesEmpty)
{
    CapturedFrameData frame;
    frame.PreSortCommands.push_back(MakeCmd(CommandType::DrawMesh, "only", 1, 1, 0, 0, 0, false, 0));
    // PostSort and PostBatch are empty.

    const Json jPostBatch = BuildBreakdown(frame, ViewMode::PostBatch, 200);
    EXPECT_EQ("presort", jPostBatch["viewMode"].get<std::string>());
    EXPECT_EQ(1u, jPostBatch["commandCount"].get<sizet>());

    const Json jPostSort = BuildBreakdown(frame, ViewMode::PostSort, 200);
    EXPECT_EQ("presort", jPostSort["viewMode"].get<std::string>());

    // An explicit presort request stays presort even though it's the only stage.
    const Json jPreSort = BuildBreakdown(frame, ViewMode::PreSort, 200);
    EXPECT_EQ("presort", jPreSort["viewMode"].get<std::string>());
}

TEST(McpFrameBreakdown, StatsAndStageCountsPassThrough)
{
    const CapturedFrameData frame = MakeFrame();
    const Json j = BuildBreakdown(frame, ViewMode::PostSort, 200);

    const Json& stats = j["stats"];
    EXPECT_EQ(3u, stats["totalCommands"].get<u32>());
    EXPECT_EQ(2u, stats["drawCalls"].get<u32>());
    EXPECT_EQ(1u, stats["batchedCommands"].get<u32>());
    EXPECT_EQ(1u, stats["stateChanges"].get<u32>());
    EXPECT_EQ(2u, stats["shaderBinds"].get<u32>());
    EXPECT_EQ(4u, stats["textureBinds"].get<u32>());
    EXPECT_NEAR(0.123, stats["sortMs"].get<double>(), 1e-9);
    EXPECT_NEAR(1.234, stats["totalMs"].get<double>(), 1e-9);

    const Json& stage = j["stageCounts"];
    EXPECT_EQ(3u, stage["preSort"].get<sizet>());
    EXPECT_EQ(3u, stage["postSort"].get<sizet>());
    EXPECT_EQ(0u, stage["postBatch"].get<sizet>());
}
