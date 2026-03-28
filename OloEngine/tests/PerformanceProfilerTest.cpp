#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/PerformanceProfiler.h"

#include <thread>

namespace
{
    using namespace OloEngine;

    // =========================================================================
    // PerformanceProfiler tests
    // =========================================================================

    TEST(PerformanceProfilerTest, InitiallyEmpty)
    {
        PerformanceProfiler profiler;
        auto const& data = profiler.GetPreviousFrameData();
        EXPECT_TRUE(data.empty());
    }

    TEST(PerformanceProfilerTest, SetPerFrameTimingAccumulates)
    {
        PerformanceProfiler profiler;

        profiler.SetPerFrameTiming("FuncA", 1.5f);
        profiler.SetPerFrameTiming("FuncA", 2.5f);
        profiler.SetPerFrameTiming("FuncB", 3.0f);

        // EndFrame snapshots current → previous
        profiler.EndFrame();

        auto const& data = profiler.GetPreviousFrameData();
        ASSERT_EQ(data.size(), 2u);

        auto itA = data.find("FuncA");
        ASSERT_NE(itA, data.end());
        EXPECT_FLOAT_EQ(itA->second.Time, 4.0f);
        EXPECT_EQ(itA->second.Samples, 2u);

        auto itB = data.find("FuncB");
        ASSERT_NE(itB, data.end());
        EXPECT_FLOAT_EQ(itB->second.Time, 3.0f);
        EXPECT_EQ(itB->second.Samples, 1u);
    }

    TEST(PerformanceProfilerTest, EndFrameResetsCurrent)
    {
        PerformanceProfiler profiler;

        profiler.SetPerFrameTiming("FuncA", 1.0f);
        profiler.EndFrame();

        // After EndFrame, the next frame starts empty
        profiler.EndFrame();
        auto const& data = profiler.GetPreviousFrameData();
        EXPECT_TRUE(data.empty());
    }

    TEST(PerformanceProfilerTest, ClearBothMaps)
    {
        PerformanceProfiler profiler;

        profiler.SetPerFrameTiming("FuncA", 1.0f);
        profiler.EndFrame();
        profiler.SetPerFrameTiming("FuncB", 2.0f);

        profiler.Clear();
        auto const& data = profiler.GetPreviousFrameData();
        EXPECT_TRUE(data.empty());

        // Next EndFrame should also have nothing
        profiler.EndFrame();
        EXPECT_TRUE(profiler.GetPreviousFrameData().empty());
    }

    TEST(PerformanceProfilerTest, MultipleFrames)
    {
        PerformanceProfiler profiler;

        // Frame 1
        profiler.SetPerFrameTiming("Update", 5.0f);
        profiler.EndFrame();

        // Frame 2 — different timing
        profiler.SetPerFrameTiming("Update", 3.0f);
        profiler.SetPerFrameTiming("Render", 8.0f);
        profiler.EndFrame();

        auto const& data = profiler.GetPreviousFrameData();
        ASSERT_EQ(data.size(), 2u);
        EXPECT_FLOAT_EQ(data.at("Update").Time, 3.0f);
        EXPECT_EQ(data.at("Update").Samples, 1u);
        EXPECT_FLOAT_EQ(data.at("Render").Time, 8.0f);
    }

    // =========================================================================
    // ScopedPerformanceTimer tests
    // =========================================================================

    TEST(ScopedPerformanceTimerTest, RecordsTiming)
    {
        PerformanceProfiler profiler;

        {
            ScopedPerformanceTimer timer("SleepTest", &profiler);
            // Small busy-wait to ensure measurable time
            auto start = std::chrono::high_resolution_clock::now();
            while (std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::high_resolution_clock::now() - start)
                       .count() < 100)
            {
                // spin
            }
        }

        profiler.EndFrame();
        auto const& data = profiler.GetPreviousFrameData();
        ASSERT_EQ(data.count("SleepTest"), 1u);
        EXPECT_GT(data.at("SleepTest").Time, 0.0f);
        EXPECT_EQ(data.at("SleepTest").Samples, 1u);
    }

    TEST(ScopedPerformanceTimerTest, NullProfilerSafe)
    {
        // Should not crash when profiler is nullptr
        { ScopedPerformanceTimer timer("NullTest", nullptr); }
        SUCCEED();
    }

    // =========================================================================
    // Thread-safety test
    // =========================================================================

    TEST(PerformanceProfilerTest, ConcurrentWriters)
    {
        PerformanceProfiler profiler;

        constexpr int kThreads = 4;
        constexpr int kIterations = 1000;

        auto writer = [&profiler](const char* name)
        {
            for (int i = 0; i < kIterations; ++i)
            {
                profiler.SetPerFrameTiming(name, 0.1f);
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        const char* names[] = { "Thread0", "Thread1", "Thread2", "Thread3" };
        for (int i = 0; i < kThreads; ++i)
        {
            threads.emplace_back(writer, names[i]);
        }
        for (auto& t : threads)
        {
            t.join();
        }

        profiler.EndFrame();
        auto const& data = profiler.GetPreviousFrameData();
        ASSERT_EQ(data.size(), static_cast<size_t>(kThreads));

        for (int i = 0; i < kThreads; ++i)
        {
            auto it = data.find(names[i]);
            ASSERT_NE(it, data.end());
            EXPECT_EQ(it->second.Samples, static_cast<u32>(kIterations));
            EXPECT_NEAR(it->second.Time, kIterations * 0.1f, 0.01f);
        }
    }

    // =========================================================================
    // PerFrameData tests
    // =========================================================================

    TEST(PerFrameDataTest, DefaultValues)
    {
        PerFrameData data;
        EXPECT_FLOAT_EQ(data.Time, 0.0f);
        EXPECT_EQ(data.Samples, 0u);
    }

} // namespace
