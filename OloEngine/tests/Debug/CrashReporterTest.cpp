#include "OloEnginePCH.h"
#include "OloEngine/Debug/CrashReporter.h"
#include "OloEngine/Debug/CrashReporterPlatform.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace
{
    // Each test in this file installs / uninstalls the platform handlers so
    // a failure in one test can't leak its sigaction state into the next one
    // and turn an unrelated SIGSEGV into our "best-effort crash report" path.
    class CrashReporterTestFixture : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            std::error_code ec;
            m_TempDir = std::filesystem::temp_directory_path(ec) / "oloengine_crash_reporter_tests";
            std::filesystem::remove_all(m_TempDir, ec);
            std::filesystem::create_directories(m_TempDir, ec);
            ASSERT_FALSE(ec) << "failed to create temp dir: " << ec.message();
        }

        void TearDown() override
        {
            // Unconditionally uninstall — Init may have run during the test, and a
            // half-installed state would leak into the next test in the binary.
            OloEngine::CrashReporter::Shutdown();
            std::error_code ec;
            std::filesystem::remove_all(m_TempDir, ec);
        }

        std::filesystem::path m_TempDir;
    };

    std::string SlurpReport(const std::filesystem::path& dir)
    {
        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.path().extension() == ".txt")
            {
                std::ifstream f(entry.path());
                std::ostringstream ss;
                ss << f.rdbuf();
                return ss.str();
            }
        }
        return {};
    }
} // namespace

TEST_F(CrashReporterTestFixture, CollectSystemInfoIsNonEmpty)
{
    // CollectSystemInfo runs unconditionally on Init's first call. It must
    // return at minimum an OS-identifying line — never an empty string. This
    // guards both the Windows path (RtlGetVersion / GetNativeSystemInfo) and
    // the Linux path (/etc/os-release / uname / sysinfo).
    const std::string info = OloEngine::CrashReporterPlatform::CollectSystemInfo();
    EXPECT_FALSE(info.empty()) << "CollectSystemInfo returned an empty string";

    // First non-empty line should mention the OS family. Loose check — the
    // Linux path emits "Linux" or a distro PRETTY_NAME like "Ubuntu 24.04";
    // the Windows path emits "Windows".
    const bool hasOSMarker =
        info.find("OS:") != std::string::npos ||
        info.find("Windows") != std::string::npos ||
        info.find("Linux") != std::string::npos;
    EXPECT_TRUE(hasOSMarker) << "CollectSystemInfo missing OS marker: " << info;
}

TEST_F(CrashReporterTestFixture, ReportFatalErrorWritesReadableFile)
{
    // ReportFatalError is the synthetic-crash entry point: it writes a report
    // through the same WriteCrashReport pipeline that the signal/SEH handlers
    // use, but the process keeps running. This is the only safe way to drive
    // the report-writing path from a test — actually raising a signal would
    // either bring down the test binary or trigger ASan/UBSan.
    OloEngine::CrashReporter::Init(m_TempDir);
    OloEngine::CrashReporter::SetApplicationInfo("CrashReporterTest", "1.0-test");
    OloEngine::CrashReporter::SetHeadless(true); // never pop a message box from a test
    OloEngine::CrashReporter::SetGPUInfo("Test GPU (no driver)");

    const std::string kSentinel = "Synthetic-Error-Sentinel-3F7B2A1E";
    OloEngine::CrashReporter::ReportFatalError(kSentinel);

    // Exactly one .txt report should have been written into the temp dir.
    int reportCount = 0;
    std::filesystem::path reportPath;
    for (const auto& entry : std::filesystem::directory_iterator(m_TempDir))
    {
        if (entry.path().extension() == ".txt")
        {
            ++reportCount;
            reportPath = entry.path();
        }
    }
    ASSERT_EQ(reportCount, 1) << "expected exactly one crash report file in " << m_TempDir.string();

    const std::string content = SlurpReport(m_TempDir);
    EXPECT_NE(content.find("OloEngine Crash Report"), std::string::npos);
    EXPECT_NE(content.find("CrashReporterTest"), std::string::npos)
        << "report missing application name";
    EXPECT_NE(content.find("1.0-test"), std::string::npos)
        << "report missing application version";
    EXPECT_NE(content.find(kSentinel), std::string::npos)
        << "report missing the fatal-error sentinel";
    EXPECT_NE(content.find("Test GPU (no driver)"), std::string::npos)
        << "report missing GPU info";
    EXPECT_NE(content.find("--- System Info ---"), std::string::npos);
}

TEST_F(CrashReporterTestFixture, WriteMiniDumpReturnsFalseWithoutContext)
{
    // The minidump path is platform-specific: on Windows it expects an
    // EXCEPTION_POINTERS* in platformContext; on Linux it expects our
    // LinuxSignalContext struct. Neither path produces a usable dump when
    // handed a path to a directory that doesn't exist — both should fail
    // gracefully (return false) rather than crash.
    const auto bogusPath = m_TempDir / "does_not_exist_dir" / "out.dmp";
    EXPECT_FALSE(OloEngine::CrashReporterPlatform::WriteMiniDump(bogusPath, nullptr))
        << "WriteMiniDump should fail when the parent directory is missing";
}

TEST_F(CrashReporterTestFixture, EmitToDebugOutputDoesNotCrash)
{
    // Smoke test only — EmitToDebugOutput goes to OutputDebugStringA on Windows
    // (no observable side effect from a test) and stderr on Linux. We can't
    // assert on the output but we can ensure the call returns normally with
    // both empty and non-empty strings.
    OloEngine::CrashReporterPlatform::EmitToDebugOutput("");
    OloEngine::CrashReporterPlatform::EmitToDebugOutput("crash-reporter-test-line\n");
    SUCCEED();
}

TEST_F(CrashReporterTestFixture, InstallUninstallRoundTrips)
{
    // Multiple Init/Shutdown cycles must not leak handler state. The Linux
    // implementation keeps an std::array of PreviousAction entries and an
    // atomic flag; a second Init after a clean Shutdown should re-install
    // cleanly. A leak here would manifest as the second Init silently
    // failing to install handlers (which can't be observed without crashing
    // the process), so this test only proves the API is re-entrant.
    OloEngine::CrashReporter::Init(m_TempDir);
    OloEngine::CrashReporter::Shutdown();
    OloEngine::CrashReporter::Init(m_TempDir);
    OloEngine::CrashReporter::Shutdown();
    SUCCEED();
}
