// OLO_TEST_LAYER: meta
//
// The GPU gate's two switches, observed from outside the process.
//
// `--olo-gl-backend=none` must make BOTH gate sites skip -- OLO_ENSURE_GPU_OR_SKIP
// and RendererAttachedTest::SetUp -- with a message that says the skip was
// deliberate, and `--olo-require-gpu` must turn that skip into a failure. Those
// decisions are process-wide (the context is created once and the flags are
// parsed once, in main), so the only honest test re-launches OloEngine-Tests
// with the flag under test and reads the child's gtest XML. Anything done
// in-process would be testing a copy of the logic, not the gate the CI jobs
// actually run through.
//
// Why it matters enough to spawn a process (issue #1015): the self-hosted CI
// box has a GPU, so a sanitizer job routed there runs ~200 GL-gated tests that
// skip on a GitHub-hosted runner. `none` is what makes the box's run mean the
// same thing as the hosted run; `--olo-require-gpu` is what stops the nightly
// GPU-under-sanitizer job from going green by skipping everything. A typo in
// either direction is a run that reports the wrong thing with a green tick.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Rendering/PropertyTests/RenderPropertyTest.h"
#include "TestTempDir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <sys/wait.h> // WIFEXITED / WEXITSTATUS for std::system's return value
#endif

#ifndef OLO_TEST_SELF_EXE
#error "OLO_TEST_SELF_EXE must be defined by OloEngine/tests/CMakeLists.txt"
#endif

namespace fs = std::filesystem;

namespace OloEngine::Tests
{
    namespace
    {
        // One test from each gate site. Both are the cheapest case their site
        // has: a single fullscreen post-process draw, and the renderer
        // bring-up smoke test. With a GPU and --olo-require-gpu they run for
        // real, so they must stay cheap.
        constexpr std::string_view kMacroGatedTest = "VignettePropertyTest.CenterBrighterThanCorners";
        constexpr std::string_view kFixtureGatedTest = "EmptyScene.RendererInitAndTickDoNotCrash";

        struct ChildRun
        {
            int ExitCode = -1;
            std::string Output; // stdout + stderr, interleaved
            std::string Xml;    // the child's --gtest_output report, or empty
        };

        std::string ReadWholeFile(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // Re-launch this binary with `extraFlags` on the two gate-site tests.
        // std::system rather than the fork/CreateProcess pair AppLaunchSmokeTest
        // carries: nothing here needs a timeout or a kill, and the redirect is
        // the whole reason to use the shell.
        ChildRun RunSelf(std::string_view label, std::string_view extraFlags)
        {
            const fs::path dir = TempDir(label);
            const fs::path xmlPath = dir / "child.xml";
            const fs::path outPath = dir / "child.txt";

            std::string cmd;
            cmd += '"';
            cmd += OLO_TEST_SELF_EXE;
            cmd += "\" ";
            cmd += extraFlags;
            cmd += " --gtest_filter=";
            cmd += kMacroGatedTest;
            cmd += ':';
            cmd += kFixtureGatedTest;
            cmd += " \"--gtest_output=xml:";
            cmd += xmlPath.generic_string();
            cmd += "\" >\"";
            cmd += outPath.generic_string();
            cmd += "\" 2>&1";
#if defined(_WIN32)
            // cmd.exe strips the first and last quote of a command line that
            // starts with a quote and holds more than two of them -- which this
            // one does. An outer pair is the documented way to keep the inner
            // ones intact.
            cmd = "\"" + cmd + "\"";
#endif

            ChildRun run;
            const int raw = std::system(cmd.c_str());
#if defined(_WIN32)
            run.ExitCode = raw;
#else
            run.ExitCode = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
            run.Output = ReadWholeFile(outPath);
            if (fs::exists(xmlPath))
            {
                run.Xml = ReadWholeFile(xmlPath);
            }
            return run;
        }

        // gtest writes one <testcase ...> per selected test; a skip nests a
        // <skipped message="..."> and a failure a <failure message="...">.
        sizet Count(std::string_view haystack, std::string_view needle)
        {
            sizet n = 0;
            for (sizet pos = haystack.find(needle); pos != std::string_view::npos; pos = haystack.find(needle, pos + needle.size()))
            {
                ++n;
            }
            return n;
        }

        // How many <skipped>/<failure> elements carry `needle` in their message
        // ATTRIBUTE. gtest repeats every message a second time as the element's
        // CDATA body, so a whole-file substring count reports two per test and
        // says nothing about which element it came from; the attribute is
        // XML-escaped (`'` becomes &apos;) but the needles below contain nothing
        // that escapes.
        sizet CountMessagesContaining(std::string_view xml, std::string_view needle)
        {
            constexpr std::string_view kAttr = " message=\"";
            sizet n = 0;
            for (sizet pos = xml.find(kAttr); pos != std::string_view::npos; pos = xml.find(kAttr, pos + kAttr.size()))
            {
                const sizet begin = pos + kAttr.size();
                const sizet end = xml.find('"', begin);
                if (end == std::string_view::npos)
                    break;
                if (xml.substr(begin, end - begin).find(needle) != std::string_view::npos)
                    ++n;
            }
            return n;
        }
    } // namespace

    TEST(GpuGateSwitch, BackendNoneMakesBothGateSitesSkipAndSaySo)
    {
        const ChildRun run = RunSelf("gpu-gate-none", "--olo-gl-backend=none");

        EXPECT_EQ(run.ExitCode, 0) << run.Output;
        ASSERT_FALSE(run.Xml.empty()) << "child wrote no gtest XML\n"
                                      << run.Output;
        EXPECT_EQ(Count(run.Xml, "<testcase "), 2u) << run.Xml;
        EXPECT_EQ(Count(run.Xml, "<skipped "), 2u) << run.Xml;
        EXPECT_EQ(Count(run.Xml, "<failure "), 0u) << run.Xml;

        // Each site keeps the wording the nightly's "Assert GPU tests actually
        // ran" step matches, and both append the reason.
        EXPECT_EQ(CountMessagesContaining(run.Xml, "No GPU / GL 4.5+ context available"), 1u) << run.Xml;
        EXPECT_EQ(CountMessagesContaining(run.Xml, "no usable GL 4.6 context available"), 1u) << run.Xml;
        EXPECT_EQ(CountMessagesContaining(run.Xml, "--olo-gl-backend=none"), 2u) << run.Xml;
    }

    TEST(GpuGateSwitch, RequireGpuNeverSkips)
    {
        // The child auto-detects its own context; this process cannot know
        // whether one is obtainable, because under ctest on the CI box it was
        // itself launched with --olo-gl-backend=none (IsGpuAvailable() here is
        // a forced false, not "no hardware"). So the invariant under test is
        // the one that holds either way: with --olo-require-gpu a gate-site
        // test RUNS or FAILS, and never skips. Which of the two happened is
        // read off the child's exit code and then checked for consistency.
        const ChildRun run = RunSelf("gpu-gate-require", "--olo-require-gpu");
        ASSERT_FALSE(run.Xml.empty()) << "child wrote no gtest XML\n"
                                      << run.Output;
        EXPECT_EQ(Count(run.Xml, "<testcase "), 2u) << run.Xml;
        EXPECT_EQ(Count(run.Xml, "<skipped "), 0u) << run.Xml;

        if (run.ExitCode == 0)
        {
            // A context existed: both tests ran for real and passed.
            EXPECT_EQ(Count(run.Xml, "<failure "), 0u) << run.Xml;
        }
        else
        {
            // No context (a GitHub-hosted runner): both gate sites turned
            // their skip into a failure that names the flag.
            EXPECT_EQ(Count(run.Xml, "<failure "), 2u) << run.Xml;
            EXPECT_EQ(CountMessagesContaining(run.Xml, "--olo-require-gpu"), 2u) << run.Xml;
        }
    }

    TEST(GpuGateSwitch, RequireGpuWithBackendNoneIsRejectedBeforeAnyTestRuns)
    {
        const ChildRun run = RunSelf("gpu-gate-contradiction", "--olo-gl-backend=none --olo-require-gpu");

        // TestOptions' Fail() exits 2 before InitGoogleTest, so no XML exists.
        EXPECT_EQ(run.ExitCode, 2) << run.Output;
        EXPECT_TRUE(run.Xml.empty()) << run.Xml;
        EXPECT_NE(run.Output.find("contradicts"), std::string::npos) << run.Output;
    }

    TEST(GpuGateSwitch, MisspelledBackendIsRejectedRatherThanAutoDetected)
    {
        // The old behaviour: any unknown spelling fell back to auto-detection,
        // which on a GPU box runs everything the caller meant to skip.
        const ChildRun run = RunSelf("gpu-gate-typo", "--olo-gl-backend=nnoe");

        EXPECT_EQ(run.ExitCode, 2) << run.Output;
        EXPECT_TRUE(run.Xml.empty()) << run.Xml;
        EXPECT_NE(run.Output.find("--olo-gl-backend must be one of"), std::string::npos) << run.Output;
    }
} // namespace OloEngine::Tests
