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

#include <initializer_list>
#include <string>

namespace
{
    using OloEngine::CapturedCommandData;
    using OloEngine::CapturedFrameData;
    using OloEngine::CapturedPassData;
    using OloEngine::CommandType;
    using OloEngine::DrawKey;
    using OloEngine::RenderMode;
    using OloEngine::ViewLayerType;
    using OloEngine::MCP::FrameBreakdown::BuildBreakdown;
    using OloEngine::MCP::FrameBreakdown::GraphAttribution;
    using OloEngine::MCP::FrameBreakdown::GraphPassInfo;
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

// ---- graph-wide command-bucket attribution (#316 Part 4) --------------------

namespace
{
    // A synthetic multi-pass graph mirroring a typical deferred path: a shadow
    // pass (no bucket), the scene pass (bucket + capture source), water and
    // forward-overlay (bucket, not captured), a culled pass, and the final
    // composite (no bucket).
    GraphAttribution MakeGraphAttribution()
    {
        GraphAttribution attr;
        attr.CaptureSourcePass = "SceneRenderPass";
        attr.Passes.push_back(GraphPassInfo{ "ShadowPass", "Graphics", /*bucket*/ false, /*culled*/ false, /*final*/ false, /*idx*/ 0 });
        attr.Passes.push_back(GraphPassInfo{ "SceneRenderPass", "Graphics", true, false, false, 1 });
        attr.Passes.push_back(GraphPassInfo{ "WaterRenderPass", "Graphics", true, false, false, 2 });
        attr.Passes.push_back(GraphPassInfo{ "ForwardOverlayPass", "Graphics", true, false, false, 3 });
        attr.Passes.push_back(GraphPassInfo{ "DisabledFogPass", "Graphics", true, /*culled*/ true, false, -1 });
        attr.Passes.push_back(GraphPassInfo{ "CompositePass", "Graphics", false, false, /*final*/ true, 4 });
        return attr;
    }
} // namespace

TEST(McpFrameBreakdown, SourcePassReportedFromFrameWithoutAttribution)
{
    CapturedFrameData frame = MakeFrame();
    frame.SourcePassName = "SceneRenderPass";

    // No attribution supplied -> sourcePass from the frame, no graph block.
    const Json j = BuildBreakdown(frame, ViewMode::PostSort, 200);
    EXPECT_EQ("SceneRenderPass", j["sourcePass"].get<std::string>());
    EXPECT_EQ("SceneRenderPass command bucket", j["bucket"].get<std::string>());
    EXPECT_FALSE(j.contains("graphAttribution"));
}

TEST(McpFrameBreakdown, EmptySourcePassFallsBackToGenericBucketLabel)
{
    // A synthetic frame (no recorded pass) keeps an empty sourcePass and the
    // generic bucket label, so existing callers/tests are unaffected.
    const CapturedFrameData frame = MakeFrame();
    const Json j = BuildBreakdown(frame, ViewMode::PostSort, 200);
    EXPECT_EQ("", j["sourcePass"].get<std::string>());
    EXPECT_EQ("scene render command bucket", j["bucket"].get<std::string>());
}

TEST(McpFrameBreakdown, GraphAttributionEnumeratesPassesAndFlagsCaptureSource)
{
    CapturedFrameData frame = MakeFrame();
    frame.SourcePassName = "SceneRenderPass";
    const GraphAttribution attr = MakeGraphAttribution();

    const Json j = BuildBreakdown(frame, ViewMode::PostSort, 200, &attr);

    ASSERT_TRUE(j.contains("graphAttribution"));
    const Json& g = j["graphAttribution"];
    EXPECT_EQ("SceneRenderPass", g["captureSourcePass"].get<std::string>());
    EXPECT_EQ(6u, g["passCount"].get<u32>());
    // ShadowPass + Composite lack buckets; DisabledFogPass owns a bucket but is
    // culled. commandBucketPassCount counts only command-bucket passes that RAN
    // (non-culled) -> Scene + Water + ForwardOverlay = 3.
    EXPECT_EQ(3u, g["commandBucketPassCount"].get<u32>());
    // This frame has no per-pass captures (legacy single-pass MakeFrame), so the
    // captured count falls back to the source-pass-only count of 1.
    EXPECT_EQ(1u, g["capturedPassCount"].get<u32>());

    const Json& passes = g["passes"];
    ASSERT_EQ(6u, passes.size());

    // Order is preserved from the attribution (submission order).
    EXPECT_EQ("ShadowPass", passes[0]["name"].get<std::string>());
    EXPECT_FALSE(passes[0]["usesCommandBucket"].get<bool>());
    EXPECT_FALSE(passes[0]["isCaptureSource"].get<bool>());

    // The scene pass is the one capture source and owns a bucket.
    const Json& scene = passes[1];
    EXPECT_EQ("SceneRenderPass", scene["name"].get<std::string>());
    EXPECT_TRUE(scene["usesCommandBucket"].get<bool>());
    EXPECT_TRUE(scene["isCaptureSource"].get<bool>());
    EXPECT_EQ(1, scene["executionIndex"].get<int>());

    // Water owns a bucket but is NOT the capture source (the deferred gap).
    const Json& water = passes[2];
    EXPECT_TRUE(water["usesCommandBucket"].get<bool>());
    EXPECT_FALSE(water["isCaptureSource"].get<bool>());

    // The culled pass carries its cull flag and an unscheduled execution index.
    const Json& fog = passes[4];
    EXPECT_EQ("DisabledFogPass", fog["name"].get<std::string>());
    EXPECT_TRUE(fog["culled"].get<bool>());
    EXPECT_EQ(-1, fog["executionIndex"].get<int>());

    // The final pass is flagged.
    EXPECT_TRUE(passes[5]["isFinalPass"].get<bool>());
}

TEST(McpFrameBreakdown, CaptureSourceFallsBackToAttributionWhenFrameUnset)
{
    // Frame has no recorded source pass, but the attribution carries one — the
    // breakdown still attributes to it and flags the matching pass.
    CapturedFrameData frame = MakeFrame();
    frame.SourcePassName.clear();
    const GraphAttribution attr = MakeGraphAttribution();

    const Json j = BuildBreakdown(frame, ViewMode::PostSort, 200, &attr);
    EXPECT_EQ("SceneRenderPass", j["sourcePass"].get<std::string>());

    const Json& passes = j["graphAttribution"]["passes"];
    EXPECT_TRUE(passes[1]["isCaptureSource"].get<bool>());
    EXPECT_FALSE(passes[2]["isCaptureSource"].get<bool>());
}

// ---- full per-pass command capture (#463 / #316 Part 4) ---------------------

namespace
{
    // One captured per-pass entry with `drawNames.size()` DrawMesh commands in
    // both submission (presort) and sorted (postsort) order — mirrors what the
    // secondary command-bucket passes (Water / Foliage / Decal / ForwardOverlay)
    // accumulate: sorted but not batched.
    CapturedPassData MakePassEntry(const char* name, std::initializer_list<const char*> drawNames)
    {
        CapturedPassData pass;
        pass.PassName = name;
        u32 idx = 0;
        for (const char* dn : drawNames)
        {
            pass.PreSortCommands.push_back(MakeCmd(CommandType::DrawMesh, dn, 1, idx, idx * 10, idx, idx, false, idx));
            pass.PostSortCommands.push_back(MakeCmd(CommandType::DrawMesh, dn, 1, idx, idx * 10, idx, idx, false, idx));
            ++idx;
        }
        pass.HasPreSort = true;
        pass.HasPostSort = true;
        return pass;
    }

    // A multi-pass captured frame: the scene pass (also the source, with a batched
    // post-batch stage) plus the four secondary command-bucket passes, each with
    // its own commands. The top-level lists mirror the scene/source pass.
    CapturedFrameData MakeMultiPassFrame()
    {
        CapturedFrameData frame;
        frame.FrameNumber = 7;
        frame.SourcePassName = "SceneRenderPass";

        CapturedPassData scene = MakePassEntry("SceneRenderPass", { "cube", "sphere", "plane" });
        scene.PostBatchCommands = scene.PostSortCommands; // pretend batching collapsed nothing
        scene.HasPostBatch = true;
        frame.Passes.push_back(scene);
        frame.Passes.push_back(MakePassEntry("FoliageRenderPass", { "grass" }));
        frame.Passes.push_back(MakePassEntry("WaterRenderPass", { "waterA", "waterB" }));
        frame.Passes.push_back(MakePassEntry("DecalRenderPass", { "decal" }));
        frame.Passes.push_back(MakePassEntry("ForwardOverlayPass", { "skybox", "grid" }));

        // Top-level (legacy) view = the source/scene pass's lists.
        frame.PreSortCommands = frame.Passes[0].PreSortCommands;
        frame.PostSortCommands = frame.Passes[0].PostSortCommands;
        frame.PostBatchCommands = frame.Passes[0].PostBatchCommands;
        return frame;
    }

    // Attribution for the multi-pass frame: a shadow pass (no bucket), the five
    // command-bucket passes (all non-culled, so all RAN), and a final composite
    // (no bucket). Mirrors a frame where every command-bucket pass executed.
    GraphAttribution MakeMultiPassAttribution()
    {
        GraphAttribution attr;
        attr.CaptureSourcePass = "SceneRenderPass";
        attr.Passes.push_back(GraphPassInfo{ "ShadowPass", "Graphics", false, false, false, 0 });
        attr.Passes.push_back(GraphPassInfo{ "SceneRenderPass", "Graphics", true, false, false, 1 });
        attr.Passes.push_back(GraphPassInfo{ "FoliageRenderPass", "Graphics", true, false, false, 2 });
        attr.Passes.push_back(GraphPassInfo{ "WaterRenderPass", "Graphics", true, false, false, 3 });
        attr.Passes.push_back(GraphPassInfo{ "DecalRenderPass", "Graphics", true, false, false, 4 });
        attr.Passes.push_back(GraphPassInfo{ "ForwardOverlayPass", "Graphics", true, false, false, 5 });
        attr.Passes.push_back(GraphPassInfo{ "CompositePass", "Graphics", false, false, true, 6 });
        return attr;
    }
} // namespace

TEST(McpFrameBreakdown, PerPassBreakdownListsEveryCapturedPassWithCommands)
{
    const CapturedFrameData frame = MakeMultiPassFrame();

    // No attribution needed: passBreakdowns is driven purely by the captured frame.
    const Json j = BuildBreakdown(frame, ViewMode::PostBatch, 200);

    ASSERT_TRUE(j.contains("passBreakdowns"));
    const Json& passes = j["passBreakdowns"];
    ASSERT_EQ(5u, passes.size());

    // Order preserved (execution order); each tagged with its pass name + commands.
    EXPECT_EQ("SceneRenderPass", passes[0]["name"].get<std::string>());
    EXPECT_TRUE(passes[0]["isCaptureSource"].get<bool>());
    EXPECT_EQ(3u, passes[0]["commandCount"].get<sizet>());
    // Scene batched -> the post-batch stage is what's listed.
    EXPECT_EQ("postbatch", passes[0]["viewMode"].get<std::string>());

    EXPECT_EQ("FoliageRenderPass", passes[1]["name"].get<std::string>());
    EXPECT_FALSE(passes[1]["isCaptureSource"].get<bool>());
    EXPECT_EQ(1u, passes[1]["commandCount"].get<sizet>());
    // Secondary passes only sort (no batch) -> post-batch empty, falls back to post-sort.
    EXPECT_EQ("postsort", passes[1]["viewMode"].get<std::string>());

    EXPECT_EQ("WaterRenderPass", passes[2]["name"].get<std::string>());
    EXPECT_EQ(2u, passes[2]["commandCount"].get<sizet>());
    EXPECT_EQ("waterA", passes[2]["commands"][0]["debugName"].get<std::string>());
    EXPECT_EQ("waterB", passes[2]["commands"][1]["debugName"].get<std::string>());

    EXPECT_EQ("DecalRenderPass", passes[3]["name"].get<std::string>());
    EXPECT_EQ(1u, passes[3]["commandCount"].get<sizet>());

    EXPECT_EQ("ForwardOverlayPass", passes[4]["name"].get<std::string>());
    EXPECT_EQ(2u, passes[4]["commandCount"].get<sizet>());

    // Top-level view is unchanged (the scene/source pass's bucket).
    EXPECT_EQ("SceneRenderPass", j["sourcePass"].get<std::string>());
    EXPECT_EQ(3u, j["commandCount"].get<sizet>());
}

TEST(McpFrameBreakdown, CapturedPassCountEqualsCommandBucketPassCountForFullFrame)
{
    const CapturedFrameData frame = MakeMultiPassFrame();
    const GraphAttribution attr = MakeMultiPassAttribution();

    const Json j = BuildBreakdown(frame, ViewMode::PostBatch, 200, &attr);

    ASSERT_TRUE(j.contains("graphAttribution"));
    const Json& g = j["graphAttribution"];

    // Every command-bucket pass ran and was captured — the headline invariant.
    EXPECT_EQ(5u, g["commandBucketPassCount"].get<u32>());
    EXPECT_EQ(5u, g["capturedPassCount"].get<u32>());
    EXPECT_EQ(g["commandBucketPassCount"].get<u32>(), g["capturedPassCount"].get<u32>());

    // Each graph pass is flagged captured iff it has a per-pass capture entry.
    const Json& passes = g["passes"];
    ASSERT_EQ(7u, passes.size());
    for (const auto& p : passes)
    {
        const bool usesBucket = p["usesCommandBucket"].get<bool>();
        EXPECT_EQ(usesBucket, p["captured"].get<bool>())
            << "pass " << p["name"].get<std::string>() << " captured flag should match bucket ownership";
    }

    // The shadow/composite passes own no bucket and aren't captured.
    EXPECT_FALSE(passes[0]["captured"].get<bool>()); // ShadowPass
    EXPECT_FALSE(passes[6]["captured"].get<bool>()); // CompositePass
}

TEST(McpFrameBreakdown, CulledCommandBucketPassIsExcludedFromRunningCount)
{
    // A frame that captured Scene + Water (2 passes), with a graph where a third
    // bucket pass (Foliage) was culled. The running command-bucket count excludes
    // the culled pass, so it still matches the captured count.
    CapturedFrameData frame;
    frame.SourcePassName = "SceneRenderPass";
    frame.Passes.push_back(MakePassEntry("SceneRenderPass", { "cube" }));
    frame.Passes.push_back(MakePassEntry("WaterRenderPass", { "waterA" }));
    frame.PreSortCommands = frame.Passes[0].PreSortCommands;
    frame.PostSortCommands = frame.Passes[0].PostSortCommands;

    GraphAttribution attr;
    attr.CaptureSourcePass = "SceneRenderPass";
    attr.Passes.push_back(GraphPassInfo{ "SceneRenderPass", "Graphics", true, false, false, 0 });
    attr.Passes.push_back(GraphPassInfo{ "WaterRenderPass", "Graphics", true, false, false, 1 });
    attr.Passes.push_back(GraphPassInfo{ "FoliageRenderPass", "Graphics", true, /*culled*/ true, false, -1 });

    const Json j = BuildBreakdown(frame, ViewMode::PostSort, 200, &attr);
    const Json& g = j["graphAttribution"];

    EXPECT_EQ(2u, g["commandBucketPassCount"].get<u32>()); // Foliage culled -> excluded
    EXPECT_EQ(2u, g["capturedPassCount"].get<u32>());

    // The culled foliage pass is listed but flagged not-captured.
    const Json& passes = g["passes"];
    EXPECT_TRUE(passes[2]["culled"].get<bool>());
    EXPECT_FALSE(passes[2]["captured"].get<bool>());
}
