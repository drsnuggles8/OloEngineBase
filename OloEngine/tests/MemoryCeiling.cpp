#include "OloEnginePCH.h"
#include "MemoryCeiling.h"

#include "OloEngine/Memory/PlatformMemory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace OloEngine::Tests
{
    namespace
    {
        // The running test's name, written by the gtest listener on the main
        // thread and read by the watchdog thread -- under a mutex rather than
        // through gtest's own current_test_info(), which is not written for
        // cross-thread reads and would be a TSan report in the one build where
        // this watchdog matters most.
        std::mutex s_RunningTestMutex;
        std::string s_RunningTest;

        class RunningTestNameListener final : public ::testing::EmptyTestEventListener
        {
            void OnTestStart(const ::testing::TestInfo& info) override
            {
                const std::lock_guard<std::mutex> lock(s_RunningTestMutex);
                s_RunningTest = std::string(info.test_suite_name()) + "." + info.name();
            }
            void OnTestEnd(const ::testing::TestInfo& /*info*/) override
            {
                const std::lock_guard<std::mutex> lock(s_RunningTestMutex);
                s_RunningTest.clear();
            }
        };

        std::string RunningTestName()
        {
            const std::lock_guard<std::mutex> lock(s_RunningTestMutex);
            return s_RunningTest.empty() ? std::string("(no test running -- process start-up or shutdown)")
                                         : s_RunningTest;
        }

        constexpr auto kPollInterval = std::chrono::milliseconds(100);
    } // namespace

    u64 CurrentResidentBytes()
    {
        return FPlatformMemory::GetStats().UsedPhysical;
    }

    void RegisterMemoryCeilingListener()
    {
        ::testing::UnitTest::GetInstance()->listeners().Append(new RunningTestNameListener());
    }

    void StartMemoryCeilingWatchdog(const u64 ceilingMb)
    {
        if (ceilingMb == 0)
        {
            return;
        }

        // Detached on purpose: it must outlive nothing and be joined by nobody.
        // The process either exits normally (the thread dies with it) or the
        // thread ends the process itself.
        std::thread(
            [ceilingMb]
            {
                const u64 ceilingBytes = ceilingMb * 1024ull * 1024ull;
                for (;;)
                {
                    std::this_thread::sleep_for(kPollInterval);
                    const u64 resident = CurrentResidentBytes();
                    if (resident <= ceilingBytes)
                    {
                        continue;
                    }

                    const std::string test = RunningTestName();
                    std::fflush(stdout);
                    std::fprintf(stderr,
                                 "\nOloEngine-Tests: MEMORY CEILING EXCEEDED: resident set %llu MB is over the "
                                 "%llu MB per-process ceiling while running %s.\n"
                                 "Stopping this process (exit code %d) before the runner's OOM killer does. "
                                 "A test that needs this much memory is a defect in that test, not in the "
                                 "box; fix the growth, and raise or disable the ceiling with "
                                 "--olo-rss-ceiling-mb=<n> (0 disables) only when the footprint is legitimate.\n",
                                 static_cast<unsigned long long>(resident / (1024ull * 1024ull)),
                                 static_cast<unsigned long long>(ceilingMb), test.c_str(), kMemoryCeilingExitCode);
                    std::fflush(stderr);
                    // _Exit, not exit: no destructors, no atexit -- the process is
                    // over its budget and unwinding it could push it further.
                    std::_Exit(kMemoryCeilingExitCode);
                }
            })
            .detach();
    }
} // namespace OloEngine::Tests
