#include "OloEnginePCH.h"
#include "MemoryCeiling.h"

#include "OloEngine/Memory/PlatformMemory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
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

        // The ceiling the watchdog enforces, in bytes; 0 enforces nothing. Set
        // by StartMemoryCeilingWatchdog, and lowered once by the listener when
        // the first test starts if a headroom was asked for.
        std::atomic<u64> s_CeilingBytes{ 0 };

        // --olo-rss-ceiling-headroom-mb in bytes, applied when the first test
        // starts; 0 keeps the absolute ceiling. s_FirstTestStarted makes that
        // a one-shot.
        std::atomic<u64> s_HeadroomBytes{ 0 };
        std::atomic<bool> s_FirstTestStarted{ false };

        // The watchdog is OWNED, not detached: main stops and joins it before
        // returning, so it can never outlive the statics above and read them
        // mid-destruction (CodeRabbit on #1204).
        std::thread s_Watchdog;
        std::atomic<bool> s_StopRequested{ false };

        class RunningTestNameListener final : public ::testing::EmptyTestEventListener
        {
            void OnTestStart(const ::testing::TestInfo& info) override
            {
                const u64 headroom = s_HeadroomBytes.load(std::memory_order_relaxed);
                if (headroom != 0 && !s_FirstTestStarted.exchange(true, std::memory_order_relaxed))
                {
                    // Start-up is measured in THIS process, not in whoever chose
                    // the headroom: two processes of the same binary on the same
                    // runner differ there by more than 128 MB under ASan.
                    const auto stats = FPlatformMemory::GetStats();
                    const u64 startup = std::max(stats.UsedPhysical, stats.PeakUsedPhysical);
                    const u64 absolute = s_CeilingBytes.load(std::memory_order_relaxed);
                    const u64 relative = startup + headroom;
                    s_CeilingBytes.store(absolute == 0 ? relative : std::min(absolute, relative),
                                         std::memory_order_relaxed);
                }
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

    void StartMemoryCeilingWatchdog(const u64 ceilingMb, const u64 headroomMb)
    {
        if ((ceilingMb == 0 && headroomMb == 0) || s_Watchdog.joinable())
        {
            return;
        }

        s_CeilingBytes.store(ceilingMb * 1024ull * 1024ull, std::memory_order_relaxed);
        s_HeadroomBytes.store(headroomMb * 1024ull * 1024ull, std::memory_order_relaxed);
        s_StopRequested.store(false, std::memory_order_release);
        s_Watchdog = std::thread(
            []
            {
                while (!s_StopRequested.load(std::memory_order_acquire))
                {
                    std::this_thread::sleep_for(kPollInterval);
                    const u64 ceilingBytes = s_CeilingBytes.load(std::memory_order_relaxed);
                    const u64 resident = CurrentResidentBytes();
                    if (ceilingBytes == 0 || resident <= ceilingBytes)
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
                                 static_cast<unsigned long long>(ceilingBytes / (1024ull * 1024ull)), test.c_str(),
                                 kMemoryCeilingExitCode);
                    std::fflush(stderr);
                    // _Exit, not exit: no destructors, no atexit -- the process is
                    // over its budget and unwinding it could push it further.
                    std::_Exit(kMemoryCeilingExitCode);
                }
            });
    }

    void StopMemoryCeilingWatchdog()
    {
        if (!s_Watchdog.joinable())
        {
            return;
        }
        s_StopRequested.store(true, std::memory_order_release);
        s_Watchdog.join();
    }
} // namespace OloEngine::Tests
