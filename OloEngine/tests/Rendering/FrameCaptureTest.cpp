#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"
#include "OloEngine/Renderer/Debug/CommandPacketDebugger.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// CapturedCommandData Construction and Typed Access
// =============================================================================

TEST(CapturedCommandData, ConstructionPreservesFields)
{
    auto cmd = MakeSyntheticDrawMeshCommand(10, 20, 0.5f, 42);
    DrawKey key = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 10, 20, 500);

    CapturedCommandData captured(
        CommandType::DrawMesh,
        &cmd, sizeof(cmd),
        key,
        /*groupID=*/3, /*executionOrder=*/7,
        /*isStatic=*/true, /*dependsOnPrevious=*/false,
        "TestDrawMesh",
        /*originalIndex=*/0);

    EXPECT_EQ(captured.GetCommandType(), CommandType::DrawMesh);
    EXPECT_STREQ(captured.GetCommandTypeString(), "DrawMesh");
    EXPECT_EQ(captured.GetGroupID(), 3u);
    EXPECT_EQ(captured.GetExecutionOrder(), 7u);
    EXPECT_TRUE(captured.IsStatic());
    EXPECT_FALSE(captured.DependsOnPrevious());
    EXPECT_EQ(captured.GetDebugName(), "TestDrawMesh");
    EXPECT_EQ(captured.GetSortKey().GetKey(), key.GetKey());
    EXPECT_EQ(captured.GetDataSize(), sizeof(DrawMeshCommand));
}

TEST(CapturedCommandData, TypedAccessRoundTrip)
{
    auto cmd = MakeSyntheticDrawMeshCommand(42, 99, 1.5f, 7);
    DrawKey key = MakeSyntheticOpaqueKey();

    CapturedCommandData captured(
        CommandType::DrawMesh,
        &cmd, sizeof(cmd),
        key, 0, 0, false, false, nullptr, 0);

    const auto* retrieved = captured.GetCommandData<DrawMeshCommand>();
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->shaderRendererID, 42u);
    EXPECT_EQ(retrieved->entityID, 7);
    EXPECT_EQ(retrieved->indexCount, 36u);
}

TEST(CapturedCommandData, NullDataHandled)
{
    CapturedCommandData captured(
        CommandType::Invalid,
        nullptr, 0,
        DrawKey(), 0, 0, false, false, nullptr, 0);

    EXPECT_EQ(captured.GetRawData(), nullptr);
    EXPECT_EQ(captured.GetDataSize(), 0u);
    EXPECT_EQ(captured.GetCommandData<DrawMeshCommand>(), nullptr);
}

// =============================================================================
// Command Classification
// =============================================================================

TEST(CapturedCommandData, IsDrawCommand)
{
    auto isDrawTypeHelper = [](CommandType type)
    {
        CapturedCommandData c(type, nullptr, 0, DrawKey(), 0, 0, false, false, nullptr, 0);
        return c.IsDrawCommand();
    };

    EXPECT_TRUE(isDrawTypeHelper(CommandType::DrawMesh));
    EXPECT_TRUE(isDrawTypeHelper(CommandType::DrawMeshInstanced));
    EXPECT_TRUE(isDrawTypeHelper(CommandType::DrawQuad));
    EXPECT_TRUE(isDrawTypeHelper(CommandType::DrawIndexed));
    EXPECT_TRUE(isDrawTypeHelper(CommandType::DrawArrays));
    EXPECT_TRUE(isDrawTypeHelper(CommandType::DrawLines));
    EXPECT_TRUE(isDrawTypeHelper(CommandType::DrawSkybox));
    EXPECT_TRUE(isDrawTypeHelper(CommandType::DrawInfiniteGrid));
    EXPECT_TRUE(isDrawTypeHelper(CommandType::DrawIndexedInstanced));

    EXPECT_FALSE(isDrawTypeHelper(CommandType::Invalid));
    EXPECT_FALSE(isDrawTypeHelper(CommandType::Clear));
    EXPECT_FALSE(isDrawTypeHelper(CommandType::SetViewport));
    EXPECT_FALSE(isDrawTypeHelper(CommandType::BindTexture));
}

TEST(CapturedCommandData, IsStateCommand)
{
    auto isStateTypeHelper = [](CommandType type)
    {
        CapturedCommandData c(type, nullptr, 0, DrawKey(), 0, 0, false, false, nullptr, 0);
        return c.IsStateCommand();
    };

    EXPECT_TRUE(isStateTypeHelper(CommandType::SetViewport));
    EXPECT_TRUE(isStateTypeHelper(CommandType::SetClearColor));
    EXPECT_TRUE(isStateTypeHelper(CommandType::SetBlendState));
    EXPECT_TRUE(isStateTypeHelper(CommandType::SetDepthTest));
    EXPECT_TRUE(isStateTypeHelper(CommandType::SetDepthMask));
    EXPECT_TRUE(isStateTypeHelper(CommandType::SetCulling));
    EXPECT_TRUE(isStateTypeHelper(CommandType::SetPolygonMode));
    EXPECT_TRUE(isStateTypeHelper(CommandType::SetScissorTest));
    EXPECT_TRUE(isStateTypeHelper(CommandType::SetColorMask));
    EXPECT_TRUE(isStateTypeHelper(CommandType::SetMultisampling));

    EXPECT_FALSE(isStateTypeHelper(CommandType::DrawMesh));
    EXPECT_FALSE(isStateTypeHelper(CommandType::BindTexture));
    EXPECT_FALSE(isStateTypeHelper(CommandType::Invalid));
}

TEST(CapturedCommandData, IsBindCommand)
{
    auto isBindTypeHelper = [](CommandType type)
    {
        CapturedCommandData c(type, nullptr, 0, DrawKey(), 0, 0, false, false, nullptr, 0);
        return c.IsBindCommand();
    };

    EXPECT_TRUE(isBindTypeHelper(CommandType::BindTexture));
    EXPECT_TRUE(isBindTypeHelper(CommandType::BindDefaultFramebuffer));
    EXPECT_TRUE(isBindTypeHelper(CommandType::SetShaderResource));

    EXPECT_FALSE(isBindTypeHelper(CommandType::DrawMesh));
    EXPECT_FALSE(isBindTypeHelper(CommandType::SetViewport));
    EXPECT_FALSE(isBindTypeHelper(CommandType::Clear));
}

// =============================================================================
// GPU Timing
// =============================================================================

TEST(CapturedCommandData, GpuTimingAccessors)
{
    CapturedCommandData captured(
        CommandType::DrawMesh,
        nullptr, 0,
        DrawKey(), 0, 0, false, false, nullptr, 0);

    EXPECT_EQ(captured.GetGpuTimeMs(), 0.0);

    captured.SetGpuTimeMs(1.234);
    EXPECT_DOUBLE_EQ(captured.GetGpuTimeMs(), 1.234);
}

// =============================================================================
// Deep Copy Independence
// =============================================================================

TEST(CapturedCommandData, CopyIsIndependent)
{
    auto cmd = MakeSyntheticDrawMeshCommand(5, 5, 0.5f, 1);
    CapturedCommandData original(
        CommandType::DrawMesh,
        &cmd, sizeof(cmd),
        MakeSyntheticOpaqueKey(), 1, 0, false, false, "Original", 0);

    CapturedCommandData copy = original; // NOLINT(performance-unnecessary-copy-initialization)

    EXPECT_EQ(copy.GetCommandType(), original.GetCommandType());
    EXPECT_EQ(copy.GetDataSize(), original.GetDataSize());
    EXPECT_EQ(copy.GetDebugName(), original.GetDebugName());

    // Modify copy's GPU time — shouldn't affect original
    copy.SetGpuTimeMs(99.9);
    EXPECT_EQ(original.GetGpuTimeMs(), 0.0);
    EXPECT_DOUBLE_EQ(copy.GetGpuTimeMs(), 99.9);
}

// =============================================================================
// FrameCaptureStats Default Values
// =============================================================================

TEST(FrameCaptureStats, DefaultsAreZero)
{
    FrameCaptureStats stats;
    EXPECT_EQ(stats.TotalCommands, 0u);
    EXPECT_EQ(stats.BatchedCommands, 0u);
    EXPECT_EQ(stats.DrawCalls, 0u);
    EXPECT_EQ(stats.StateChanges, 0u);
    EXPECT_EQ(stats.ShaderBinds, 0u);
    EXPECT_EQ(stats.TextureBinds, 0u);
    EXPECT_DOUBLE_EQ(stats.SortTimeMs, 0.0);
    EXPECT_DOUBLE_EQ(stats.BatchTimeMs, 0.0);
    EXPECT_DOUBLE_EQ(stats.ExecuteTimeMs, 0.0);
    EXPECT_DOUBLE_EQ(stats.TotalFrameTimeMs, 0.0);
}

// =============================================================================
// CapturedFrameData Structure
// =============================================================================

TEST(CapturedFrameData, DefaulConstruction)
{
    CapturedFrameData frame;
    EXPECT_EQ(frame.FrameNumber, 0u);
    EXPECT_DOUBLE_EQ(frame.TimestampSeconds, 0.0);
    EXPECT_TRUE(frame.PreSortCommands.empty());
    EXPECT_TRUE(frame.PostSortCommands.empty());
    EXPECT_TRUE(frame.PostBatchCommands.empty());
    EXPECT_TRUE(frame.Notes.empty());
}

TEST(CapturedFrameData, CanStoreCommandsAtMultipleStages)
{
    CapturedFrameData frame;
    frame.FrameNumber = 42;

    // Add pre-sort commands
    auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.5f);
    frame.PreSortCommands.emplace_back(
        CommandType::DrawMesh, &cmd, sizeof(cmd),
        MakeSyntheticOpaqueKey(), 0, 0, false, false, nullptr, 0);
    frame.PreSortCommands.emplace_back(
        CommandType::DrawMesh, &cmd, sizeof(cmd),
        MakeSyntheticOpaqueKey(), 0, 1, false, false, nullptr, 1);

    // Add post-sort commands (same data, different order)
    frame.PostSortCommands = frame.PreSortCommands;

    // Stats
    frame.Stats.TotalCommands = 2;
    frame.Stats.DrawCalls = 2;

    EXPECT_EQ(frame.PreSortCommands.size(), 2u);
    EXPECT_EQ(frame.PostSortCommands.size(), 2u);
    EXPECT_EQ(frame.Stats.TotalCommands, 2u);
}

// =============================================================================
// FrameCaptureManager Singleton and State Machine
// =============================================================================

TEST(FrameCaptureManager, InitialStateIsIdle)
{
    auto& mgr = FrameCaptureManager::GetInstance();
    // Ensure clean state (may have residue from other tests)
    mgr.StopRecording();
    mgr.ClearCaptures();

    EXPECT_EQ(mgr.GetState(), CaptureState::Idle);
    EXPECT_FALSE(mgr.IsCapturing());
}

TEST(FrameCaptureManager, CaptureNextFrameTransition)
{
    auto& mgr = FrameCaptureManager::GetInstance();
    mgr.StopRecording();

    mgr.CaptureNextFrame();
    EXPECT_EQ(mgr.GetState(), CaptureState::CaptureNextFrame);
    EXPECT_TRUE(mgr.IsCapturing());

    // Cleanup
    mgr.StopRecording();
}

TEST(FrameCaptureManager, StartStopRecording)
{
    auto& mgr = FrameCaptureManager::GetInstance();

    mgr.StartRecording();
    EXPECT_EQ(mgr.GetState(), CaptureState::Recording);
    EXPECT_TRUE(mgr.IsCapturing());

    mgr.StopRecording();
    EXPECT_EQ(mgr.GetState(), CaptureState::Idle);
    EXPECT_FALSE(mgr.IsCapturing());
}

TEST(FrameCaptureManager, MaxCapturedFramesConfig)
{
    auto& mgr = FrameCaptureManager::GetInstance();

    mgr.SetMaxCapturedFrames(120);
    EXPECT_EQ(mgr.GetMaxCapturedFrames(), 120u);

    mgr.SetMaxCapturedFrames(60); // Reset to default
}

TEST(FrameCaptureManager, ClearCaptures)
{
    auto& mgr = FrameCaptureManager::GetInstance();
    mgr.ClearCaptures();

    EXPECT_EQ(mgr.GetCapturedFrameCount(), 0u);
    auto frames = mgr.GetCapturedFramesCopy();
    EXPECT_TRUE(frames.empty());
}

TEST(FrameCaptureManager, SelectedFrameIndex)
{
    auto& mgr = FrameCaptureManager::GetInstance();

    mgr.SetSelectedFrameIndex(5);
    EXPECT_EQ(mgr.GetSelectedFrameIndex(), 5);

    mgr.SetSelectedFrameIndex(-1);
    EXPECT_EQ(mgr.GetSelectedFrameIndex(), -1);
}

TEST(FrameCaptureManager, GetSelectedFrameReturnsNulloptWhenEmpty)
{
    auto& mgr = FrameCaptureManager::GetInstance();
    mgr.ClearCaptures();
    mgr.SetSelectedFrameIndex(0);

    auto frame = mgr.GetSelectedFrame();
    EXPECT_FALSE(frame.has_value());
}

// =============================================================================
// Full Capture Pipeline — drive a real CommandBucket through the capture hooks
// =============================================================================

class FrameCapturePipelineTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Allocator = std::make_unique<CommandAllocator>();

        // Ensure clean singleton state
        auto& mgr = FrameCaptureManager::GetInstance();
        mgr.StopRecording();
        mgr.ClearCaptures();
        mgr.SetSelectedFrameIndex(-1);
    }

    void TearDown() override
    {
        auto& mgr = FrameCaptureManager::GetInstance();
        mgr.StopRecording();
        mgr.ClearCaptures();
        mgr.SetSelectedFrameIndex(-1);
        m_Allocator.reset();
    }

    /// Build a bucket with N draw commands + 1 state command, sort it, return it.
    CommandBucket MakeTestBucket(u32 drawCount = 5)
    {
        CommandBucketConfig config;
        config.EnableSorting = true;
        config.EnableBatching = false;
        CommandBucket bucket(config);

        for (u32 i = 0; i < drawCount; ++i)
        {
            auto cmd = MakeSyntheticDrawMeshCommand(i % 3 + 1, i % 2 + 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
            PacketMetadata meta;
            meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, i % 3 + 1, i % 2 + 1, i * 100);
            bucket.Submit(cmd, meta, m_Allocator.get());
        }

        // Add a state command to verify classification in captured data
        auto clear = MakeSyntheticClearCommand();
        bucket.Submit(clear, {}, m_Allocator.get());

        return bucket;
    }

    std::unique_ptr<CommandAllocator> m_Allocator;
};

TEST_F(FrameCapturePipelineTest, CaptureNextFrameRecordsSingleFrame)
{
    auto& mgr = FrameCaptureManager::GetInstance();

    auto bucket = MakeTestBucket(5);
    bucket.SortCommands();

    // Trigger single-frame capture
    mgr.CaptureNextFrame();
    ASSERT_EQ(mgr.GetState(), CaptureState::CaptureNextFrame);

    // Drive the pipeline hooks
    mgr.OnPreSort(bucket);
    mgr.OnPostSort(bucket);
    mgr.OnFrameEnd(/*frameNumber=*/1, /*sortMs=*/0.5, /*batchMs=*/0.0, /*execMs=*/1.0);

    // Should have returned to Idle after single-frame capture
    EXPECT_EQ(mgr.GetState(), CaptureState::Idle);

    // Should have captured exactly one frame
    EXPECT_EQ(mgr.GetCapturedFrameCount(), 1u);

    auto frames = mgr.GetCapturedFramesCopy();
    ASSERT_EQ(frames.size(), 1u);

    const auto& frame = frames[0];
    EXPECT_EQ(frame.FrameNumber, 1u);
    EXPECT_GT(frame.TimestampSeconds, 0.0);

    // Pre-sort commands should be in submission order (linked list traversal)
    EXPECT_EQ(frame.PreSortCommands.size(), 6u); // 5 draw + 1 clear

    // Post-sort commands should be present
    EXPECT_EQ(frame.PostSortCommands.size(), 6u);

    // Stats should be populated
    EXPECT_EQ(frame.Stats.TotalCommands, 6u);
    EXPECT_DOUBLE_EQ(frame.Stats.SortTimeMs, 0.5);
    EXPECT_DOUBLE_EQ(frame.Stats.ExecuteTimeMs, 1.0);
    EXPECT_DOUBLE_EQ(frame.Stats.TotalFrameTimeMs, 0.5 + 0.0 + 1.0);
}

TEST_F(FrameCapturePipelineTest, CapturedCommandsPreserveTypes)
{
    auto& mgr = FrameCaptureManager::GetInstance();

    auto bucket = MakeTestBucket(3);
    bucket.SortCommands();

    mgr.CaptureNextFrame();
    mgr.OnPreSort(bucket);
    mgr.OnPostSort(bucket);
    mgr.OnFrameEnd(42, 0.1, 0.0, 0.2);

    auto frames = mgr.GetCapturedFramesCopy();
    ASSERT_EQ(frames.size(), 1u);

    const auto& preSortCmds = frames[0].PreSortCommands;

    // Count draw vs state commands
    u32 drawCount = 0;
    u32 stateCount = 0;
    for (const auto& cmd : preSortCmds)
    {
        if (cmd.IsDrawCommand())
            drawCount++;
        if (cmd.IsStateCommand())
            stateCount++;
    }

    // We submitted 3 draw + 1 clear (Clear is a state command in IsStateCommand?
    // Actually Clear is CommandType::Clear — not in IsStateCommand(). Let's check.
    // IsStateCommand returns true for SetViewport, SetClearColor, etc. — not Clear.
    // IsDrawCommand returns true for DrawMesh, etc. — not Clear.
    // Clear is neither draw nor state. It's its own thing.
    EXPECT_EQ(drawCount, 3u);

    // Verify typed access round-trips for at least one draw command
    bool foundDraw = false;
    for (const auto& cmd : preSortCmds)
    {
        if (cmd.GetCommandType() == CommandType::DrawMesh)
        {
            const auto* meshCmd = cmd.GetCommandData<DrawMeshCommand>();
            ASSERT_NE(meshCmd, nullptr);
            EXPECT_EQ(meshCmd->indexCount, 36u);
            foundDraw = true;
            break;
        }
    }
    EXPECT_TRUE(foundDraw) << "Should find at least one DrawMesh in captured data";
}

TEST_F(FrameCapturePipelineTest, RecordingCapturesMultipleFrames)
{
    auto& mgr = FrameCaptureManager::GetInstance();

    mgr.StartRecording();
    ASSERT_EQ(mgr.GetState(), CaptureState::Recording);

    constexpr u32 NUM_FRAMES = 5;
    for (u32 f = 0; f < NUM_FRAMES; ++f)
    {
        auto bucket = MakeTestBucket(3);
        bucket.SortCommands();

        mgr.OnPreSort(bucket);
        mgr.OnPostSort(bucket);
        mgr.OnFrameEnd(f + 1, 0.1, 0.0, 0.2);
    }

    mgr.StopRecording();
    EXPECT_EQ(mgr.GetState(), CaptureState::Idle);
    EXPECT_EQ(mgr.GetCapturedFrameCount(), NUM_FRAMES);

    auto frames = mgr.GetCapturedFramesCopy();
    for (u32 f = 0; f < NUM_FRAMES; ++f)
    {
        EXPECT_EQ(frames[f].FrameNumber, f + 1);
        EXPECT_EQ(frames[f].PreSortCommands.size(), 4u); // 3 draw + 1 clear
    }
}

TEST_F(FrameCapturePipelineTest, MaxCapturedFramesTrimsOldest)
{
    auto& mgr = FrameCaptureManager::GetInstance();
    mgr.SetMaxCapturedFrames(3);

    mgr.StartRecording();

    for (u32 f = 0; f < 10; ++f)
    {
        auto bucket = MakeTestBucket(2);
        bucket.SortCommands();
        mgr.OnPreSort(bucket);
        mgr.OnPostSort(bucket);
        mgr.OnFrameEnd(f + 1, 0.1, 0.0, 0.2);
    }

    mgr.StopRecording();

    // Should only keep the last 3 frames
    EXPECT_EQ(mgr.GetCapturedFrameCount(), 3u);

    auto frames = mgr.GetCapturedFramesCopy();
    EXPECT_EQ(frames[0].FrameNumber, 8u);
    EXPECT_EQ(frames[1].FrameNumber, 9u);
    EXPECT_EQ(frames[2].FrameNumber, 10u);

    mgr.SetMaxCapturedFrames(60); // Restore default
}

TEST_F(FrameCapturePipelineTest, PostSortOrderDiffersFromPreSort)
{
    auto& mgr = FrameCaptureManager::GetInstance();

    // Create a bucket with commands that will be reordered by sort
    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    // Submit commands with different shaders to force reordering
    for (u32 i = 0; i < 10; ++i)
    {
        u32 shader = (i % 3) + 1; // Shader 1, 2, 3, 1, 2, 3, ...
        auto cmd = MakeSyntheticDrawMeshCommand(shader, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, shader, 1, i * 100);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    mgr.CaptureNextFrame();
    mgr.OnPreSort(bucket);

    bucket.SortCommands();
    mgr.OnPostSort(bucket);

    mgr.OnFrameEnd(1, 0.1, 0.0, 0.2);

    auto frames = mgr.GetCapturedFramesCopy();
    ASSERT_EQ(frames.size(), 1u);

    const auto& pre = frames[0].PreSortCommands;
    const auto& post = frames[0].PostSortCommands;

    EXPECT_EQ(pre.size(), post.size());

    // Post-sort should be in ascending DrawKey order
    for (sizet i = 1; i < post.size(); ++i)
    {
        EXPECT_LE(post[i - 1].GetSortKey().GetKey(), post[i].GetSortKey().GetKey())
            << "Post-sort commands should be in ascending DrawKey order at index " << i;
    }

    // Pre-sort and post-sort shouldn't be in the same order (unless by luck)
    // since we interleaved shaders
    bool orderDiffers = false;
    for (sizet i = 0; i < pre.size(); ++i)
    {
        if (pre[i].GetSortKey().GetKey() != post[i].GetSortKey().GetKey())
        {
            orderDiffers = true;
            break;
        }
    }
    EXPECT_TRUE(orderDiffers) << "Sort should reorder interleaved shader commands";
}

TEST_F(FrameCapturePipelineTest, DrawCallAndStateChangeStats)
{
    auto& mgr = FrameCaptureManager::GetInstance();

    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);

    // Submit 3 draw commands
    for (u32 i = 0; i < 3; ++i)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, static_cast<f32>(i) * 0.1f, static_cast<i32>(i));
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, i * 100);
        bucket.Submit(cmd, meta, m_Allocator.get());
    }

    // Submit 2 state commands
    auto viewport = MakeSyntheticViewportCommand();
    auto depth = MakeSyntheticDepthTestCommand();
    bucket.Submit(viewport, {}, m_Allocator.get());
    bucket.Submit(depth, {}, m_Allocator.get());

    bucket.SortCommands();

    mgr.CaptureNextFrame();
    mgr.OnPreSort(bucket);
    mgr.OnPostSort(bucket);
    mgr.OnFrameEnd(1, 0.5, 0.0, 1.0);

    auto frames = mgr.GetCapturedFramesCopy();
    ASSERT_EQ(frames.size(), 1u);

    EXPECT_EQ(frames[0].Stats.DrawCalls, 3u);
    EXPECT_EQ(frames[0].Stats.StateChanges, 2u);
    EXPECT_EQ(frames[0].Stats.TotalCommands, 5u);
}

TEST_F(FrameCapturePipelineTest, CaptureGenerationIncrements)
{
    auto& mgr = FrameCaptureManager::GetInstance();
    u64 gen0 = mgr.GetCaptureGeneration();

    auto bucket = MakeTestBucket(1);
    bucket.SortCommands();

    mgr.CaptureNextFrame();
    mgr.OnPreSort(bucket);
    mgr.OnPostSort(bucket);
    mgr.OnFrameEnd(1, 0.1, 0.0, 0.1);

    u64 gen1 = mgr.GetCaptureGeneration();
    EXPECT_GT(gen1, gen0) << "Capture generation should increment after each captured frame";

    mgr.ClearCaptures();
    u64 gen2 = mgr.GetCaptureGeneration();
    EXPECT_GT(gen2, gen1) << "Capture generation should increment after clear";
}

// =============================================================================
// Frame Export Tests — CSV and Markdown
// =============================================================================

class FrameExportTest : public FrameCapturePipelineTest
{
  protected:
    void SetUp() override
    {
        FrameCapturePipelineTest::SetUp();

        // Determine a temp directory for test output
        m_TestOutputDir = std::filesystem::temp_directory_path() / "olo_frame_export_test";
        std::filesystem::create_directories(m_TestOutputDir);
    }

    void TearDown() override
    {
        // Clean up test files
        std::error_code ec;
        std::filesystem::remove_all(m_TestOutputDir, ec);

        FrameCapturePipelineTest::TearDown();
    }

    /// Capture a frame and select it so export functions can read it.
    void CaptureAndSelectFrame(u32 drawCount = 5)
    {
        auto& mgr = FrameCaptureManager::GetInstance();

        auto bucket = MakeTestBucket(drawCount);
        bucket.SortCommands();

        mgr.CaptureNextFrame();
        mgr.OnPreSort(bucket);
        mgr.OnPostSort(bucket);
        mgr.OnFrameEnd(/*frameNumber=*/100, /*sortMs=*/0.42, /*batchMs=*/0.0, /*execMs=*/1.23);

        // Select the captured frame
        mgr.SetSelectedFrameIndex(0);
    }

    std::filesystem::path m_TestOutputDir;
};

TEST_F(FrameExportTest, ExportToCSVCreatesValidFile)
{
    CaptureAndSelectFrame(5);

    auto csvPath = (m_TestOutputDir / "test_export.csv").string();
    auto& debugger = CommandPacketDebugger::GetInstance();

    bool success = debugger.ExportToCSV(csvPath);
    ASSERT_TRUE(success) << "ExportToCSV should succeed";

    // File should exist
    EXPECT_TRUE(std::filesystem::exists(csvPath));

    // Read and validate CSV content
    std::ifstream file(csvPath);
    ASSERT_TRUE(file.is_open());

    std::string line;

    // First line should be the header
    std::getline(file, line);
    EXPECT_NE(line.find("Index"), std::string::npos) << "CSV header should contain 'Index'";
    EXPECT_NE(line.find("Type"), std::string::npos) << "CSV header should contain 'Type'";
    EXPECT_NE(line.find("DrawKey"), std::string::npos) << "CSV header should contain 'DrawKey'";
    EXPECT_NE(line.find("ShaderID"), std::string::npos) << "CSV header should contain 'ShaderID'";

    // Count data lines
    u32 dataLines = 0;
    while (std::getline(file, line))
    {
        if (!line.empty())
            dataLines++;
    }

    // Should have at least 6 lines (5 draw + 1 clear) from the captured frame
    EXPECT_GE(dataLines, 6u) << "CSV should contain all captured commands";
}

TEST_F(FrameExportTest, ExportToMarkdownCreatesValidFile)
{
    CaptureAndSelectFrame(5);

    auto mdPath = (m_TestOutputDir / "test_export.md").string();
    auto& debugger = CommandPacketDebugger::GetInstance();

    bool success = debugger.ExportToMarkdown(mdPath);
    ASSERT_TRUE(success) << "ExportToMarkdown should succeed";

    EXPECT_TRUE(std::filesystem::exists(mdPath));

    // Read the entire file
    std::ifstream file(mdPath);
    ASSERT_TRUE(file.is_open());

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    // Should contain expected sections
    EXPECT_NE(content.find("# Command Bucket Frame Capture Report"), std::string::npos);
    EXPECT_NE(content.find("## Frame Info"), std::string::npos);
    EXPECT_NE(content.find("Frame Number"), std::string::npos);
    EXPECT_NE(content.find("## Pipeline Statistics"), std::string::npos);
    EXPECT_NE(content.find("## Command List"), std::string::npos);

    // Frame number should appear
    EXPECT_NE(content.find("100"), std::string::npos) << "Frame number should appear in report";

    // Should contain draw command type
    EXPECT_NE(content.find("DrawMesh"), std::string::npos);
}

TEST_F(FrameExportTest, ExportToCSVWithNoSelectedFrameFails)
{
    // Don't capture anything — no frame selected
    auto& mgr = FrameCaptureManager::GetInstance();
    mgr.SetSelectedFrameIndex(-1);

    auto csvPath = (m_TestOutputDir / "should_not_exist.csv").string();
    auto& debugger = CommandPacketDebugger::GetInstance();

    bool success = debugger.ExportToCSV(csvPath);
    EXPECT_FALSE(success) << "ExportToCSV should fail without a selected frame";
    EXPECT_FALSE(std::filesystem::exists(csvPath));
}

TEST_F(FrameExportTest, ExportToMarkdownWithNoSelectedFrameFails)
{
    auto& mgr = FrameCaptureManager::GetInstance();
    mgr.SetSelectedFrameIndex(-1);

    auto mdPath = (m_TestOutputDir / "should_not_exist.md").string();
    auto& debugger = CommandPacketDebugger::GetInstance();

    bool success = debugger.ExportToMarkdown(mdPath);
    EXPECT_FALSE(success) << "ExportToMarkdown should fail without a selected frame";
    EXPECT_FALSE(std::filesystem::exists(mdPath));
}

TEST_F(FrameExportTest, GenerateExportFilenameContainsFrameNumber)
{
    std::string filename = CommandPacketDebugger::GenerateExportFilename("csv", 42);

    EXPECT_NE(filename.find("42"), std::string::npos) << "Filename should contain frame number";
    EXPECT_NE(filename.find(".csv"), std::string::npos) << "Filename should have correct extension";
    EXPECT_NE(filename.find("cmd_bucket_frame"), std::string::npos) << "Filename should have standard prefix";
}

TEST_F(FrameExportTest, CSVContainsCorrectSortKeyData)
{
    CaptureAndSelectFrame(3);

    auto csvPath = (m_TestOutputDir / "key_check.csv").string();
    auto& debugger = CommandPacketDebugger::GetInstance();

    bool success = debugger.ExportToCSV(csvPath);
    ASSERT_TRUE(success);

    std::ifstream file(csvPath);
    std::string header;
    std::getline(file, header); // Skip header

    // Read first data line and verify DrawKey fields are present
    std::string firstDataLine;
    std::getline(file, firstDataLine);

    // Should contain hex DrawKey value
    EXPECT_NE(firstDataLine.find("0x"), std::string::npos)
        << "CSV data should contain DrawKey in hex format";

    // Should contain ViewLayer string (e.g., "3D")
    // The commands use ViewLayerType::ThreeD which ToString() returns "3D"
    EXPECT_NE(firstDataLine.find("3D"), std::string::npos)
        << "CSV should contain ViewLayer type";

    // Should contain RenderMode string
    EXPECT_NE(firstDataLine.find("Opaque"), std::string::npos)
        << "CSV should contain RenderMode type for opaque commands";
}

TEST_F(FrameExportTest, MarkdownSortAnalysisPresent)
{
    CaptureAndSelectFrame(10); // More commands for interesting sort analysis

    auto mdPath = (m_TestOutputDir / "sort_analysis.md").string();
    auto& debugger = CommandPacketDebugger::GetInstance();

    bool success = debugger.ExportToMarkdown(mdPath);
    ASSERT_TRUE(success);

    std::ifstream file(mdPath);
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    // Sort analysis section should exist
    EXPECT_NE(content.find("## Sort Analysis"), std::string::npos);
    EXPECT_NE(content.find("Commands moved"), std::string::npos);

    // State change analysis section should exist
    EXPECT_NE(content.find("## State Change Analysis"), std::string::npos);
    EXPECT_NE(content.find("Shader Coherence"), std::string::npos);
}
