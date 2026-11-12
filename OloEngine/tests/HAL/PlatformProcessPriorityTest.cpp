// OLO_TEST_LAYER: unit
//
// FPlatformProcess::SetThreadPriority on Linux, when the kernel refuses it.
// Without CAP_SYS_NICE a thread may lower its priority (raise its nice) but
// not raise it back, and the task scheduler lowers a worker for every
// background task and restores it afterwards. The refusal was logged on every
// restore: ~3 million identical lines, 644 MB, in one AMD showcase job (#1674).
// It must be reported exactly once, not every time and not never.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/HAL/PlatformProcess.h"

#include <spdlog/sinks/callback_sink.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#ifdef OLO_PLATFORM_LINUX
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace OloEngine;

#ifdef OLO_PLATFORM_LINUX
namespace
{
    constexpr int kLoweredNice = 10; // ThreadPriorityToNice(TPri_Lowest)

    [[nodiscard]] id_t CurrentTid()
    {
        return static_cast<id_t>(syscall(SYS_gettid));
    }

    // Whether this process may raise a thread's priority back after lowering
    // it, asked of the kernel directly rather than through the code under test.
    [[nodiscard]] bool CanRaiseNiceBack()
    {
        bool raised = false;
        std::thread probe(
            [&raised]
            {
                const id_t tid = CurrentTid();
                if (setpriority(PRIO_PROCESS, tid, kLoweredNice) == 0)
                {
                    raised = setpriority(PRIO_PROCESS, tid, 0) == 0;
                }
            });
        probe.join();
        return raised;
    }

    // Runs in a fresh process (the threadsafe death-test style re-executes the
    // binary), so no earlier test has spent the once-per-process report.
    // Returns the number of setpriority refusals logged.
    int CountRefusalReportsOverManyRestores()
    {
        std::atomic<int> reports{ 0 };
        const spdlog::sink_ptr sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&reports](const spdlog::details::log_msg& msg)
            {
                const std::string_view payload(msg.payload.data(), msg.payload.size());
                if (payload.find("[PlatformProcess] setpriority(") != std::string_view::npos)
                {
                    reports.fetch_add(1, std::memory_order_relaxed);
                }
            });
        Log::Get().AttachSink(sink);

        // The scheduler's pattern: lower for a background task, restore after.
        std::thread worker(
            []
            {
                for (int task = 0; task < 1000; ++task)
                {
                    FPlatformProcess::SetThreadPriority(EThreadPriority::TPri_Lowest);
                    FPlatformProcess::SetThreadPriority(EThreadPriority::TPri_Normal);
                }
            });
        worker.join();

        Log::Get().DetachSink(sink);
        return reports.load(std::memory_order_relaxed);
    }
} // namespace
#endif

TEST(PlatformProcessPriorityTest, ARefusedPriorityRaiseIsReportedOnceNotPerTask)
{
#ifndef OLO_PLATFORM_LINUX
    GTEST_SKIP() << "the nice-based priority path and its refusal are Linux-only";
#else
    if (CanRaiseNiceBack())
    {
        GTEST_SKIP() << "this process may raise its own priority (CAP_SYS_NICE or RLIMIT_NICE), "
                        "so the kernel never refuses the restore this test is about";
    }
    const std::string previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Exit code = report count: 1 is right, 0 a silent fallback, 1000 the flood.
    EXPECT_EXIT({ std::_Exit(CountRefusalReportsOverManyRestores()); }, ::testing::ExitedWithCode(1), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
#endif
}
