// OLO_TEST_LAYER: plumbing
#include "OloEnginePCH.h"
#include "MockRendererAPI.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/FrontendRecordingContext.h"

#include <gtest/gtest.h>
#include <array>
#include <barrier>
#include <cstddef>
#include <thread>
#include <vector>

using namespace OloEngine;          // NOLINT(google-build-using-namespace)
using namespace OloEngine::Testing; // NOLINT(google-build-using-namespace)

class CommandDispatchRecording : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        CommandDispatch::Initialize();
        auto& profiler = RendererProfiler::GetInstance();
        profiler.Initialize();
        profiler.BeginFrame();
        profiler.SetRecordInstancedDraws(true);
    }

    void TearDown() override
    {
        // Undo the FLAG this fixture set, and nothing else.
        //
        // RendererProfiler and CommandDispatch are process-wide singletons this
        // fixture did not create, and the whole suite runs in one process
        // (gtest_discover_tests gives CI a process per case, so CI never sees
        // what this costs). Shutting them down here left the profiler's frame
        // history EMPTY — the next test to render a frame subscripted an empty
        // vector inside RendererProfiler::EndFrame — and dropped the
        // dispatcher's UBO references, which Renderer3D publishes once at
        // Init() and cannot be asked to publish again. Every later rendering
        // test then drew with no camera/material UBO bound, so ~100 visual
        // evidence assertions failed with the feature-on and feature-off
        // captures identical.
        RendererProfiler::GetInstance().SetRecordInstancedDraws(false);
    }
};

TEST_F(CommandDispatchRecording, RecordingContextsIsolateCachesViewsAndProfilerWritesAcrossRealThreads)
{
    CommandDispatch::SetViewPosition(glm::vec3(99.0f));
    auto& profiler = RendererProfiler::GetInstance();
    constexpr u32 itemCount = 8;
    std::array<FrontendRecordingContext, itemCount> items;
    std::array<MockRendererAPI, itemCount> apis;
    for (auto& item : items)
        item.Prepare(1);
    std::barrier start(static_cast<std::ptrdiff_t>(itemCount));
    std::vector<std::jthread> threads;
    for (u32 item = 0; item < itemCount; ++item)
    {
        threads.emplace_back([&, item]
                             {
            const ScopedFrontendRecordingContext scope(items[item]);
            CommandDispatch::SetViewPosition(glm::vec3(static_cast<f32>(item)));
            start.arrive_and_wait();
            EXPECT_NEAR(CommandDispatch::GetViewPosition().x, static_cast<f32>(item), 0.001f);
            DrawIndexedCommand draw{};
            draw.vertexArrayID = RHI::ResourceHandle{ 100u, 1u };
            draw.indexCount = 3;
            CommandDispatch::DrawIndexed(&draw, apis[item]);
            CommandDispatch::DrawIndexed(&draw, apis[item]);
            CommandDispatch::GetStatistics().DrawCalls += 2;
            profiler.IncrementCounter(RendererProfiler::MetricType::InstancesRendered, item + 1u);
            const i32 entity = static_cast<i32>(item);
            profiler.RecordInstancedDraw(item, 100, 3, 1, &entity, false); });
    }
    threads.clear();
    EXPECT_NEAR(CommandDispatch::GetViewPosition().x, 99.0f, 0.001f);
    EXPECT_EQ(CommandDispatch::GetStatistics().DrawCalls, 0u);
    EXPECT_TRUE(profiler.GetInstancedDrawRecords().IsEmpty());
    for (u32 item = 0; item < itemCount; ++item)
    {
        EXPECT_EQ(apis[item].CountCalls("BindVertexArrayRaw"), 1u);
        items[item].Publish();
    }
    EXPECT_EQ(CommandDispatch::GetStatistics().DrawCalls, itemCount * 2u);
    EXPECT_EQ(profiler.GetCurrentFrameData().m_InstancesRendered, itemCount * (itemCount + 1u) / 2u);
    ASSERT_EQ(profiler.GetInstancedDrawRecords().Num(), itemCount);
    for (u32 item = 0; item < itemCount; ++item)
        EXPECT_EQ(profiler.GetInstancedDrawRecords()[item].m_MeshHandle, item);

    // Reusing an item on the caller must start with a fresh bind cache and
    // retain the caller's view and statistics after the scope ends.
    items[0].Prepare(1);
    {
        const ScopedFrontendRecordingContext scope(items[0]);
        EXPECT_EQ(CommandDispatch::GetStatistics().DrawCalls, 0u);
        DrawIndexedCommand draw{};
        draw.vertexArrayID = RHI::ResourceHandle{ 100u, 1u };
        draw.indexCount = 3;
        CommandDispatch::DrawIndexed(&draw, apis[0]);
    }
    EXPECT_EQ(apis[0].CountCalls("BindVertexArrayRaw"), 2u);
    EXPECT_EQ(CommandDispatch::GetStatistics().DrawCalls, itemCount * 2u);
}

TEST(CommandDispatchProfilerStorage, CapturedFramesPreserveNestedTelemetryAcrossGrowthAndRemoval)
{
    TArray<RendererProfiler::CapturedFrame> frames;
    for (u32 frameIndex = 0; frameIndex < 64; ++frameIndex)
    {
        RendererProfiler::CapturedFrame frame;
        frame.m_FrameNumber = frameIndex;
        frame.m_Notes = std::string("frame\0note", 10);
        frame.m_BottleneckAnalysis.m_Type = RendererProfiler::BottleneckInfo::Balanced;
        frame.m_BottleneckAnalysis.m_Recommendations.Add("reduce draw calls");
        RendererAPI::ParallelRecordingRegionStats region;
        region.PassName = "main";
        region.ItemRecordMs = { 1.5, 2.5 };
        region.ItemPassNames = { "first", "second" };
        frame.m_FrameData.m_ParallelRecording.RegionTimings.Add(std::move(region));
        RendererProfiler::RenderPassInfo pass;
        pass.m_Name = "scene";
        RendererProfiler::DrawCallInfo draw;
        draw.m_Name = "mesh";
        draw.m_VertexCount = frameIndex + 3;
        pass.m_DrawCalls.Add(std::move(draw));
        frame.m_RenderPasses.Add(std::move(pass));
        frames.Add(std::move(frame));
    }

    frames.RemoveAt(0, 17, EAllowShrinking::No);
    auto copy = frames;
    frames.Reset();
    ASSERT_EQ(copy.Num(), 47);
    for (i32 index = 0; index < copy.Num(); ++index)
    {
        const auto& frame = copy[index];
        EXPECT_EQ(frame.m_FrameNumber, static_cast<u32>(index + 17));
        EXPECT_EQ(frame.m_Notes.ToView(), std::string_view("frame\0note", 10));
        ASSERT_EQ(frame.m_RenderPasses.Num(), 1);
        ASSERT_EQ(frame.m_RenderPasses[0].m_DrawCalls.Num(), 1);
        EXPECT_EQ(frame.m_RenderPasses[0].m_DrawCalls[0].m_VertexCount, static_cast<u32>(index + 20));
        ASSERT_EQ(frame.m_BottleneckAnalysis.m_Recommendations.Num(), 1);
        EXPECT_EQ(frame.m_BottleneckAnalysis.m_Recommendations[0], "reduce draw calls");
        const auto& regions = frame.m_FrameData.m_ParallelRecording.RegionTimings;
        ASSERT_EQ(regions.Num(), 1);
        EXPECT_EQ(regions[0].PassName, "main");
        ASSERT_EQ(regions[0].ItemRecordMs.Num(), 2);
        EXPECT_DOUBLE_EQ(regions[0].ItemRecordMs[1], 2.5);
        ASSERT_EQ(regions[0].ItemPassNames.Num(), 2);
        EXPECT_EQ(regions[0].ItemPassNames[1], "second");
    }
}

TEST(CommandDispatchProfilerStorage, CounterHistoryKeepsChronologicalOrderAfterWrapAndReset)
{
    RendererProfiler::PerformanceCounter counter;
    TArray<f32> history;
    constexpr u32 capacity = RendererProfiler::PerformanceCounter::OLO_HISTORY_SIZE;
    for (u32 sample = 0; sample < capacity + 17; ++sample)
        counter.AddSample(static_cast<f64>(sample));
    counter.GetHistoryInOrder(history);
    ASSERT_EQ(history.Num(), static_cast<i32>(capacity));
    for (i32 index = 0; index < history.Num(); ++index)
        EXPECT_FLOAT_EQ(history[index], static_cast<f32>(index + 17));

    counter.Reset();
    counter.GetHistoryInOrder(history);
    EXPECT_TRUE(history.IsEmpty());
    counter.AddSample(42.0);
    counter.GetHistoryInOrder(history);
    ASSERT_EQ(history.Num(), 1);
    EXPECT_FLOAT_EQ(history[0], 42.0f);
}
