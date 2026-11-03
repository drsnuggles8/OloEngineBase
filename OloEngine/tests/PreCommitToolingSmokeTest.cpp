// =============================================================================
// PreCommitToolingSmokeTest.cpp
//
// The pre-commit hook config (per CLAUDE.md) runs Python tooling on every
// commit, including `OloEngine/tests/scripts/generate_test_catalogue.py`.
// A syntax error in any of those scripts blocks every developer's commits
// until someone notices and fixes it — a high-friction failure mode that
// hits the whole team at once.
//
// What this test does
// -------------------
//   For every `.py` file under `OloEngine/tests/scripts/`:
//     Invoke `python -m py_compile <file>`. py_compile parses the source
//     to bytecode without executing it. A non-zero exit code means a
//     syntax error. Imports are not executed by py_compile.
//
// Why a runtime test instead of a pre-commit-time check
// -----------------------------------------------------
//   The pre-commit hook config covers the catalogue-in-sync check but
//   doesn't currently lint the scripts themselves. Adding a hook that
//   runs py_compile is a separate change; this runtime test gives us
//   the safety net immediately and surfaces failures in CI alongside
//   the rest of the suite.
//
// If `python` isn't on PATH the test SKIPs rather than fails — CI
// without Python is a legitimate (if unusual) configuration.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Repo root: OLO_TEST_EDITOR_ROOT points at OloEditor/; the
        // parent is the repo root.
        fs::path RepoRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
        }

        // Probe a Python interpreter. Tries `python`, `python3`, then
        // `py -3` (the Windows launcher). Returns the working invocation
        // string, or empty if none found.
        template<typename RunCommand>
        std::string FindPythonInterpreter(RunCommand&& runCommand)
        {
            const std::array<const char*, 3> candidates{
                "python",
                "python3",
                "py -3",
            };
            for (const auto& cmd : candidates)
            {
                // Redirect output so we don't spam stderr. On Windows
                // `2>nul` works for both cmd and PowerShell; on POSIX
                // `2>/dev/null` works. `> nul` on Windows, `> /dev/null`
                // elsewhere — `2>&1` then redirect-to-file is portable
                // but adds files. We accept the stderr noise for the
                // probe; if the interpreter isn't there, the noise is
                // a single "command not found" line.
                std::string probeCmd = std::string{ cmd } +
                                       " -c \"import sys; sys.exit(sys.version_info[0] != 3)\"";
#ifdef _WIN32
                probeCmd += " >NUL 2>&1";
#else
                probeCmd += " >/dev/null 2>&1";
#endif
                const int rc = runCommand(probeCmd.c_str());
                if (rc == 0)
                {
                    return cmd;
                }
            }
            return {};
        }
    } // namespace

    // Substitute only OS command execution: the existing compile smoke below
    // still exercises a real interpreter. Pin the launcher's Python 3 argument
    // and reject a Python 2 executable that accepts --version successfully.
    TEST(PreCommitToolingSmoke, PythonLauncherFallbackPreservesVersionSelection)
    {
        std::vector<std::string> probes;
        const auto python = FindPythonInterpreter([&probes](const char* command)
                                                  {
                                                     probes.emplace_back(command);
                                                     return probes.back().starts_with("py -3 -c ") ? 0 : 1; });
        EXPECT_EQ(python, "py -3");
        ASSERT_EQ(probes.size(), 3u);
        for (const auto& probe : probes)
        {
            EXPECT_NE(probe.find("sys.version_info[0] != 3"), std::string::npos);
        }
    }

    TEST(PreCommitToolingSmoke, NoPython3InterpreterReturnsEmpty)
    {
        EXPECT_TRUE(FindPythonInterpreter([](const char*)
                                          { return 1; })
                        .empty());
    }

    TEST(PreCommitToolingSmoke, EveryToolingPythonScriptCompiles)
    {
        const std::string python = FindPythonInterpreter(std::system);
        if (python.empty())
        {
            GTEST_SKIP() << "No usable Python interpreter on PATH "
                            "(tried python, python3, py -3) — skipping syntactic check.";
        }

        const fs::path scriptsDir = RepoRoot() / "OloEngine" / "tests" / "scripts";
        ASSERT_TRUE(fs::exists(scriptsDir));

        std::vector<fs::path> scripts;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(scriptsDir, ec))
        {
            if (ec)
                break;
            if (entry.is_regular_file() && entry.path().extension() == ".py")
                scripts.push_back(entry.path());
        }
        ASSERT_FALSE(scripts.empty()) << "No .py files in " << scriptsDir.string();
        std::ranges::sort(scripts);

        struct Failure
        {
            std::string Path;
            int ExitCode;
        };
        std::vector<Failure> failures;

        for (const auto& path : scripts)
        {
            // py_compile parses to bytecode without execution. Returns
            // 0 on success, non-zero on syntax error.
            std::ostringstream cmd;
            cmd << python << " -m py_compile \"" << path.generic_string() << "\"";
#ifdef _WIN32
            cmd << " >NUL 2>&1";
#else
            cmd << " >/dev/null 2>&1";
#endif
            const int rc = std::system(cmd.str().c_str());
            if (rc != 0)
                failures.push_back({ path.generic_string(), rc });
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " Python script(s) failed py_compile:\n";
            for (const auto& f : failures)
            {
                oss << "----\n"
                    << f.Path << "\n"
                    << "    py_compile exit code: " << f.ExitCode << "\n"
                    << "    re-run manually: " << python
                    << " -m py_compile " << f.Path << "\n";
            }
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
