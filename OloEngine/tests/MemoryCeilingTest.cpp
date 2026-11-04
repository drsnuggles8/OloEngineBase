// OLO_TEST_LAYER: meta
//
// The per-process memory ceiling (MemoryCeiling.h), observed from outside the
// process. The watchdog ends the process it lives in, so the only honest test
// re-launches OloEngine-Tests with a ceiling the probe case below is sure to
// cross, and reads the child's exit code and output. Anything in-process would
// be testing a copy of the logic, not the thread CI actually runs under.
//
// Why it exists (the BC6HGpuEncoder OOM): a case that grows past the runner's
// cgroup is killed by the kernel with no name and no message, and can take the
// runner down with it. The ceiling turns that into a named failure. This test
// pins the three things that make it useful: it fires, it exits with the
// distinct code, and the message names the test that was running.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MemoryCeiling.h"
#include "TestProcessLaunch.h"
#include "TestTempDir.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
    using namespace OloEngine;

    constexpr sizet kMiB = 1024u * 1024u;

    // Path of the running test binary, so the guard can re-launch itself.
    std::string SelfExecutablePath()
    {
#if defined(_WIN32)
        char buffer[MAX_PATH] = {};
        const DWORD n = ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        return std::string(buffer, n);
#else
        char buffer[4096] = {};
        const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        return n > 0 ? std::string(buffer, static_cast<sizet>(n)) : std::string();
#endif
    }

    // The retaining probe below keeps its memory until the process exits, so it
    // runs only in a child these tests launch, never in a suite run it would leave
    // a quarter gigabyte heavier. The child inherits the variable.
    constexpr const char* kRetainingProbeVariable = "OLO_MEMORY_CEILING_RETAINING_PROBE";

    class ScopedRetainingProbeEnabled
    {
      public:
        ScopedRetainingProbeEnabled()
        {
            Set("1");
        }
        ~ScopedRetainingProbeEnabled()
        {
            Set(nullptr);
        }
        ScopedRetainingProbeEnabled(const ScopedRetainingProbeEnabled&) = delete;
        ScopedRetainingProbeEnabled& operator=(const ScopedRetainingProbeEnabled&) = delete;

      private:
        static void Set(const char* value)
        {
#if defined(_WIN32)
            ::_putenv_s(kRetainingProbeVariable, value != nullptr ? value : "");
#else
            if (value != nullptr)
                ::setenv(kRetainingProbeVariable, value, 1);
            else
                ::unsetenv(kRetainingProbeVariable);
#endif
        }
    };

    void TouchedChunk(std::unique_ptr<char[]>& chunk, const sizet bytes, const int fill)
    {
        chunk = std::make_unique<char[]>(bytes);
        std::memset(chunk.get(), fill, bytes);
    }

    struct ReportRow
    {
        std::string Name;
        long long StartMb = 0;
        long long EndMb = 0;
        long long PeakMb = 0;
        long long RetainedMb = 0;
    };

    [[nodiscard]] ReportRow ParseReportRow(const std::string& row)
    {
        ReportRow parsed;
        std::istringstream fields(row);
        fields >> parsed.Name >> parsed.StartMb >> parsed.EndMb >> parsed.PeakMb >> parsed.RetainedMb;
        return parsed;
    }

    // The retaining probe alone in an uncapped child, writing the per-test
    // report: what keeping 256 MB costs in this build. The caller holds a
    // ScopedRetainingProbeEnabled.
    [[nodiscard]] OloEngine::Tests::LaunchResult RunRetainingProbeWithReport(const std::string& exe,
                                                                             const std::filesystem::path& report)
    {
        return OloEngine::Tests::RunProcessWithTimeout(
            exe,
            { "--olo-rss-ceiling-mb=0", "--olo-rss-report=" + report.string(),
              "--gtest_filter=MemoryCeilingProbe.KeepsAQuarterGigabyteUntilExit" },
            "", 120000);
    }
} // namespace

// A test that keeps a quarter gigabyte after it finishes: what a whole-suite
// process carries from one case into the next. Registered before the half-
// gigabyte probe so gtest runs it first when a filter selects both.
TEST(MemoryCeilingProbe, KeepsAQuarterGigabyteUntilExit)
{
    if (std::getenv(kRetainingProbeVariable) == nullptr)
    {
        GTEST_SKIP() << "runs only in the child MemoryCeiling.* launches (it never frees its memory)";
    }
    static std::unique_ptr<char[]> s_Kept;
    TouchedChunk(s_Kept, 256u * kMiB, 7);
    EXPECT_EQ(s_Kept[256u * kMiB - 1], 7);
}

// A death test after a finished test: on Windows gtest re-launches this binary
// with the parent's whole command line, --olo-rss-report included. Registered
// after the retaining probe so the parent has written a row the child could
// truncate.
TEST(MemoryCeilingProbe, ADeathTestRunsAfterAFinishedTest)
{
    if (std::getenv(kRetainingProbeVariable) == nullptr)
    {
        GTEST_SKIP() << "runs only in the child MemoryCeiling.* launches";
    }
    EXPECT_EXIT({ std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

// The probe: grows the resident set by half a gigabyte, touching every page so
// the growth is real memory rather than reserved address space, in steps slow
// enough for a 100 ms poll to catch it mid-test. In a normal suite run this is
// far under the default ceiling and passes; the guard below runs it in a child
// with a ceiling it must cross.
TEST(MemoryCeilingProbe, AllocatesAndTouchesHalfAGigabyte)
{
    constexpr sizet kChunk = 32u * kMiB;
    constexpr int kChunks = 16;
    std::vector<std::unique_ptr<char[]>> chunks;
    chunks.reserve(kChunks);
    for (int i = 0; i < kChunks; ++i)
    {
        auto chunk = std::make_unique<char[]>(kChunk);
        std::memset(chunk.get(), static_cast<int>(i + 1), kChunk);
        chunks.push_back(std::move(chunk));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Keep every chunk alive until the end, and use them so nothing is elided.
    sizet sum = 0;
    for (const auto& chunk : chunks)
    {
        sum += static_cast<unsigned char>(chunk[kChunk - 1]);
    }
    EXPECT_EQ(sum, static_cast<sizet>(kChunks * (kChunks + 1) / 2));
}

TEST(MemoryCeiling, ARunawayTestIsStoppedAtTheCeilingAndNamedInTheOutput)
{
    const std::string exe = SelfExecutablePath();
    ASSERT_FALSE(exe.empty()) << "could not resolve the test binary's own path";

    // Without a resident-set figure the watchdog cannot fire, and the child
    // would pass the probe for a reason that has nothing to do with the ceiling.
    ASSERT_GT(OloEngine::Tests::CurrentResidentBytes() / kMiB, 0u)
        << "CurrentResidentBytes() reports nothing on this platform";

    // The child takes its ceiling as 128 MB above ITS OWN start-up, measured
    // when its first test starts; the probe adds 512 MB on top, so the ceiling
    // is crossed while the probe runs and never during start-up. A ceiling
    // computed here from this process's footprint is not one: on the ASan
    // (Windows) job the child's start-up reached 350 and 376 MB against this
    // process's 216 MB, and the child was stopped with no test running.
    const std::vector<std::string> args = {
        "--olo-rss-ceiling-headroom-mb=128",
        "--gtest_filter=MemoryCeilingProbe.AllocatesAndTouchesHalfAGigabyte",
    };
    const OloEngine::Tests::LaunchResult r =
        OloEngine::Tests::RunProcessWithTimeout(exe, args, /*workingDir*/ "", /*timeoutMs*/ 120000);

    ASSERT_TRUE(r.Launched) << "Failed to launch " << exe << ": " << r.Error;
    ASSERT_FALSE(r.TimedOut) << "the child never exited -- the watchdog did not fire.\n--- child output ---\n"
                             << r.Output;
    EXPECT_EQ(r.ExitCode, OloEngine::Tests::kMemoryCeilingExitCode)
        << "expected the watchdog's exit code; the child exited " << r.ExitCode
        << " instead.\n--- child output ---\n"
        << r.Output;
    EXPECT_NE(r.Output.find("MEMORY CEILING EXCEEDED"), std::string::npos)
        << "the watchdog's message is missing from the child's output.\n--- child output ---\n"
        << r.Output;
    EXPECT_NE(r.Output.find("MemoryCeilingProbe.AllocatesAndTouchesHalfAGigabyte"), std::string::npos)
        << "the message does not name the test that was running.\n--- child output ---\n"
        << r.Output;
}

TEST(MemoryCeiling, LargestRetainersRanksByRetainedGrowthAndNeverListsATestThatReleased)
{
    using OloEngine::Tests::TestResidency;
    const std::vector<TestResidency> history = {
        { "A.Small", 100, 110, 120 },
        { "A.Released", 500, 100, 900 },
        { "A.Large", 100, 400, 450 },
        { "A.Flat", 300, 300, 300 },
        { "A.Medium", 100, 250, 260 },
    };
    const auto top = OloEngine::Tests::LargestRetainers(history, 2);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].Name, "A.Large");
    EXPECT_EQ(top[1].Name, "A.Medium");
    EXPECT_EQ(OloEngine::Tests::LargestRetainers(history, 10).size(), 3u);
}

// The report a whole-suite run uploads: one row per finished test, with the
// memory it kept. The probe keeps 256 MB, so its row must say so.
TEST(MemoryCeiling, TheReportHasARowPerFinishedTestWithTheMemoryItKept)
{
    const std::string exe = SelfExecutablePath();
    ASSERT_FALSE(exe.empty());
    const std::filesystem::path report = OloEngine::Tests::TempFile("rss.tsv");
    const ScopedRetainingProbeEnabled enabled;
    const OloEngine::Tests::LaunchResult r = RunRetainingProbeWithReport(exe, report);
    ASSERT_TRUE(r.Launched) << r.Error;
    ASSERT_EQ(r.ExitCode, 0) << r.Output;

    std::ifstream in(report);
    std::string header;
    std::string row;
    ASSERT_TRUE(std::getline(in, header)) << "the report is empty";
    EXPECT_EQ(header, "test\tstart_mb\tend_mb\tpeak_mb\tretained_mb");
    ASSERT_TRUE(std::getline(in, row)) << "the report has no row for the finished test";
    const ReportRow parsed = ParseReportRow(row);
    EXPECT_EQ(parsed.Name, "MemoryCeilingProbe.KeepsAQuarterGigabyteUntilExit");
    EXPECT_GE(parsed.RetainedMb, 200) << row;
    EXPECT_EQ(parsed.RetainedMb, parsed.EndMb - parsed.StartMb) << row;
    EXPECT_GE(parsed.PeakMb, parsed.EndMb) << row;
}

// A death-test child inherits --olo-rss-report. Opening it there truncated the
// parent's file: a local sweep lost its first 1715 rows to NUL bytes at
// ThreadManagerTest's EXPECT_EXIT. Every row must survive a death test.
TEST(MemoryCeiling, ADeathTestChildLeavesTheParentsReportIntact)
{
    const std::string exe = SelfExecutablePath();
    ASSERT_FALSE(exe.empty());
    const std::filesystem::path report = OloEngine::Tests::TempFile("rss.tsv");
    const ScopedRetainingProbeEnabled enabled;
    const OloEngine::Tests::LaunchResult r = OloEngine::Tests::RunProcessWithTimeout(
        exe,
        { "--olo-rss-ceiling-mb=0", "--olo-rss-report=" + report.string(),
          "--gtest_filter=MemoryCeilingProbe.KeepsAQuarterGigabyteUntilExit:"
          "MemoryCeilingProbe.ADeathTestRunsAfterAFinishedTest" },
        "", 120000);
    ASSERT_TRUE(r.Launched) << r.Error;
    ASSERT_EQ(r.ExitCode, 0) << r.Output;

    std::ifstream in(report, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find('\0'), std::string::npos) << "the report holds NUL bytes: something truncated it";
    EXPECT_EQ(content.find("test\tstart_mb"), 0u) << content;
    EXPECT_NE(content.find("MemoryCeilingProbe.KeepsAQuarterGigabyteUntilExit\t"), std::string::npos) << content;
    EXPECT_NE(content.find("MemoryCeilingProbe.ADeathTestRunsAfterAFinishedTest\t"), std::string::npos) << content;
}

// What the AMD nightly needed on 2026-11-04: the process crossed 6 GiB during
// AcceptanceSceneTest, 6466 tests in, and the message named only that test.
// It must also name the tests that left the memory behind.
TEST(MemoryCeiling, TheStopMessageNamesTheFinishedTestsThatKeptTheMemory)
{
    const std::string exe = SelfExecutablePath();
    ASSERT_FALSE(exe.empty());
    const ScopedRetainingProbeEnabled enabled;

    // What the kept 256 MB costs in this build, measured, not assumed: a
    // sanitizer's shadow memory counts toward the resident set (TSan kept
    // +450 MB), and a fixed 384 MB headroom stopped that child inside the
    // retaining probe, before any test had finished.
    const std::filesystem::path report = OloEngine::Tests::TempFile("kept.tsv");
    const OloEngine::Tests::LaunchResult measured = RunRetainingProbeWithReport(exe, report);
    ASSERT_TRUE(measured.Launched) << measured.Error;
    ASSERT_EQ(measured.ExitCode, 0) << measured.Output;
    std::ifstream in(report);
    std::string header;
    std::string row;
    ASSERT_TRUE(std::getline(in, header) && std::getline(in, row)) << "the measuring child wrote no row";
    const ReportRow kept = ParseReportRow(row);
    ASSERT_GE(kept.PeakMb - kept.StartMb, 200) << row;

    // 128 MB above the kept probe's peak: it fits, and the half gigabyte after
    // it crosses the ceiling a quarter of the way in, well inside the window a
    // 100 ms poll sees.
    const long long headroom = kept.PeakMb - kept.StartMb + 128;
    const OloEngine::Tests::LaunchResult r = OloEngine::Tests::RunProcessWithTimeout(
        exe,
        { "--olo-rss-ceiling-headroom-mb=" + std::to_string(headroom),
          "--gtest_filter=MemoryCeilingProbe.KeepsAQuarterGigabyteUntilExit:"
          "MemoryCeilingProbe.AllocatesAndTouchesHalfAGigabyte" },
        "", 120000);
    ASSERT_TRUE(r.Launched) << r.Error;
    ASSERT_EQ(r.ExitCode, OloEngine::Tests::kMemoryCeilingExitCode) << r.Output;
    const sizet listing = r.Output.find("Finished tests that kept the most memory");
    ASSERT_NE(listing, std::string::npos) << r.Output;
    EXPECT_NE(r.Output.find("MemoryCeilingProbe.KeepsAQuarterGigabyteUntilExit", listing), std::string::npos)
        << r.Output;
}
