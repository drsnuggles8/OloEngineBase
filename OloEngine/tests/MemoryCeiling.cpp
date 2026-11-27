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
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A sanitizer that replaces malloc owns the heap; glibc's arenas then hold
// nothing, and its figures would describe a heap the process does not use.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define OLO_TESTS_SANITIZER_OWNS_HEAP 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define OLO_TESTS_SANITIZER_OWNS_HEAP 1
#endif

#if defined(__GLIBC__) && !defined(OLO_TESTS_SANITIZER_OWNS_HEAP)
#define OLO_TESTS_GLIBC_HEAP 1
#include <malloc.h>
#endif

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

        // The running test's resident set when it started, and the highest the
        // watchdog saw during it; reset by OnTestStart, read by OnTestEnd.
        std::atomic<u64> s_TestStartBytes{ 0 };
        std::atomic<u64> s_TestPeakBytes{ 0 };
        std::atomic<u64> s_FirstTestStartBytes{ 0 };

        // Every finished test's figures, appended on the main thread and read by
        // the watchdog only on its way out. A whole-suite run holds ~11k rows.
        std::mutex s_HistoryMutex;
        std::vector<TestResidency> s_History;

        // --olo-rss-report, opened before the watchdog starts and closed after it stops.
        std::FILE* s_Report = nullptr;

        constexpr u64 kBytesPerMb = 1024ull * 1024ull;

        long long ToMb(const u64 bytes)
        {
            return static_cast<long long>(bytes / kBytesPerMb);
        }

        long long SignedMb(const u64 from, const u64 to)
        {
            return ToMb(to) - ToMb(from);
        }

        void RaisePeak(const u64 resident)
        {
            u64 seen = s_TestPeakBytes.load(std::memory_order_relaxed);
            while (resident > seen && !s_TestPeakBytes.compare_exchange_weak(seen, resident, std::memory_order_relaxed))
            {
            }
        }

        // Formats an optional megabyte figure for the report: empty where the
        // platform cannot say, so a reader never mistakes "unknown" for 0.
        std::string OptionalMb(const std::optional<u64>& bytes)
        {
            return bytes ? std::to_string(ToMb(*bytes)) : std::string();
        }

        class RunningTestNameListener final : public ::testing::EmptyTestEventListener
        {
            void OnTestStart(const ::testing::TestInfo& info) override
            {
                const u64 resident = CurrentResidentBytes();
                s_TestStartBytes.store(resident, std::memory_order_relaxed);
                s_TestPeakBytes.store(resident, std::memory_order_relaxed);
                u64 unset = 0;
                s_FirstTestStartBytes.compare_exchange_strong(unset, resident, std::memory_order_relaxed);

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
            void OnTestEnd(const ::testing::TestInfo& info) override
            {
                const u64 beforeRelease = CurrentResidentBytes();
                RaisePeak(beforeRelease);
                // Hand the freed heap back before the next test inherits it: the
                // end figure, and so every later start, is then what the test
                // really kept.
                const HeapRelease heap = ReleaseFreeHeap();
                const u64 end = CurrentResidentBytes();
                TestResidency row{ std::string(info.test_suite_name()) + "." + info.name(),
                                   s_TestStartBytes.load(std::memory_order_relaxed), end,
                                   s_TestPeakBytes.load(std::memory_order_relaxed) };
                if (s_Report != nullptr)
                {
                    const std::optional<u64> released =
                        heap.Supported ? std::optional<u64>(beforeRelease > end ? beforeRelease - end : 0)
                                       : std::nullopt;
                    std::fprintf(s_Report, "%s\t%lld\t%lld\t%lld\t%lld\t%s\t%s\t%s\n", row.Name.c_str(),
                                 ToMb(row.StartBytes), ToMb(row.EndBytes), ToMb(row.PeakBytes),
                                 SignedMb(row.StartBytes, row.EndBytes), OptionalMb(heap.UsedBytes).c_str(),
                                 OptionalMb(heap.FreeBytes).c_str(), OptionalMb(released).c_str());
                    std::fflush(s_Report);
                }
                {
                    const std::lock_guard<std::mutex> lock(s_HistoryMutex);
                    s_History.push_back(std::move(row));
                }
                const std::lock_guard<std::mutex> lock(s_RunningTestMutex);
                s_RunningTest.clear();
            }
        };

        // What the process carried into the test that crossed the ceiling. One
        // case alone rarely reaches 6 GiB; a whole-suite run reaches it on what
        // the cases before it kept, and the name of the last one says nothing
        // about those.
        void PrintRetainers(const u64 resident)
        {
            std::vector<TestResidency> history;
            {
                const std::lock_guard<std::mutex> lock(s_HistoryMutex);
                history = s_History;
            }
            const u64 first = s_FirstTestStartBytes.load(std::memory_order_relaxed);
            const u64 start = s_TestStartBytes.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "Resident set when the first test started: %lld MB; when this test started: %lld MB "
                         "(%zu tests had finished); %+lld MB since.\n",
                         ToMb(first), ToMb(start), history.size(), SignedMb(start, resident));
            const std::vector<TestResidency> top = LargestRetainers(history, 15);
            if (top.empty())
            {
                return;
            }
            std::fprintf(stderr, "Finished tests that kept the most memory (end minus start, then peak):\n");
            for (const TestResidency& row : top)
            {
                std::fprintf(stderr, "  %+6lld MB  peak %6lld MB  %s\n", SignedMb(row.StartBytes, row.EndBytes),
                             ToMb(row.PeakBytes), row.Name.c_str());
            }
        }

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

    HeapRelease ReleaseFreeHeap()
    {
        HeapRelease result;
#if defined(OLO_TESTS_GLIBC_HEAP)
#if __GLIBC_PREREQ(2, 33)
        const struct mallinfo2 info = ::mallinfo2();
        result.UsedBytes = static_cast<u64>(info.uordblks) + static_cast<u64>(info.hblkhd);
        result.FreeBytes = static_cast<u64>(info.fordblks);
#endif
        result.Supported = true;
        ::malloc_trim(0);
#endif
        return result;
    }

    void RegisterMemoryCeilingListener()
    {
        ::testing::UnitTest::GetInstance()->listeners().Append(new RunningTestNameListener());
    }

    std::vector<TestResidency> LargestRetainers(const std::vector<TestResidency>& history, const sizet count)
    {
        std::vector<TestResidency> grew;
        for (const TestResidency& row : history)
        {
            if (row.EndBytes > row.StartBytes)
            {
                grew.push_back(row);
            }
        }
        const auto byRetained = [](const TestResidency& a, const TestResidency& b)
        { return a.EndBytes - a.StartBytes > b.EndBytes - b.StartBytes; };
        const sizet keep = std::min(count, grew.size());
        std::partial_sort(grew.begin(), grew.begin() + static_cast<std::ptrdiff_t>(keep), grew.end(), byRetained);
        grew.resize(keep);
        return grew;
    }

    bool OpenResidentSetReport(const std::string& path)
    {
        if (path.empty())
        {
            return true;
        }
        s_Report = std::fopen(path.c_str(), "w");
        if (s_Report == nullptr)
        {
            std::fprintf(stderr, "OloEngine-Tests: --olo-rss-report: cannot open '%s' for writing.\n", path.c_str());
            return false;
        }
        std::fprintf(s_Report, "test\tstart_mb\tend_mb\tpeak_mb\tretained_mb\theap_used_mb\theap_free_mb\treleased_mb\n");
        std::fflush(s_Report);
        return true;
    }

    void StartMemoryCeilingWatchdog(const u64 ceilingMb, const u64 headroomMb)
    {
        if ((ceilingMb == 0 && headroomMb == 0 && s_Report == nullptr) || s_Watchdog.joinable())
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
                    RaisePeak(resident);
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
                    PrintRetainers(resident);
                    std::fflush(stderr);
                    // _Exit, not exit: no destructors, no atexit -- the process is
                    // over its budget and unwinding it could push it further.
                    std::_Exit(kMemoryCeilingExitCode);
                }
            });
    }

    void StopMemoryCeilingWatchdog()
    {
        if (s_Watchdog.joinable())
        {
            s_StopRequested.store(true, std::memory_order_release);
            s_Watchdog.join();
        }
        if (s_Report != nullptr)
        {
            std::fclose(s_Report);
            s_Report = nullptr;
        }
    }
} // namespace OloEngine::Tests
