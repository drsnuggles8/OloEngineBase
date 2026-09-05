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
        auto& profiler = RendererProfiler::GetInstance();
        profiler.SetRecordInstancedDraws(false);
        profiler.Shutdown();
        CommandDispatch::Shutdown();
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
    EXPECT_TRUE(profiler.GetInstancedDrawRecords().empty());
    for (u32 item = 0; item < itemCount; ++item)
    {
        EXPECT_EQ(apis[item].CountCalls("BindVertexArrayRaw"), 1u);
        items[item].Publish();
    }
    EXPECT_EQ(CommandDispatch::GetStatistics().DrawCalls, itemCount * 2u);
    EXPECT_EQ(profiler.GetCurrentFrameData().m_InstancesRendered, itemCount * (itemCount + 1u) / 2u);
    ASSERT_EQ(profiler.GetInstancedDrawRecords().size(), itemCount);
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
