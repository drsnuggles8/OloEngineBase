// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"

#include <gtest/gtest.h>

#include <thread>

namespace
{
    // RendererProfiler is a process-wide singleton (GetInstance()), so every
    // test resets it in SetUp/TearDown to avoid leaking frame state into
    // whatever else runs in this binary.
    class RendererProfilerTimingTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            OloEngine::RendererProfiler::GetInstance().Reset();
        }

        void TearDown() override
        {
            OloEngine::RendererProfiler::GetInstance().Reset();
        }
    };

    // Issue #519 bug 1: RendererProfiler measured m_FrameTime at BeginFrame()
    // (the previous frame's wall-clock delta) and m_CPUTime at EndFrame() (the
    // current bracket), so a consumer reading GetCurrentFrameData() mid-frame
    // saw frameTimeMs from frame N-1 paired with cpuMs from frame N — which
    // could read as cpuMs > frameTimeMs whenever frame times swing. This test
    // drives two frames with deliberately different durations and asserts
    // GetLastCompletedFrameData() never mixes them: FrameTime/CPUTime/
    // GPUWaitTime for the reported frame must all be mutually consistent.
    TEST_F(RendererProfilerTimingTest, LastCompletedFrameNeverMixesMeasurementsAcrossFrames)
    {
        using namespace std::chrono_literals;
        auto& profiler = OloEngine::RendererProfiler::GetInstance();

        // Frame 1: long CPU-bound frame.
        profiler.BeginFrame();
        std::this_thread::sleep_for(20ms);
        profiler.EndFrame();

        // Gap between EndFrame(1) and BeginFrame(2) — stands in for
        // SwapBuffers/pacing time that happens outside the profiler bracket.
        std::this_thread::sleep_for(10ms);
        profiler.AddPostFrameGPUWaitTime(4.0);

        // Frame 2: short frame. BeginFrame() here is what patches frame 1's
        // FrameTime/GPUWaitTime now that they're fully known.
        profiler.BeginFrame();
        std::this_thread::sleep_for(1ms);
        profiler.EndFrame();

        const auto& completed = profiler.GetLastCompletedFrameData();

        // Frame 1 (the one just completed and patched) must describe a
        // single, self-consistent frame: its CPU time plus the post-frame
        // GPU wait we reported cannot exceed its total frame time.
        EXPECT_GE(completed.m_FrameTime, completed.m_CPUTime)
            << "frameTimeMs must never be smaller than cpuMs for the same completed frame";
        EXPECT_GE(completed.m_FrameTime, completed.m_CPUTime + completed.m_GPUWaitTime - 0.5)
            << "frameTimeMs must account for CPU work plus any reported GPU wait (small tolerance for scheduler jitter)";

        // The post-frame wait (e.g. SwapBuffers blocking) reported between
        // EndFrame(1) and BeginFrame(2) must land on frame 1, not be dropped.
        EXPECT_GE(completed.m_GPUWaitTime, 4.0 - 1e-6)
            << "gpuWaitMs must include GPU/present waits reported after EndFrame() via AddPostFrameGPUWaitTime()";

        // Frame 1 ran ~20ms of CPU work plus a ~10ms gap — its total frame
        // time should reflect that, not the ~1ms of frame 2.
        EXPECT_GT(completed.m_FrameTime, 25.0)
            << "frameTimeMs for frame 1 must reflect frame 1's own duration, not frame 2's short one";
    }

    // GetCurrentFrameData() is documented as a live, in-progress approximation
    // (its FrameTime is carried over from the previous frame) — this test
    // just pins that GetLastCompletedFrameData() is the one that stays
    // self-consistent even while a new frame is mid-flight.
    TEST_F(RendererProfilerTimingTest, CompletedFrameStaysStableWhileNextFrameIsInProgress)
    {
        using namespace std::chrono_literals;
        auto& profiler = OloEngine::RendererProfiler::GetInstance();

        profiler.BeginFrame();
        std::this_thread::sleep_for(5ms);
        profiler.EndFrame();

        std::this_thread::sleep_for(2ms);
        profiler.BeginFrame(); // patches frame 1 into GetLastCompletedFrameData()

        const auto snapshotBefore = profiler.GetLastCompletedFrameData();
        std::this_thread::sleep_for(3ms); // frame 2 still in progress

        const auto& snapshotDuring = profiler.GetLastCompletedFrameData();
        EXPECT_DOUBLE_EQ(snapshotBefore.m_FrameTime, snapshotDuring.m_FrameTime);
        EXPECT_DOUBLE_EQ(snapshotBefore.m_CPUTime, snapshotDuring.m_CPUTime);

        profiler.EndFrame();
    }

    TEST_F(RendererProfilerTimingTest, GPUSceneStatsPublishAndResetAtTheFrameBoundary)
    {
        auto& profiler = OloEngine::RendererProfiler::GetInstance();
        profiler.BeginFrame();
        // The per-kind upload figures sum to the total: 2 instances, 2
        // geometries, 3 materials, 2 lights and 1 environment record.
        profiler.SetGPUSceneStats(OloEngine::GPUSceneFrameStats{
            .m_Instances = { .m_Live = 7, .m_SlotCount = 9, .m_BufferCapacity = 16, .m_FreeSlots = 2, .m_RetiredSlots = 0, .m_UploadBytes = 256 },
            .m_Geometries = { .m_Live = 3, .m_SlotCount = 4, .m_BufferCapacity = 8, .m_FreeSlots = 1, .m_RetiredSlots = 0, .m_UploadBytes = 128 },
            .m_Materials = { .m_Live = 3, .m_SlotCount = 5, .m_BufferCapacity = 8, .m_FreeSlots = 1, .m_RetiredSlots = 1, .m_UploadBytes = 528 },
            .m_Lights = { .m_Live = 2, .m_SlotCount = 3, .m_BufferCapacity = 4, .m_FreeSlots = 1, .m_RetiredSlots = 0, .m_UploadBytes = 160 },
            .m_Environments = { .m_Live = 1, .m_SlotCount = 1, .m_BufferCapacity = 1, .m_UploadBytes = 64 },
            .m_BufferGrowthEvents = 2,
            .m_UnsupportedTotal = 5,
            .m_UploadBytes = 1136,
            .m_ExtractionTimeMs = 1.25,
        });

        const auto& published = profiler.GetCurrentFrameData().m_GPUScene;
        EXPECT_EQ(published.m_Instances.m_Live, 7u);
        EXPECT_EQ(published.m_Instances.m_SlotCount, 9u);
        EXPECT_EQ(published.m_Instances.m_BufferCapacity, 16u);
        EXPECT_EQ(published.m_Instances.m_FreeSlots, 2u);
        EXPECT_EQ(published.m_Instances.m_UploadBytes, 256u);
        EXPECT_EQ(published.m_Geometries.m_Live, 3u);
        EXPECT_EQ(published.m_Geometries.m_UploadBytes, 128u);
        EXPECT_EQ(published.m_Materials.m_Live, 3u);
        EXPECT_EQ(published.m_Materials.m_SlotCount, 5u);
        EXPECT_EQ(published.m_Materials.m_BufferCapacity, 8u);
        EXPECT_EQ(published.m_Materials.m_FreeSlots, 1u);
        EXPECT_EQ(published.m_Materials.m_RetiredSlots, 1u);
        EXPECT_EQ(published.m_Materials.m_UploadBytes, 528u);
        EXPECT_EQ(published.m_Lights.m_Live, 2u);
        EXPECT_EQ(published.m_Lights.m_SlotCount, 3u);
        EXPECT_EQ(published.m_Lights.m_BufferCapacity, 4u);
        EXPECT_EQ(published.m_Lights.m_FreeSlots, 1u);
        EXPECT_EQ(published.m_Lights.m_UploadBytes, 160u);
        EXPECT_EQ(published.m_Environments.m_Live, 1u);
        EXPECT_EQ(published.m_Environments.m_BufferCapacity, 1u);
        EXPECT_EQ(published.m_Environments.m_UploadBytes, 64u);
        EXPECT_EQ(published.m_BufferGrowthEvents, 2u);
        EXPECT_EQ(published.m_UnsupportedTotal, 5u);
        EXPECT_EQ(published.m_UploadBytes, 1136u);
        EXPECT_DOUBLE_EQ(published.m_ExtractionTimeMs, 1.25);
        profiler.EndFrame();

        profiler.BeginFrame();
        const auto& reset = profiler.GetCurrentFrameData().m_GPUScene;
        EXPECT_EQ(reset.m_Instances.m_Live, 0u);
        EXPECT_EQ(reset.m_Instances.m_UploadBytes, 0u);
        EXPECT_EQ(reset.m_Geometries.m_Live, 0u);
        EXPECT_EQ(reset.m_Geometries.m_UploadBytes, 0u);
        EXPECT_EQ(reset.m_Materials.m_Live, 0u);
        EXPECT_EQ(reset.m_Materials.m_SlotCount, 0u);
        EXPECT_EQ(reset.m_Materials.m_BufferCapacity, 0u);
        EXPECT_EQ(reset.m_Materials.m_FreeSlots, 0u);
        EXPECT_EQ(reset.m_Materials.m_RetiredSlots, 0u);
        EXPECT_EQ(reset.m_Materials.m_UploadBytes, 0u);
        EXPECT_EQ(reset.m_Lights.m_Live, 0u);
        EXPECT_EQ(reset.m_Lights.m_UploadBytes, 0u);
        EXPECT_EQ(reset.m_Environments.m_Live, 0u);
        EXPECT_EQ(reset.m_Environments.m_UploadBytes, 0u);
        EXPECT_EQ(reset.m_BufferGrowthEvents, 0u);
        EXPECT_EQ(reset.m_UploadBytes, 0u)
            << "a frame without GPU-scene extraction must not retain prior telemetry";
        profiler.EndFrame();
    }

    // ---- Bottleneck analysis and the fence/present split (#1337) ------------
    //
    // These pin two verdicts that were wrong in opposite directions, and the
    // review that caught the second is the reason the first is tested at all.

    TEST_F(RendererProfilerTimingTest, AnUnmeasuredGpuTimeStillLetsAFenceWaitDecide)
    {
        // REGRESSION GUARD. Removing the "0.0 means the GPU did nothing" verdict
        // is right; removing the FENCE-WAIT verdict along with it is not. The
        // fence wait is its own measurement and answers the question without a
        // GPU timer: the CPU finished and sat blocked until the GPU caught up.
        auto& profiler = OloEngine::RendererProfiler::GetInstance();

        profiler.BeginFrame();
        profiler.AddGPUWaitTime(9.0); // fence: the GPU is behind
        profiler.SetFrameGpuSample(
            OloEngine::GpuTimingSample::Absent(OloEngine::GpuTimingStatus::Dropped));
        profiler.SetValue(OloEngine::RendererProfiler::MetricType::FrameTime, 16.0);
        profiler.EndFrame();

        const auto info = profiler.AnalyzeBottlenecks();
        EXPECT_EQ(info.m_Type, OloEngine::RendererProfiler::BottleneckInfo::GPU_Bound)
            << "a 9 ms fence wait in a 16 ms frame is a GPU-bound frame whether or not the GPU timer resolved";
        EXPECT_GT(info.m_Confidence, 0.0f);
    }

    TEST_F(RendererProfilerTimingTest, AVsyncPacedFrameSeparatesFenceWaitFromPresentWait)
    {
        // The reporting surface, which is where the conflation was real and
        // where it was measured: a live Vulkan session reported gpuWaitMs
        // 17.515 against presentWaitMs 17.485 in a 17.906 ms frame — i.e.
        // essentially none of that "GPU wait" was the fence. (On Vulkan the
        // SwapBuffers span is the frame's own recording, #691, so it is not a
        // vsync reading either; the split is what makes the fence half
        // readable on its own.) Before the split there was one number.
        //
        // Deliberately NOT a bottleneck-verdict assertion. AnalyzeBottlenecks
        // reads the IN-PROGRESS frame, where a present wait can never appear:
        // it is only known after EndFrame, so it is patched into the COMPLETED
        // frame at the next BeginFrame. An earlier revision of this test
        // asserted a verdict here and failed, because it was asserting a state
        // the code cannot reach.
        auto& profiler = OloEngine::RendererProfiler::GetInstance();

        profiler.BeginFrame();
        profiler.AddGPUWaitTime(0.031);           // fence: the GPU is barely behind
        profiler.AddPostFrameGPUWaitTime(17.485); // present: vsync holds the frame
        profiler.SetFrameGpuSample(OloEngine::GpuTimingSample::Measured(4.6));
        profiler.EndFrame();
        // The patch happens here, at the next BeginFrame.
        profiler.BeginFrame();

        const auto& completed = profiler.GetLastCompletedFrameData();
        EXPECT_NEAR(completed.m_FenceWaitTime, 0.031, 1e-6);
        EXPECT_NEAR(completed.m_PresentWaitTime, 17.485, 1e-6);
        EXPECT_NEAR(completed.m_GPUWaitTime, completed.m_FenceWaitTime + completed.m_PresentWaitTime, 1e-6)
            << "gpuWaitMs must stay the sum of its two halves, or the CSV and every MCP reply disagree";
        EXPECT_GT(completed.m_PresentWaitTime, completed.m_FenceWaitTime * 100.0)
            << "this frame is display-paced, and the split is what makes that legible";
    }

    TEST_F(RendererProfilerTimingTest, AStalledPoolsRepublishedFrameIsSampledOnce)
    {
        // While the GPU is behind, GPUPassTimerPool keeps publishing its last
        // resolved frame with a growing age. RenderPipeline hands that to the
        // profiler every frame; without the frame id the same measurement was
        // sampled once per frame and the aggregate drifted toward whichever
        // frame preceded the stall.
        auto& profiler = OloEngine::RendererProfiler::GetInstance();
        const auto frame = [&profiler](double gpuMs, u64 poolFrame)
        {
            profiler.BeginFrame();
            profiler.SetFrameGpuSample(OloEngine::GpuTimingSample::Measured(gpuMs), poolFrame);
            profiler.SetValue(OloEngine::RendererProfiler::MetricType::FrameTime, 16.0);
            profiler.EndFrame();
        };
        frame(4.0, 10);
        frame(4.0, 10); // the pool republished frame 10
        frame(4.0, 10); // ... and again
        frame(6.0, 11); // a new resolve

        const auto& counter = profiler.GetCounter(OloEngine::RendererProfiler::MetricType::GPUTime);
        EXPECT_EQ(counter.m_SampleCount, 2u) << "frame 10 was sampled once per republish";
        EXPECT_NEAR(counter.m_Average, 5.0, 1e-9);
    }

    TEST_F(RendererProfilerTimingTest, AnUnknownFrameIdStillSamplesEveryMeasuredFrame)
    {
        // The dedupe keys on the pool frame id; a producer that does not carry
        // one (frameId 0) must not have its second frame swallowed.
        auto& profiler = OloEngine::RendererProfiler::GetInstance();
        for (int i = 0; i < 3; ++i)
        {
            profiler.BeginFrame();
            profiler.SetFrameGpuSample(OloEngine::GpuTimingSample::Measured(2.0));
            profiler.EndFrame();
        }
        EXPECT_EQ(profiler.GetCounter(OloEngine::RendererProfiler::MetricType::GPUTime).m_SampleCount, 3u);
    }

} // namespace
