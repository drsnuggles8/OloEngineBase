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

#include <chrono>
#include <cstring>
#include <memory>
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
} // namespace

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

    // A ceiling relative to THIS process's resident set, not an absolute number:
    // the child is the same binary with the same start-up footprint (which
    // under a sanitizer is hundreds of MB before any test runs), and the probe
    // adds 512 MB on top, so +128 MB is crossed while the probe is running and
    // not during start-up.
    const u64 baselineMb = OloEngine::Tests::CurrentResidentBytes() / kMiB;
    ASSERT_GT(baselineMb, 0u) << "CurrentResidentBytes() reports nothing on this platform";
    const u64 ceilingMb = baselineMb + 128;

    const std::vector<std::string> args = {
        "--olo-rss-ceiling-mb=" + std::to_string(ceilingMb),
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
