// OLO_TEST_LAYER: plumbing

// =============================================================================
// FrameResourceManager::SubmitForDeletion from worker threads (found on #1392).
//
// Build Game packs the project on a worker thread, and the asset loads it does
// there create and release GL textures. A texture's destructor queues its GL
// deletion with SubmitForDeletion, so that queue is appended to from the
// worker while the main thread's BeginFrame drains and resets it. The queue
// was an unguarded TArray: the editor died with heap corruption (0xc0000374)
// inside BeginFrame, a few seconds into the pack build.
//
// Workers submit counting deletions while the main thread runs frames; every
// one must run exactly once, and on the main thread, where the GL context is.
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"
#include "PropertyTests/RendererAttachedTest.h"

#include "OloEngine/Renderer/Commands/FrameResourceManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace OloEngine::Tests
{
    class FrameResourceDeletionQueueThreadTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override {}
    };

    TEST_F(FrameResourceDeletionQueueThreadTest, WorkerSubmissionsRunOnceOnTheMainThread)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWorkers = 4;
        constexpr u32 kPerWorker = 5000;
        auto& frames = FrameResourceManager::Get();
        const std::thread::id mainThread = std::this_thread::get_id();

        std::atomic<u32> ran{ 0 };
        std::atomic<u32> ranOffMain{ 0 };
        std::atomic<u32> finishedWorkers{ 0 };

        std::vector<std::thread> workers;
        workers.reserve(kWorkers);
        for (u32 w = 0; w < kWorkers; ++w)
        {
            workers.emplace_back([&]
                                 {
                for (u32 i = 0; i < kPerWorker; ++i)
                {
                    frames.SubmitForDeletion([&ran, &ranOffMain, mainThread]
                                             {
                        ran.fetch_add(1, std::memory_order_relaxed);
                        if (std::this_thread::get_id() != mainThread)
                            ranOffMain.fetch_add(1, std::memory_order_relaxed); });
                }
                finishedWorkers.fetch_add(1, std::memory_order_release); });
        }

        // The main thread keeps draining while the workers append.
        while (finishedWorkers.load(std::memory_order_acquire) < kWorkers)
        {
            frames.BeginFrame();
            frames.EndFrame();
        }
        for (auto& worker : workers)
            worker.join();

        // Drain the rest the way a real frame does: each slot's queue runs
        // when BeginFrame reuses it, after that slot's fence.
        for (u32 i = 0; i <= FrameResourceManager::NUM_BUFFERED_FRAMES; ++i)
        {
            frames.BeginFrame();
            frames.EndFrame();
        }

        EXPECT_EQ(ran.load(), kWorkers * kPerWorker)
            << "deletions submitted from worker threads were lost or run twice";
        EXPECT_EQ(ranOffMain.load(), 0u) << "a deletion ran off the main thread, away from the GL context";
    }
} // namespace OloEngine::Tests
