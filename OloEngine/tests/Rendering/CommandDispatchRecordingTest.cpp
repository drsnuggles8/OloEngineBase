// OLO_TEST_LAYER: plumbing
#include "OloEnginePCH.h"
#include "MockRendererAPI.h"
#include "OloEngine/Math/Math.h"
#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/FrontendRecordingContext.h"

#include <glm/gtc/matrix_transform.hpp>
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

// -----------------------------------------------------------------------------
// The recording view (ADR 0025, issue #1330)
// -----------------------------------------------------------------------------
namespace
{
    // A main view whose every field differs from the default and from each
    // other, so a field an alternate view failed to replace or restore shows.
    [[nodiscard]] DispatchView MakeDistinctMainView()
    {
        DispatchView view;
        view.ViewMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
        view.ProjectionMatrix = glm::perspective(0.9f, 1.5f, 0.1f, 500.0f);
        view.ViewProjectionMatrix = view.ProjectionMatrix * view.ViewMatrix;
        view.PrevViewProjectionMatrix = view.ProjectionMatrix * glm::translate(glm::mat4(1.0f), glm::vec3(1.5f, 2.0f, 3.0f));
        view.JitterUV = glm::vec4(0.125f, -0.25f, 0.375f, -0.5f);
        view.ViewPosition = glm::vec3(-1.0f, -2.0f, -3.0f);
        view.RenderOrigin = glm::vec3(1024.0f, 0.0f, -2048.0f);
        view.MainViewLanes = true;
        return view;
    }

    [[nodiscard]] bool SameView(const DispatchView& a, const DispatchView& b)
    {
        return Math::BitwiseEqual(a.ViewMatrix, b.ViewMatrix) && Math::BitwiseEqual(a.ProjectionMatrix, b.ProjectionMatrix) &&
               Math::BitwiseEqual(a.ViewProjectionMatrix, b.ViewProjectionMatrix) &&
               Math::BitwiseEqual(a.PrevViewProjectionMatrix, b.PrevViewProjectionMatrix) &&
               Math::BitwiseEqual(a.JitterUV, b.JitterUV) && Math::BitwiseEqual(a.ViewPosition, b.ViewPosition) &&
               Math::BitwiseEqual(a.RenderOrigin, b.RenderOrigin) && a.MainViewLanes == b.MainViewLanes;
    }

    struct ReplayAborted
    {
    };
} // namespace

// The planar reflection used to swap four of the dispatcher's seven camera
// fields by hand, so its camera block carried the main view's previous
// view-projection and jitter, and restored them with straight-line code a
// throwing packet skipped. An alternate view is installed whole and restored
// by scope.
TEST_F(CommandDispatchRecording, AlternateViewIsWholeAndRestoredOnEveryExit)
{
    const DispatchView main = MakeDistinctMainView();
    CommandDispatch::SetView(main);

    const glm::mat4 mirrorView = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f));
    const glm::mat4 mirrorProjection = glm::perspective(0.7f, 1.0f, 0.5f, 100.0f);
    const glm::mat4 mirrorViewProjection = mirrorProjection * mirrorView;
    const glm::vec3 mirrorPosition(4.0f, -5.0f, 6.0f);
    const DispatchView alternate =
        CommandDispatch::MakeAlternateView(mirrorView, mirrorProjection, mirrorViewProjection, mirrorPosition);

    // What an alternate view is: the given camera, the frame's render origin,
    // no history of its own, no claim on the main view's screen-space lanes.
    EXPECT_TRUE(Math::BitwiseEqual(alternate.PrevViewProjectionMatrix, mirrorViewProjection))
        << "an alternate view inherited the main view's previous view-projection";
    EXPECT_TRUE(Math::BitwiseEqual(alternate.JitterUV, glm::vec4(main.JitterUV.x, main.JitterUV.y, main.JitterUV.x, main.JitterUV.y)))
        << "an alternate view inherited the main view's previous jitter";
    EXPECT_TRUE(Math::BitwiseEqual(alternate.RenderOrigin, main.RenderOrigin)) << "one render origin per frame";
    EXPECT_FALSE(alternate.MainViewLanes);

    {
        const CommandDispatch::ScopedAlternateView scope(alternate);
        EXPECT_TRUE(SameView(CommandDispatch::GetView(), alternate));
        EXPECT_TRUE(Math::BitwiseEqual(CommandDispatch::GetViewPosition(), mirrorPosition));
    }
    EXPECT_TRUE(SameView(CommandDispatch::GetView(), main)) << "leaving the scope did not restore the whole main view";

    try
    {
        const CommandDispatch::ScopedAlternateView scope(alternate);
        throw ReplayAborted{};
    }
    catch (const ReplayAborted&)
    {
    }
    EXPECT_TRUE(SameView(CommandDispatch::GetView(), main)) << "an aborted replay left the alternate view installed";
}

// Serial and worker replay read one context type. An item prepared inside an
// alternate-view scope draws from that view, whole; what an item does to its
// own view never reaches the primary or another item.
TEST_F(CommandDispatchRecording, SerialAndWorkerReplayReadTheSameView)
{
    const DispatchView main = MakeDistinctMainView();
    CommandDispatch::SetView(main);
    const glm::mat4 mirrorViewProjection = glm::perspective(0.7f, 1.0f, 0.5f, 100.0f);
    const DispatchView alternate =
        CommandDispatch::MakeAlternateView(glm::mat4(1.0f), mirrorViewProjection, mirrorViewProjection, glm::vec3(7.0f));

    constexpr u32 itemCount = 4;
    std::array<FrontendRecordingContext, itemCount> items;
    std::array<bool, itemCount> sawAlternate{};
    {
        const CommandDispatch::ScopedAlternateView scope(alternate);
        // The serial replay's answer, on the primary.
        EXPECT_TRUE(SameView(CommandDispatch::GetView(), alternate));
        // The fork: prepared on the caller before any worker starts.
        for (auto& item : items)
            item.Prepare(1);
        std::vector<std::jthread> threads;
        for (u32 item = 0; item < itemCount; ++item)
        {
            threads.emplace_back([&, item]
                                 {
                const ScopedFrontendRecordingContext recording(items[item]);
                sawAlternate[item] = SameView(CommandDispatch::GetView(), alternate);
                // An item-local override (a bucket's view state) stays in the item.
                DispatchView local = alternate;
                local.ViewPosition = glm::vec3(static_cast<f32>(item) + 100.0f);
                const CommandDispatch::ScopedAlternateView override(local); });
        }
        threads.clear();
        for (auto& item : items)
            item.Publish();
        EXPECT_TRUE(SameView(CommandDispatch::GetView(), alternate)) << "an item's view override reached the primary";
    }
    for (u32 item = 0; item < itemCount; ++item)
        EXPECT_TRUE(sawAlternate[item]) << "item " << item << " did not draw from the view the fork was made under";
    EXPECT_TRUE(SameView(CommandDispatch::GetView(), main));
}

// A bucket's view override (CommandBucket::SetViewState) used to patch four
// camera fields through a read/write callback pair and never upload the camera
// block. It has no production caller yet (it is the seam a second view would
// replay through), so this is what holds it to the same scope the planar
// reflection uses: a packet replayed under the override sees the whole
// alternate view, and the primary's view is back afterwards.
TEST_F(CommandDispatchRecording, ABucketViewOverrideReplaysInsideTheAlternateViewScope)
{
    const DispatchView main = MakeDistinctMainView();
    CommandDispatch::SetView(main);

    static DispatchView s_SeenByThePacket;
    s_SeenByThePacket = {};
    CommandAllocator allocator;
    CommandBucket bucket;
    CommandPacket* packet = allocator.CreateCommandPacket(MakeSyntheticViewportCommand(0, 0, 4, 4));
    ASSERT_NE(packet, nullptr);
    packet->SetDispatchFunction([](const void*, RendererAPI&)
                                { s_SeenByThePacket = CommandDispatch::GetView(); });
    bucket.SubmitPacket(packet);

    BucketViewState override;
    override.ViewMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -9.0f, 0.0f));
    override.ProjectionMatrix = glm::perspective(0.6f, 2.0f, 1.0f, 50.0f);
    override.ViewProjectionMatrix = override.ProjectionMatrix * override.ViewMatrix;
    override.ViewPosition = glm::vec3(0.0f, 9.0f, 0.0f);
    bucket.SetViewState(override);
    bucket.SortCommands();
    MockRendererAPI api;
    bucket.Execute(api);

    EXPECT_TRUE(Math::BitwiseEqual(s_SeenByThePacket.ViewMatrix, override.ViewMatrix));
    EXPECT_TRUE(Math::BitwiseEqual(s_SeenByThePacket.ViewProjectionMatrix, override.ViewProjectionMatrix));
    EXPECT_TRUE(Math::BitwiseEqual(s_SeenByThePacket.ViewPosition, override.ViewPosition));
    EXPECT_TRUE(Math::BitwiseEqual(s_SeenByThePacket.PrevViewProjectionMatrix, override.ViewProjectionMatrix))
        << "the override drew with the main view's previous view-projection";
    EXPECT_TRUE(Math::BitwiseEqual(s_SeenByThePacket.RenderOrigin, main.RenderOrigin));
    EXPECT_FALSE(s_SeenByThePacket.MainViewLanes);
    EXPECT_TRUE(SameView(CommandDispatch::GetView(), main)) << "the bucket's replay left its view override installed";
}

// CommandDispatch::Shutdown used to drop five buffer references and leave the
// rest: a dispatcher shut down for a device teardown came back with the
// previous device's camera, counters and pass flags.
TEST_F(CommandDispatchRecording, DispatcherShutdownLeavesNoRecordingState)
{
    CommandDispatch::SetView(MakeDistinctMainView());
    CommandDispatch::GetStatistics().DrawCalls = 12;
    CommandDispatch::SetDepthPrepassActive(true);

    CommandDispatch::Shutdown();

    EXPECT_TRUE(SameView(CommandDispatch::GetView(), DispatchView{})) << "Shutdown left the previous camera in the primary context";
    EXPECT_EQ(CommandDispatch::GetStatistics().DrawCalls, 0u);
    EXPECT_FALSE(CommandDispatch::HasUBOReferences());
    EXPECT_FALSE(CommandDispatch::IsDepthPrepassActive());

    // The dispatch table and the queue's callbacks are device-independent and
    // survive; Initialize re-arms the rest, as Renderer3D's reclaim does.
    EXPECT_NE(CommandDispatch::GetDispatchFunction(CommandType::DrawMesh), nullptr);
    CommandDispatch::Initialize();
}
