// OLO_TEST_LAYER: unit
//
// FPlatformProcess::SetThreadPriority on Linux, when the kernel refuses it.
// Without CAP_SYS_NICE a thread may lower its priority (raise its nice) but
// not raise it back, and the task scheduler lowers a worker for every
// background task and restores it afterwards. The refused restore was logged
// every time: ~3 million identical lines, 644 MB, in one AMD showcase job
// (#1674). And the worker stayed lowered, so every later task on it ran
// deprioritised. Such a process must not lower a thread it cannot restore, and
// must say so exactly once: not every time, and not never.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/HAL/PlatformProcess.h"

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
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

    [[nodiscard]] int CurrentNice()
    {
        errno = 0;
        const int nice = getpriority(PRIO_PROCESS, CurrentTid());
        return (nice == -1 && errno != 0) ? -100 : nice;
    }

    // Runs in a fresh process (the threadsafe death-test style re-executes the
    // binary), so no earlier test has spent the once-per-process report.
    // Returns 10 x the [PlatformProcess] reports logged (capped at 9, so the
    // exit status stays below 256), plus 1 if the worker did not end at the
    // nice it started with.
    int ReportsAndStrandingOverManyRestores()
    {
        std::atomic<int> reports{ 0 };
        const spdlog::sink_ptr sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&reports](const spdlog::details::log_msg& msg)
            {
                const std::string_view payload(msg.payload.data(), msg.payload.size());
                if (payload.find("[PlatformProcess]") != std::string_view::npos)
                {
                    reports.fetch_add(1, std::memory_order_relaxed);
                }
            });
        Log::Get().AttachSink(sink);

        // The scheduler's pattern: lower for a background task, restore after.
        bool stranded = false;
        std::thread worker(
            [&stranded]
            {
                const int start = CurrentNice();
                for (int task = 0; task < 1000; ++task)
                {
                    FPlatformProcess::SetThreadPriority(EThreadPriority::TPri_Lowest);
                    FPlatformProcess::SetThreadPriority(EThreadPriority::TPri_Normal);
                }
                stranded = CurrentNice() != start;
            });
        worker.join();

        Log::Get().DetachSink(sink);
        return std::min(reports.load(std::memory_order_relaxed), 9) * 10 + (stranded ? 1 : 0);
    }
} // namespace
#endif

TEST(PlatformProcessPriorityTest, AnUnrestorableLoweringIsSkippedAndReportedOnce)
{
#ifndef OLO_PLATFORM_LINUX
    GTEST_SKIP() << "the nice-based priority path and its refusal are Linux-only";
#else
    if (CanRaiseNiceBack())
    {
        GTEST_SKIP() << "this process may raise its own priority (CAP_SYS_NICE or RLIMIT_NICE), "
                        "so the kernel never refuses the restore this test is about";
    }
    if (CurrentNice() != 0)
    {
        // TPri_Normal is nice 0, which a process started above it may not reach
        // without CAP_SYS_NICE: every restore is then a second, legitimate refusal.
        GTEST_SKIP() << "this process starts at nice " << CurrentNice()
                     << ", so the restore to TPri_Normal is refused and the report count differs";
    }
    const std::string previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // 10 is one report and an unstranded worker. 0 is a silent fallback, 11 the
    // report with the worker left at nice 10, 90 or 91 the flood.
    EXPECT_EXIT({ std::_Exit(ReportsAndStrandingOverManyRestores()); }, ::testing::ExitedWithCode(10), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
#endif
}
