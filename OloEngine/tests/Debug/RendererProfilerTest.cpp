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
} // namespace
